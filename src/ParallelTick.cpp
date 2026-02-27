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

using LevelRemoveByActor   = ::OwnerPtr<::EntityContext>(Level::*)(::Actor&);
using LevelRemoveByWeakRef = ::OwnerPtr<::EntityContext>(Level::*)(::WeakEntityRef);

// ── SEH 安全 tick（独立非内联函数，无 C++ 析构对象）──
static int tickActorSafe(Actor* actor, BlockSource& region) {
    __try {
        actor->tick(region);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
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
                if (tot > 0)
                    getSelf().getLogger().info(
                        "[{}][Stats] Total={} Phases={}",
                        currentTimeString(), tot, ph);
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
        "[{}][load] enabled={} debug={} threads={} maxE={} grid={} auto={} target={}ms",
        currentTimeString(), mConfig.enabled, mConfig.debug,
        mConfig.threadCount, mConfig.maxEntitiesPerTask,
        mConfig.gridSizeBase, mConfig.autoAdjust, mConfig.targetTickMs);
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
// 并行阶段通过 LevelMutex 串行化 origin 调用
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
    if (Actor* a = tryGetActorFromWeakRef(entityRef))
        pt.onActorRemoved(a);

    if (pt.isParallelPhase()) {
        std::lock_guard<std::mutex> lk(pt.getLevelMutex());
        return origin(std::move(entityRef));
    }
    return origin(std::move(entityRef));
}

// ── Actor::tick Hook ──
// 收集阶段：拦截非玩家实体入队，不执行 origin
LL_TYPE_INSTANCE_HOOK(
    ParallelActorTickHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::tick,
    bool,
    ::BlockSource& region
) {
    auto& pt        = ParallelTick::getInstance();
    const auto& conf = pt.getConfig();

    if (!conf.enabled || !pt.isCollecting())
        return origin(region);

    // 玩家/模拟玩家始终串行
    if (this->isPlayer() || this->isSimulatedPlayer())
        return origin(region);

    // 已崩溃实体永久跳过
    if (pt.isCrashed(this))
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
//   同色网格在 X 和 Z 方向上间距 ≥ 2 个网格 = 2 * gridSizeBase
//   gridSizeBase=64 时，同色网格间距 ≥ 128 格，远超任何实体交互半径
//
//   执行顺序：
//     Phase 0 → waitAll → Phase 1 → waitAll → Phase 2 → waitAll → Phase 3 → waitAll
//
//   同一 Phase 内的所有任务可安全并行：
//     - 不会访问相同的区块（空间距离 ≥ 128 格）
//     - 不会互相交互（超出最大交互距离）
//     - Level 全局变更（addEntity/removeEntity）通过 LevelMutex 串行化
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
    //  阶段 1：收集实体
    //  origin() 内部遍历实体 → Actor::tick Hook 拦截入队
    //  origin() 完成后，MCBE 的非实体逻辑（红石/区块/天气等）已执行
    //  所有非玩家实体的 tick 被拦截，未实际执行
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
        if (pt.isCrashed(e.actor)) continue;
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

    // 结构：phases[color] = vector<Task>
    // Task = { BlockSource*, vector<ActorTickEntry> }
    struct Task {
        BlockSource*              bs;
        std::vector<ActorTickEntry> entries;
    };
    std::array<std::vector<Task>, 4> phases;

    // 按 BlockSource 分组
    std::unordered_map<BlockSource*, std::vector<ActorTickEntry>> bsMap;
    for (auto& e : snapshot)
        bsMap[e.region].push_back(e);

    int curMax = conf.autoAdjust
               ? pt.getAutoMaxEntities()
               : conf.maxEntitiesPerTask;

    size_t totalTasks = 0;

    for (auto& [bs, entries] : bsMap) {
        // 按网格坐标分桶
        std::unordered_map<GridPos, std::vector<ActorTickEntry>, GridPosHash> gridMap;
        for (auto& e : entries) {
            auto pos = e.actor->getPosition();
            GridPos gp{
                static_cast<int>(std::floor(pos.x / conf.gridSizeBase)),
                static_cast<int>(std::floor(pos.z / conf.gridSizeBase))
            };
            gridMap[gp].push_back(e);
        }

        // 每个网格按 color 分入对应 phase
        // 同一 color 内，如果单个网格实体数 > curMax，拆分成多个 Task
        for (auto& [gp, ge] : gridMap) {
            int color = gridColor(gp);

            if (static_cast<int>(ge.size()) <= curMax) {
                phases[color].push_back({bs, std::move(ge)});
                totalTasks++;
            } else {
                // 拆分大网格
                for (size_t i = 0; i < ge.size(); i += curMax) {
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
    //
    //  同一相位内的所有网格间距 ≥ 2*gridSizeBase（128格）
    //  → 不可能访问相同区块
    //  → 不可能互相交互（远超最大交互距离）
    //
    //  相位之间有 barrier（waitAll），确保串行
    //
    //  Level 全局变更（removeEntity 等）通过 LevelMutex 串行化
    // ════════════════════════════════════════════════════════
    pt.setParallelPhase(true);
    auto& pool = pt.getPool();

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
                    // 在锁内做最终存活检查，消除 TOCTOU
                    if (!pt.isActorSafeToTick(e.actor)) continue;

                    // 二次检查 isInWorld
                    if (!e.actor->isInWorld()) continue;

                    if (conf.debug)
                        pt.getSelf().getLogger().info(
                            "[{}][Tick] actor={:p} type={} rid={}",
                            currentTimeString(), (void*)e.actor,
                            static_cast<int>(e.actor->getEntityTypeId()),
                            e.actor->getRuntimeID().rawID);

                    int ex = tickActorSafe(e.actor, *bs);

                    if (ex != 0) {
                        // SEH 异常：标记该实体永久跳过
                        // 状态可能已损坏，不能继续 tick 此实体
                        pt.markCrashed(e.actor);
                        fprintf(stderr,
                            "[ParallelTick] SEH 0x%08X actor=%p type=%d — "
                            "permanently disabled\n",
                            static_cast<unsigned>(ex),
                            (void*)e.actor,
                            static_cast<int>(e.actor->getEntityTypeId()));
                        if (conf.debug)
                            pt.getSelf().getLogger().warn(
                                "[Tick] SEH 0x{:08X} actor={:p} — marked crashed",
                                static_cast<unsigned>(ex), (void*)e.actor);
                    } else if (conf.debug) {
                        pt.getSelf().getLogger().info(
                            "[{}][Tick] Done actor={:p}",
                            currentTimeString(), (void*)e.actor);
                    }
                }
            });
        }

        // ── Phase Barrier ──
        // 等待当前相位所有任务完成后，才开始下一相位
        pool.waitAll();

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
                    "[{}][AutoAdjust] {} → {} ({}ms)",
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
