#include "ParallelTick.h"
#include "Config.h"
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <ll/api/chrono/GameChrono.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <mc/world/actor/Actor.h>
#include <mc/legacy/ActorUniqueID.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include <unordered_map>
#include <optional>

namespace parallel_tick {

using namespace ll::chrono_literals;

// ============================================================================
// ThreadPool 实现
// ============================================================================

struct ThreadPool::Impl {
    std::vector<std::thread>          workers;
    std::queue<std::function<void()>> tasks;
    std::mutex                        queueMutex;
    std::condition_variable            cv;
    std::condition_variable            cvDone;
    std::atomic<int>                   activeTasks{0};
    bool                               stop{false};

    ~Impl() {
        {
            std::lock_guard lk(queueMutex);
            stop = true;
        }
        cv.notify_all();
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
    }
};

ThreadPool::ThreadPool(size_t threads) : mImpl(std::make_unique<Impl>()) {
    for (size_t i = 0; i < threads; ++i) {
        mImpl->workers.emplace_back([this] {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock lk(mImpl->queueMutex);
                    mImpl->cv.wait(lk, [this] {
                        return mImpl->stop || !mImpl->tasks.empty();
                    });
                    if (mImpl->stop && mImpl->tasks.empty()) return;
                    task = std::move(mImpl->tasks.front());
                    mImpl->tasks.pop();
                }

                task();

                if (--mImpl->activeTasks == 0) {
                    std::lock_guard lk(mImpl->queueMutex);
                    mImpl->cvDone.notify_all();
                }
            }
        });
    }
}

ThreadPool::~ThreadPool() = default;

void ThreadPool::submit(std::function<void()> task) {
    ++mImpl->activeTasks;
    {
        std::lock_guard lk(mImpl->queueMutex);
        mImpl->tasks.push(std::move(task));
    }
    mImpl->cv.notify_one();
}

void ThreadPool::waitAll() {
    std::unique_lock lk(mImpl->queueMutex);
    mImpl->cvDone.wait(lk, [this] {
        return mImpl->activeTasks.load() == 0 && mImpl->tasks.empty();
    });
}

bool ThreadPool::waitAllFor(std::chrono::milliseconds timeout) {
    std::unique_lock lk(mImpl->queueMutex);
    return mImpl->cvDone.wait_for(lk, timeout, [this] {
        return mImpl->activeTasks.load() == 0 && mImpl->tasks.empty();
    });
}

size_t ThreadPool::threadCount() const {
    return mImpl->workers.size();
}

// ============================================================================
// ParallelTick 内部实现
// ============================================================================

struct CrashedEntry {
    std::chrono::steady_clock::time_point timestamp;
};

struct ParallelTick::Impl {
    ll::mod::NativeMod* self{nullptr};
    std::unique_ptr<ThreadPool> pool;
    std::atomic<bool>           parallelPhase{false};

    // 崩溃黑名单（基于 ActorUniqueID）
    std::unordered_map<ActorUniqueID, CrashedEntry> crashedActors;
    std::mutex                                       crashMtx;

    // 统计
    struct {
        std::atomic<size_t> totalMobsParalleled{0};
        std::atomic<size_t> totalBatches{0};
        std::atomic<size_t> crashedActors{0};
    } stats;

    // 统计输出任务
    std::atomic<bool>                 statsRunning{false};
    std::optional<ll::coro::CoroTask<>> statsTask;

    // 最后一次清理的 tick 计数（用于周期性清理）
    std::atomic<int> lastCleanupTick{0};
};

// ============================================================================
// 单例访问
// ============================================================================

ParallelTick& ParallelTick::getInstance() {
    static ParallelTick inst;
    return inst;
}

// ============================================================================
// 生命周期
// ============================================================================

bool ParallelTick::load(ll::mod::NativeMod& self) {
    mImpl = std::make_unique<Impl>();
    mImpl->self = &self;

    // 加载配置
    if (!loadConfig()) {
        self.getLogger().error("Failed to load config, using defaults");
    }

    const auto& cfg = getConfig();
    size_t nThreads = cfg.threadCount > 0
        ? static_cast<size_t>(cfg.threadCount)
        : std::max(1u, std::thread::hardware_concurrency() - 1);

    mImpl->pool = std::make_unique<ThreadPool>(nThreads);

    self.getLogger().info(
        "[ParallelTick] loaded, threads={}, maxPerTask={}",
        nThreads, cfg.maxEntitiesPerTask
    );

    registerECSHooks();

    // 启动统计输出任务（如果启用）
    if (cfg.statsEnabled) {
        startStatsTask();
    }

    return true;
}

bool ParallelTick::unload() {
    unregisterECSHooks();

    // 停止统计输出任务
    stopStatsTask();

    saveConfig(); // 可选保存

    auto& st = mImpl->stats;
    mImpl->self->getLogger().info(
        "[ParallelTick] unloaded — totalMobs={} totalBatches={} crashed={}",
        st.totalMobsParalleled.load(),
        st.totalBatches.load(),
        st.crashedActors.load()
    );

    mImpl.reset();
    return true;
}

// ============================================================================
// 访问器
// ============================================================================

ll::mod::NativeMod& ParallelTick::getSelf() {
    return *mImpl->self;
}

const Config& ParallelTick::getConfig() const {
    return parallel_tick::getConfig();
}

ThreadPool& ParallelTick::getPool() {
    return *mImpl->pool;
}

// ============================================================================
// 安全检查
// ============================================================================

bool ParallelTick::isActorSafeToTick(Actor* a) const {
    if (!a) return false;
    if (isCrashed(a)) return false;
    return true;
}

// ============================================================================
// 崩溃黑名单
// ============================================================================

void ParallelTick::markCrashed(Actor* a) {
    if (!a) return;
    ActorUniqueID id = a->getOrCreateUniqueID();
    {
        std::lock_guard lk(mImpl->crashMtx);
        mImpl->crashedActors[id] = { std::chrono::steady_clock::now() };
        ++mImpl->stats.crashedActors;
    }
    if (getConfig().debug) {
        getSelf().getLogger().debug("Actor {} marked as crashed", id.rawID);
    }
}

bool ParallelTick::isCrashed(Actor* a) const {
    if (!a) return false;
    ActorUniqueID id = a->getOrCreateUniqueID();
    std::lock_guard lk(mImpl->crashMtx);
    return mImpl->crashedActors.find(id) != mImpl->crashedActors.end();
}

void ParallelTick::cleanupCrashedList() {
    auto now = std::chrono::steady_clock::now();
    int maxAgeTicks = getConfig().maxExpiredAge;
    auto maxAgeMs = std::chrono::milliseconds(maxAgeTicks * 50);

    std::lock_guard lk(mImpl->crashMtx);
    for (auto it = mImpl->crashedActors.begin(); it != mImpl->crashedActors.end(); ) {
        auto age = now - it->second.timestamp;
        if (age > maxAgeMs) {
            it = mImpl->crashedActors.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================================
// 并行阶段标志
// ============================================================================

void ParallelTick::setParallelPhase(bool v) {
    mImpl->parallelPhase.store(v, std::memory_order_release);
}

bool ParallelTick::isParallelPhase() const {
    return mImpl->parallelPhase.load(std::memory_order_acquire);
}

// ============================================================================
// 统计
// ============================================================================

void ParallelTick::addStats(size_t mobs, size_t batches) {
    mImpl->stats.totalMobsParalleled += mobs;
    mImpl->stats.totalBatches        += batches;
}

void ParallelTick::printStats() {
    auto& st = mImpl->stats;
    getSelf().getLogger().info(
        "[ParallelTick Stats] totalMobs={} totalBatches={} crashed={}",
        st.totalMobsParalleled.load(),
        st.totalBatches.load(),
        st.crashedActors.load()
    );
}

// ============================================================================
// 统计输出任务（协程）
// ============================================================================

void ParallelTick::startStatsTask() {
    if (!mImpl || mImpl->statsRunning.load()) return;
    mImpl->statsRunning = true;

    mImpl->statsTask = ll::coro::keepThis([this]() -> ll::coro::CoroTask<> {
        while (mImpl && mImpl->statsRunning.load()) {
            co_await 100_tick; // 5秒 = 100 tick (50ms/tick)
            if (!mImpl || !mImpl->statsRunning.load()) break;
            printStats();
        }
        co_return;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

void ParallelTick::stopStatsTask() {
    if (mImpl) {
        mImpl->statsRunning = false;
        mImpl->statsTask.reset(); // 析构协程，自动取消
    }
}

} // namespace parallel_tick
