// ParallelTick.cpp
// ParallelTick 单例 + ThreadPool 实现
// ─────────────────────────────────────────────────────────────────

#include "ParallelTick.h"

#include <ll/api/mod/NativeMod.h>
#include <mc/world/actor/Actor.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <vector>

namespace parallel_tick {

// ══════════════════════════════════════════════════════════════════
//  ThreadPool
// ══════════════════════════════════════════════════════════════════

struct ThreadPool::Impl {
    std::vector<std::thread>          workers;
    std::queue<std::function<void()>> tasks;
    std::mutex                        mtx;
    std::condition_variable           cv;
    std::condition_variable           cvDone;
    std::atomic<int>                  activeTasks{0};
    bool                              stop{false};

    explicit Impl(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lk(mtx);
                        cv.wait(lk, [this] {
                            return stop || !tasks.empty();
                        });
                        if (stop && tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                    if (--activeTasks == 0)
                        cvDone.notify_all();
                }
            });
        }
    }

    ~Impl() {
        {
            std::lock_guard lk(mtx);
            stop = true;
        }
        cv.notify_all();
        for (auto& w : workers) w.join();
    }
};

ThreadPool::ThreadPool(size_t threads)
    : mImpl(std::make_unique<Impl>(threads)) {}

ThreadPool::~ThreadPool() = default;

void ThreadPool::submit(std::function<void()> task) {
    ++mImpl->activeTasks;
    {
        std::lock_guard lk(mImpl->mtx);
        mImpl->tasks.push(std::move(task));
    }
    mImpl->cv.notify_one();
}

void ThreadPool::waitAll() {
    std::unique_lock lk(mImpl->mtx);
    mImpl->cvDone.wait(lk, [this] {
        return mImpl->activeTasks.load() == 0 && mImpl->tasks.empty();
    });
}

bool ThreadPool::waitAllFor(std::chrono::milliseconds timeout) {
    std::unique_lock lk(mImpl->mtx);
    return mImpl->cvDone.wait_for(lk, timeout, [this] {
        return mImpl->activeTasks.load() == 0 && mImpl->tasks.empty();
    });
}

size_t ThreadPool::threadCount() const {
    return mImpl->workers.size();
}

// ══════════════════════════════════════════════════════════════════
//  ParallelTick::Impl
// ══════════════════════════════════════════════════════════════════

struct ParallelTick::Impl {
    ll::plugin::NativePlugin* self{nullptr};
    Config                    config;
    Stats                     stats;
    std::unique_ptr<ThreadPool> pool;

    std::atomic<bool>       parallelPhase{false};

    // 崩溃黑名单（指针集合，插件生命周期内有效）
    std::set<const Actor*>  crashedActors;
    std::mutex              crashMtx;
};

// ══════════════════════════════════════════════════════════════════
//  单例访问
// ══════════════════════════════════════════════════════════════════

ParallelTick& ParallelTick::getInstance() {
    static ParallelTick inst;
    return inst;
}

// ══════════════════════════════════════════════════════════════════
//  生命周期
// ══════════════════════════════════════════════════════════════════

bool ParallelTick::load(ll::plugin::NativePlugin& self) {
    mImpl         = std::make_unique<Impl>();
    mImpl->self   = &self;

    // TODO: 从文件读取 config（此处使用默认值）
    // mImpl->config = loadConfigFromFile("config/parallel_tick.json");

    size_t nThreads = mImpl->config.threadCount > 0
        ? (size_t)mImpl->config.threadCount
        : std::max((size_t)1, (size_t)std::thread::hardware_concurrency() - 1);

    mImpl->pool = std::make_unique<ThreadPool>(nThreads);

    self.getLogger().info(
        "[ParallelTick] loaded, threads={}, maxPerTask={}",
        nThreads, mImpl->config.maxEntitiesPerTask
    );

    registerECSHooks();
    return true;
}

bool ParallelTick::unload() {
    unregisterECSHooks();

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

// ══════════════════════════════════════════════════════════════════
//  访问器
// ══════════════════════════════════════════════════════════════════

ll::plugin::NativePlugin& ParallelTick::getSelf() {
    return *mImpl->self;
}
const Config& ParallelTick::getConfig() const {
    return mImpl->config;
}
ThreadPool& ParallelTick::getPool() {
    return *mImpl->pool;
}

// ══════════════════════════════════════════════════════════════════
//  安全检查
// ══════════════════════════════════════════════════════════════════

bool ParallelTick::isActorSafeToTick(const Actor* a) const {
    if (!a) return false;
    if (isCrashed(a)) return false;
    // 可扩展：检查实体是否正在被销毁、是否在有效 chunk 中等
    return true;
}

// ══════════════════════════════════════════════════════════════════
//  崩溃黑名单
// ══════════════════════════════════════════════════════════════════

void ParallelTick::markCrashed(const Actor* a) {
    std::lock_guard lk(mImpl->crashMtx);
    mImpl->crashedActors.insert(a);
    ++mImpl->stats.crashedActors;
}

bool ParallelTick::isCrashed(const Actor* a) const {
    std::lock_guard lk(mImpl->crashMtx);
    return mImpl->crashedActors.count(a) > 0;
}

// ══════════════════════════════════════════════════════════════════
//  并行阶段标志
// ══════════════════════════════════════════════════════════════════

void ParallelTick::setParallelPhase(bool v) {
    mImpl->parallelPhase.store(v, std::memory_order_release);
}
bool ParallelTick::isParallelPhase() const {
    return mImpl->parallelPhase.load(std::memory_order_acquire);
}

// ══════════════════════════════════════════════════════════════════
//  统计
// ══════════════════════════════════════════════════════════════════

void ParallelTick::addStats(size_t mobs, size_t batches) {
    mImpl->stats.totalMobsParalleled += mobs;
    mImpl->stats.totalBatches        += batches;
}
Stats& ParallelTick::getStats() {
    return mImpl->stats;
}

} // namespace parallel_tick
