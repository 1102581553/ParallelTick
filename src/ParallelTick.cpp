#include "ParallelTick.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/Config.h>

#include <mc/world/level/Level.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/BlockSource.h>
#include <mc/deps/ecs/gamerefs_entity/EntityContext.h>
#include <mc/deps/ecs/WeakEntityRef.h>
#include <mc/legacy/ActorRuntimeID.h>

#include <windows.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <cstdio>
#include <exception>
#include <malloc.h>

using LevelRemoveByActor   = ::OwnerPtr<::EntityContext>(Level::*)(::Actor&);
using LevelRemoveByWeakRef = ::OwnerPtr<::EntityContext>(Level::*)(::WeakEntityRef);

static LONG actorTickSEHFilter(unsigned int code) {
    if (code == 0xE06D7363u)
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_EXECUTE_HANDLER;
}

static int tickActorSEH(Actor* actor, BlockSource& region) {
    __try {
        actor->tick(region);
        return 0;
    } __except (actorTickSEHFilter(GetExceptionCode())) {
        DWORD code = GetExceptionCode();
        if (code == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();
        return static_cast<int>(code);
    }
}

static int tickActorSafe(Actor* actor, BlockSource& region) {
    try {
        return tickActorSEH(actor, region);
    } catch (const std::exception& e) {
        fprintf(stderr,
            "[ParallelTick] C++ exception actor=%p: %s\n",
            (void*)actor, e.what());
        return -1;
    } catch (...) {
        fprintf(stderr,
            "[ParallelTick] Unknown C++ exception actor=%p\n",
            (void*)actor);
        return -2;
    }
}

namespace parallel_tick {

static std::string nowStr() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    tm buf;
    localtime_s(&buf, &t);
    std::ostringstream ss;
    ss << std::put_time(&buf, "%H:%M:%S");
    return ss.str();
}

static Actor* weakToActor(WeakEntityRef const& ref) {
    if (auto s = ref.lock())
        if (auto* ctx = s.operator->())
            return Actor::tryGetFromEntity(*ctx, false);
    return nullptr;
}

// ── 单例 ──

ParallelTick& ParallelTick::getInstance() {
    static ParallelTick inst;
    return inst;
}

// ── 统计 ──

void ParallelTick::startStatsTask() {
    if (mStatsTaskRunning.exchange(true)) return;
    ll::coro::keepThis([this]() -> ll::coro::CoroTask<> {
        while (mStatsTaskRunning.load()) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([this] {
                if (!mConfig.stats) return;
                size_t tot = mTotalStats.exchange(0);
                size_t dm  = mDimStats.exchange(0);
                size_t cr  = mCrashStats.load();
                if (tot > 0)
                    getSelf().getLogger().info(
                        "[{}][Stats] Entities={} Dims={} Crashed={}",
                        nowStr(), tot, dm, cr);
            });
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

void ParallelTick::stopStatsTask() {
    mStatsTaskRunning.store(false);
}

// ── 生命周期 ──

bool ParallelTick::load() {
    auto path = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(mConfig, path))
        ll::config::saveConfig(mConfig, path);
    getSelf().getLogger().info(
        "[{}][load] enabled={} debug={} threads={} target={}ms",
        nowStr(), mConfig.enabled, mConfig.debug,
        mConfig.threadCount, mConfig.targetTickMs);
    return true;
}

bool ParallelTick::enable() {
    int n = mConfig.threadCount;
    if (n <= 0)
        n = std::min(3u, std::max(1u, std::thread::hardware_concurrency() - 1));
    n = std::min(n, 3);
    mPool = std::make_unique<FixedThreadPool>(n, 8 * 1024 * 1024);
    registerHooks();
    getSelf().getLogger().info("[{}][enable] threads={}", nowStr(), n);
    if (mConfig.stats || mConfig.debug) startStatsTask();
    return true;
}

bool ParallelTick::disable() {
    unregisterHooks();
    stopStatsTask();
    mPool.reset();
    getSelf().getLogger().info("[{}][disable] Done", nowStr());
    return true;
}

// ================================================================
//  Hooks
// ================================================================

// ── removeEntity ──

LL_TYPE_INSTANCE_HOOK(
    ParallelRemoveActorLock,
    ll::memory::HookPriority::Normal,
    Level,
    static_cast<LevelRemoveByActor>(&Level::$removeEntity),
    ::OwnerPtr<::EntityContext>,
    ::Actor& actor
) {
    auto& pt = ParallelTick::getInstance();
    pt.onActorRemoved(&actor);
    if (pt.isParallelPhase()) {
        std::lock_guard<std::mutex> lk(pt.getLevelMutex());
        return origin(actor);
    }
    return origin(actor);
}

LL_TYPE_INSTANCE_HOOK(
    ParallelRemoveWeakRefLock,
    ll::memory::HookPriority::Normal,
    Level,
    static_cast<LevelRemoveByWeakRef>(&Level::$removeEntity),
    ::OwnerPtr<::EntityContext>,
    ::WeakEntityRef entityRef
) {
    auto& pt = ParallelTick::getInstance();
    if (Actor* a = weakToActor(entityRef))
        pt.onActorRemoved(a);
    if (pt.isParallelPhase()) {
        std::lock_guard<std::mutex> lk(pt.getLevelMutex());
        return origin(std::move(entityRef));
    }
    return origin(std::move(entityRef));
}

// ── Actor::tick ──

LL_TYPE_INSTANCE_HOOK(
    ParallelActorTickHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::tick,
    bool,
    ::BlockSource& region
) {
    auto& pt         = ParallelTick::getInstance();
    const auto& conf = pt.getConfig();

    if (!conf.enabled || !pt.isCollecting())
        return origin(region);

    if (this->isPlayer() || this->isSimulatedPlayer())
        return origin(region);

    if (pt.isPermanentlyCrashed(this))
        return true;

    pt.collectActor(this, region);
    return true;
}

// ── Level::$tick ──

LL_TYPE_INSTANCE_HOOK(
    ParallelLevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    static thread_local bool inTick = false;
    if (inTick) {
        origin();
        return;
    }
    inTick = true;

    auto& pt         = ParallelTick::getInstance();
    const auto& conf = pt.getConfig();
    auto  tickStart  = std::chrono::steady_clock::now();

    if (!conf.enabled) {
        origin();
        inTick = false;
        return;
    }

    // ─── 阶段 1: 收集 ───
    pt.setCollecting(true);
    origin();
    pt.setCollecting(false);

    auto list = pt.takeQueue();
    if (list.empty()) {
        pt.clearAll();
        inTick = false;
        return;
    }

    // ─── 阶段 2: 过滤 ───
    std::vector<ActorTickEntry> snapshot;
    snapshot.reserve(list.size());
    for (auto& e : list) {
        if (!e.actor) continue;
        if (!pt.isActorSafeToTick(e.actor)) continue;
        snapshot.push_back(e);
    }

    if (conf.debug)
        pt.getSelf().getLogger().info(
            "[{}][Filter] {}/{} passed", nowStr(),
            snapshot.size(), list.size());

    if (snapshot.empty()) {
        pt.clearAll();
        inTick = false;
        return;
    }

    // ─── 阶段 3: 按 BlockSource(维度) 分组 ───
    //
    // BlockSource* 是维度的唯一标识
    // 同一 BlockSource 的所有实体将在同一个线程内串行 tick
    // 不同 BlockSource 之间并行
    //
    std::unordered_map<BlockSource*, std::vector<ActorTickEntry>> dimMap;
    for (auto& e : snapshot)
        dimMap[e.region].push_back(e);

    size_t dimCount = dimMap.size();
    pt.addStats(snapshot.size(), dimCount);

    if (conf.debug)
        pt.getSelf().getLogger().info(
            "[{}][Dims] {} entities across {} dimensions",
            nowStr(), snapshot.size(), dimCount);

    // ─── 阶段 4: 维度级并行 ───
    //
    // 安全性保证:
    //   1. 同维度串行 → 同一 BlockSource 不会被并发访问
    //   2. 不同维度的 BlockSource 内存独立 → 可安全并行
    //   3. Level 全局操作通过 LevelMutex 串行
    //   4. 实体 tick 内部不会跨维度访问其他 BlockSource
    //
    // 最差情况: 只有 1 个维度有实体 → 退化为串行，无额外开销
    // 最好情况: 3 个维度都有实体 → 3 路并行
    //
    pt.setParallelPhase(true);
    auto& pool = pt.getPool();
    auto  timeout = std::chrono::milliseconds(
        conf.actorTickTimeoutMs > 0 ? conf.actorTickTimeoutMs : 30000);

    for (auto& [bs, entries] : dimMap) {
        pool.submit(
            [&pt, &conf, bs, entries = std::move(entries)]()
        {
            for (size_t i = 0; i < entries.size(); ++i) {
                auto& e = entries[i];

                // 最终存活检查（锁内，消除 TOCTOU）
                if (!pt.isActorSafeToTick(e.actor)) continue;

                // 可能在前一个 tick 中被移除
                if (!e.actor->isInWorld()) continue;

                if (conf.debug)
                    pt.getSelf().getLogger().info(
                        "[{}][Tick] bs={:p} [{}/{}] actor={:p} type={}",
                        nowStr(), (void*)bs, i + 1, entries.size(),
                        (void*)e.actor,
                        static_cast<int>(e.actor->getEntityTypeId()));

                auto start = std::chrono::steady_clock::now();
                int ex = tickActorSafe(e.actor, *bs);
                auto ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();

                if (ex != 0) {
                    // 崩溃：永久冻结此实体
                    const char* exType =
                        (ex == -1) ? "C++Exception" :
                        (ex == -2) ? "UnknownC++Exception" :
                                     "HardwareSEH";

                    pt.markCrashed(e.actor);

                    pt.getSelf().getLogger().error(
                        "[{}][Crash] {} 0x{:08X} actor={:p} type={} "
                        "{}ms — permanently frozen",
                        nowStr(), exType,
                        static_cast<unsigned>(ex & 0xFFFFFFFF),
                        (void*)e.actor,
                        static_cast<int>(e.actor->getEntityTypeId()),
                        ms);

                    continue;
                }

                if (ms > 100)
                    pt.getSelf().getLogger().warn(
                        "[{}][SlowTick] actor={:p} type={} {}ms",
                        nowStr(), (void*)e.actor,
                        static_cast<int>(e.actor->getEntityTypeId()),
                        ms);
            }
        });
    }

    // ─── 阶段 5: 等待所有维度完成 ───
    bool ok = pool.waitAllFor(timeout);
    if (!ok) {
        pt.getSelf().getLogger().error(
            "[{}][Timeout] Phase not done after {}ms, pending={}",
            nowStr(), timeout.count(), pool.pendingCount());

        // 二次等待
        bool ok2 = pool.waitAllFor(timeout * 2);
        if (!ok2)
            pt.getSelf().getLogger().error(
                "[{}][Timeout] Extended wait failed, pending={}. Continuing.",
                nowStr(), pool.pendingCount());
    }

    pt.setParallelPhase(false);

    // ─── 阶段 6: 清理 ───
    pt.clearAll();

    // ─── 阶段 7: 日志 ───
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tickStart).count();

    if (conf.debug)
        pt.getSelf().getLogger().info(
            "[{}][LevelTick] {}ms, {} entities, {} dims",
            nowStr(), elapsed, snapshot.size(), dimCount);

    inTick = false;
}

// ── 注册/注销 ──

void registerHooks() {
    ParallelRemoveActorLock::hook();
    ParallelRemoveWeakRefLock::hook();
    ParallelActorTickHook::hook();
    ParallelLevelTickHook::hook();
}

void unregisterHooks() {
    ParallelRemoveActorLock::unhook();
    ParallelRemoveWeakRefLock::unhook();
    ParallelActorTickHook::unhook();
    ParallelLevelTickHook::unhook();
}

LL_REGISTER_MOD(parallel_tick::ParallelTick,
                parallel_tick::ParallelTick::getInstance());

} // namespace parallel_tick
