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
#include <cmath>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <queue>
#include <unordered_map>
#include <cstdio>
#include <array>
#include <exception>
#include <malloc.h>

using LevelRemoveByActor   = ::OwnerPtr<::EntityContext>(Level::*)(::Actor&);
using LevelRemoveByWeakRef = ::OwnerPtr<::EntityContext>(Level::*)(::WeakEntityRef);

// ================================================================
//  多层防崩溃 tick
//
//  第1层: C++ try-catch    — 捕获标准异常
//  第2层: SEH __try        — 捕获硬件异常(AV/除零等)
//  第3层: 栈溢出恢复        — EXCEPTION_STACK_OVERFLOW 后 _resetstkoflw
//
//  返回值:
//    0  = 正常
//    >0 = SEH 异常代码
//    -1 = C++ std::exception
//    -2 = 未知 C++ 异常
// ================================================================
static int tickActorSEH(Actor* actor, BlockSource& region) {
    __try {
        actor->tick(region);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        if (code == EXCEPTION_STACK_OVERFLOW) {
            _resetstkoflw();
        }
        return static_cast<int>(code);
    }
}

static int tickActorSafe(Actor* actor, BlockSource& region) {
    try {
        return tickActorSEH(actor, region);
    } catch (const std::exception& e) {
        fprintf(stderr,
            "[ParallelTick] C++ exception in actor %p: %s\n",
            (void*)actor, e.what());
        return -1;
    } catch (...) {
        fprintf(stderr,
            "[ParallelTick] Unknown C++ exception in actor %p\n",
            (void*)actor);
        return -2;
    }
}

namespace parallel_tick {

// ── 工具 ──
static std::string currentTimeString() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    tm buf;
    localtime_s(&buf, &t);
    std::ostringstream ss;
    ss << std::put_time(&buf, "%H:%M:%S");
    return ss.str();
}

static Actor* tryGetActorFromWeakRef(WeakEntityRef const& ref) {
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

// ── 统计协程 ──
void ParallelTick::startStatsTask() {
    if (mStatsTaskRunning.exchange(true)) return;
    ll::coro::keepThis([this]() -> ll::coro::CoroTask<> {
        while (mStatsTaskRunning.load()) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([this] {
                if (!mConfig.stats) return;
                size_t tot = mTotalStats.exchange(0);
                size_t ph  = mPhaseStats.exchange(0);
                size_t cr  = mCrashStats.load();
                if (tot > 0)
                    getSelf().getLogger().info(
                        "[{}][Stats] Entities={} Tasks={} CrashedTotal={}",
                        currentTimeString(), tot, ph, cr);
            });
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

void ParallelTick::stopStatsTask() { mStatsTaskRunning.store(false); }

// ── 生命周期 ──
bool ParallelTick::load() {
    auto path = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(mConfig, path))
        ll::config::saveConfig(mConfig, path);
    mAutoMaxEntities.store(mConfig.maxEntitiesPerTask);
    getSelf().getLogger().info(
        "[{}][load] enabled={} debug={} threads={} maxE={} grid={} "
        "auto={} target={}ms timeout={}ms killCrashed={}",
        currentTimeString(), mConfig.enabled, mConfig.debug,
        mConfig.threadCount, mConfig.maxEntitiesPerTask,
        mConfig.gridSizeBase, mConfig.autoAdjust, mConfig.targetTickMs,
        mConfig.actorTickTimeoutMs, mConfig.killCrashedActors);
    return true;
}

bool ParallelTick::enable() {
    int n = mConfig.threadCount;
    if (n <= 0) n = std::max(1u, std::thread::hardware_concurrency() - 1);
    mPool = std::make_unique<FixedThreadPool>(n, 8 * 1024 * 1024);
    registerHooks();
    getSelf().getLogger().info(
        "[{}][enable] threads={}", currentTimeString(), n);
    if (mConfig.stats || mConfig.debug) startStatsTask();
    return true;
}

bool ParallelTick::disable() {
    unregisterHooks();
    stopStatsTask();
    mPool.reset();
    getSelf().getLogger().info(
        "[{}][disable] Done", currentTimeString());
    return true;
}

// ================================================================
//  Hook 实现
// ================================================================

// ── removeEntity（两个重载）──
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
    pt.clearCrashRecord(&actor);

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
    if (Actor* a = tryGetActorFromWeakRef(entityRef)) {
        pt.onActorRemoved(a);
        pt.clearCrashRecord(a);
    }

    if (pt.isParallelPhase()) {
        std::lock_guard<std::mutex> lk(pt.getLevelMutex());
        return origin(std::move(entityRef));
    }
    return origin(std::move(entityRef));
}

// ── Actor::tick Hook ──
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

    if (conf.debug)
        pt.getSelf().getLogger().info(
            "[{}][Collect] actor={:p} type={}",
            currentTimeString(), (void*)this,
            static_cast<int>(this->getEntityTypeId()));

    return true;
}

// ── Level::$tick Hook ──
//
// 核心设计：4-色棋盘分相并行
//
//   网格坐标 (gx, gz) 着色: color = (gx%2)*2 + (gz%2)，共 4 色
//   同色网格在 X/Z 方向间距 ≥ 2*gridSizeBase
//   gridSizeBase=64 → 同色间距 ≥ 128 格，远超任何交互范围
//
//   Phase 0 → waitAll → Phase 1 → waitAll → Phase 2 → waitAll → Phase 3 → waitAll
//
//   同 Phase 内安全并行：
//     - 不访问相同区块（空间距离 ≥ 128 格）
//     - Level 全局变更通过 LevelMutex 串行
//
LL_TYPE_INSTANCE_HOOK(
    ParallelLevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    // 防重入
    static thread_local bool inTick = false;
    if (inTick) { origin(); return; }
    inTick = true;

    auto& pt         = ParallelTick::getInstance();
    const auto& conf = pt.getConfig();
    auto  tickStart  = std::chrono::steady_clock::now();

    if (!conf.enabled) {
        origin();
        inTick = false;
        return;
    }

    // ════════════════════════════════════════════════════════
    //  阶段 0：主线程处理上一 tick 积累的延迟 kill
    //  必须在 origin() 之前、非并行阶段执行
    // ════════════════════════════════════════════════════════
    if (conf.killCrashedActors) {
        auto kills = pt.takePendingKills();
        for (Actor* a : kills) {
            try {
                if (a && a->isInWorld()) {
                    pt.getSelf().getLogger().warn(
                        "[{}][Kill] Removing crashed actor={:p} type={}",
                        currentTimeString(), (void*)a,
                        static_cast<int>(a->getEntityTypeId()));
                    a->remove();
                }
            } catch (...) {
                pt.getSelf().getLogger().error(
                    "[Kill] Failed to remove actor={:p}", (void*)a);
            }
        }
    }

    // ════════════════════════════════════════════════════════
    //  阶段 1：收集实体
    //  origin() 执行 MCBE 所有逻辑，Actor::tick Hook 拦截入队
    // ════════════════════════════════════════════════════════
    pt.setCollecting(true);
    origin();
    pt.setCollecting(false);

    auto list = pt.takeQueue();

    if (list.empty()) {
        pt.clearAll();
        inTick = false;
        return;
    }

    // ════════════════════════════════════════════════════════
    //  阶段 2：过滤已移除 / 已崩溃的实体
    // ════════════════════════════════════════════════════════
    std::vector<ActorTickEntry> snapshot;
    snapshot.reserve(list.size());
    for (auto& e : list) {
        if (!e.actor) continue;
        if (!pt.isActorSafeToTick(e.actor)) continue;
        if (pt.isPermanentlyCrashed(e.actor)) continue;
        snapshot.push_back(e);
    }

    if (conf.debug)
        pt.getSelf().getLogger().info(
            "[{}][Filter] {} / {} passed",
            currentTimeString(), snapshot.size(), list.size());

    if (snapshot.empty()) {
        pt.clearAll();
        inTick = false;
        return;
    }

    // ════════════════════════════════════════════════════════
    //  阶段 3：按 BlockSource 分组 → 网格分区 → 4-色着色
    // ════════════════════════════════════════════════════════
    struct Task {
        BlockSource*                bs;
        std::vector<ActorTickEntry> entries;
    };
    std::array<std::vector<Task>, 4> phases;

    std::unordered_map<BlockSource*, std::vector<ActorTickEntry>> bsMap;
    for (auto& e : snapshot)
        bsMap[e.region].push_back(e);

    int curMax = conf.autoAdjust
               ? pt.getAutoMaxEntities()
               : conf.maxEntitiesPerTask;

    size_t totalTasks = 0;

    for (auto& [bs, entries] : bsMap) {
        std::unordered_map<GridPos, std::vector<ActorTickEntry>, GridPosHash> gridMap;
        for (auto& e : entries) {
            auto pos = e.actor->getPosition();
            GridPos gp{
                static_cast<int>(std::floor(pos.x / conf.gridSizeBase)),
                static_cast<int>(std::floor(pos.z / conf.gridSizeBase))
            };
            gridMap[gp].push_back(e);
        }

        for (auto& [gp, ge] : gridMap) {
            int color = gridColor(gp);

            if (static_cast<int>(ge.size()) <= curMax) {
                phases[color].push_back({bs, std::move(ge)});
                totalTasks++;
            } else {
                // 拆分大网格
                for (size_t i = 0; i < ge.size(); i += static_cast<size_t>(curMax)) {
                    size_t end = std::min(i + static_cast<size_t>(curMax), ge.size());
                    std::vector<ActorTickEntry> chunk(ge.begin() + i, ge.begin() + end);
                    phases[color].push_back({bs, std::move(chunk)});
                    totalTasks++;
                }
            }
        }
    }

    pt.addStats(snapshot.size(), totalTasks);

    if (conf.debug)
        pt.getSelf().getLogger().info(
            "[{}][Grid] {} entities → {} tasks, phases=[{},{},{},{}], {} BS",
            currentTimeString(), snapshot.size(), totalTasks,
            phases[0].size(), phases[1].size(),
            phases[2].size(), phases[3].size(),
            bsMap.size());

    // ════════════════════════════════════════════════════════
    //  阶段 4：逐相位并行执行
    // ════════════════════════════════════════════════════════
    pt.setParallelPhase(true);
    auto& pool = pt.getPool();
    auto  phaseTimeoutMs = std::chrono::milliseconds(
        conf.actorTickTimeoutMs > 0 ? conf.actorTickTimeoutMs : 30000);

    for (int color = 0; color < 4; ++color) {
        auto& phaseTasks = phases[color];
        if (phaseTasks.empty()) continue;

        if (conf.debug)
            pt.getSelf().getLogger().info(
                "[{}][Phase {}] Submitting {} tasks",
                currentTimeString(), color, phaseTasks.size());

        for (auto& task : phaseTasks) {
            pool.submit([&pt, &conf, bs = task.bs,
                         entries = std::move(task.entries)]() mutable
            {
                for (auto& e : entries) {
                    // 锁内最终检查，消除 TOCTOU
                    if (!pt.isActorSafeToTick(e.actor)) continue;
                    if (!e.actor->isInWorld()) continue;

                    if (conf.debug)
                        pt.getSelf().getLogger().info(
                            "[{}][Tick] actor={:p} type={} rid={}",
                            currentTimeString(), (void*)e.actor,
                            static_cast<int>(e.actor->getEntityTypeId()),
                            e.actor->getRuntimeID().rawID);

                    auto start = std::chrono::steady_clock::now();
                    int ex = tickActorSafe(e.actor, *bs);
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count();

                    if (ex != 0) {
                        // ── 异常处理 ──
                        pt.recordCrash(e.actor);
                        pt.getCrashStats().fetch_add(1, std::memory_order_relaxed);

                        const char* exType = (ex == -1) ? "C++exception"
                                           : (ex == -2) ? "UnknownC++exception"
                                           : "SEH";

                        fprintf(stderr,
                            "[ParallelTick] %s 0x%08X actor=%p type=%d "
                            "— crash recorded\n",
                            exType, static_cast<unsigned>(ex),
                            (void*)e.actor,
                            static_cast<int>(e.actor->getEntityTypeId()));

                        pt.getSelf().getLogger().error(
                            "[{}][Crash] {} 0x{:08X} actor={:p} type={} "
                            "elapsed={}ms — marked",
                            currentTimeString(), exType,
                            static_cast<unsigned>(ex), (void*)e.actor,
                            static_cast<int>(e.actor->getEntityTypeId()), ms);

                        // 如果已达永久阈值且配置了 kill，加入延迟 kill 队列
                        if (conf.killCrashedActors
                            && pt.isPermanentlyCrashed(e.actor))
                        {
                            pt.scheduleKill(e.actor);
                            pt.getSelf().getLogger().warn(
                                "[{}][Crash] actor={:p} scheduled for kill",
                                currentTimeString(), (void*)e.actor);
                        }

                        // 跳过同 task 后续实体？不跳——其他实体可能无关
                        // 但当前实体状态可能已脏，不再继续它就够了
                        continue;
                    }

                    // ── 慢 tick 警告 ──
                    if (ms > 100) {
                        pt.getSelf().getLogger().warn(
                            "[{}][SlowTick] actor={:p} type={} took {}ms",
                            currentTimeString(), (void*)e.actor,
                            static_cast<int>(e.actor->getEntityTypeId()), ms);
                    }

                    if (conf.debug)
                        pt.getSelf().getLogger().info(
                            "[{}][Tick] Done actor={:p} {}ms",
                            currentTimeString(), (void*)e.actor, ms);
                }
            });
        }

        // ── Phase Barrier（带超时）──
        bool ok = pool.waitAllFor(phaseTimeoutMs);
        if (!ok) {
            // 超时：某些 task 卡死
            pt.getSelf().getLogger().error(
                "[{}][Phase {}] TIMEOUT after {}ms! pending={} — "
                "continuing to avoid server hang",
                currentTimeString(), color, phaseTimeoutMs.count(),
                pool.pendingCount());
            // 不能 abort 线程，只能继续（线程最终会完成或被 SEH 捕获）
            // 等待更长时间再继续
            pool.waitAllFor(std::chrono::milliseconds(phaseTimeoutMs.count() * 2));
        }

        if (conf.debug)
            pt.getSelf().getLogger().info(
                "[{}][Phase {}] Complete", currentTimeString(), color);
    }

    pt.setParallelPhase(false);

    // ════════════════════════════════════════════════════════
    //  阶段 5：清理
    // ════════════════════════════════════════════════════════
    pt.clearAll();

    // ════════════════════════════════════════════════════════
    //  阶段 6：自动调整 maxEntitiesPerTask
    // ════════════════════════════════════════════════════════
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tickStart).count();

    if (conf.autoAdjust) {
        int oldVal = pt.getAutoMaxEntities();
        int newVal = oldVal;
        if (elapsed > conf.targetTickMs + 5)
            newVal = std::max(conf.minEntitiesPerTask,
                              oldVal - conf.adjustStep);
        else if (elapsed < conf.targetTickMs - 5)
            newVal = std::min(conf.maxEntitiesPerTaskLimit,
                              oldVal + conf.adjustStep);
        if (newVal != oldVal) {
            pt.setAutoMaxEntities(newVal);
            if (conf.debug)
                pt.getSelf().getLogger().info(
                    "[{}][AutoAdjust] {} -> {} ({}ms)",
                    currentTimeString(), oldVal, newVal, elapsed);
        }
    }

    if (conf.debug)
        pt.getSelf().getLogger().info(
            "[{}][LevelTick] Exit elapsed={}ms",
            currentTimeString(), elapsed);

    inTick = false;
}

// ── 注册 / 注销 ──
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
