#include "ParallelTick.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <ll/api/mod/RegisterHelper.h>

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

using LevelRemoveByActor   = ::OwnerPtr<::EntityContext>(Level::*)(::Actor&);
using LevelRemoveByWeakRef = ::OwnerPtr<::EntityContext>(Level::*)(::WeakEntityRef);

// ----------------------------------------------------------------
// SEH 安全 tick
// 独立非内联函数，内部不含任何 C++ 析构对象
// ----------------------------------------------------------------
static int tickActorSafe(Actor* actor, BlockSource& region) {
    __try {
        actor->tick(region);
        return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

namespace parallel_tick {

// ----------------------------------------------------------------
// 工具
// ----------------------------------------------------------------
static std::string currentTimeString() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    tm buf; localtime_s(&buf, &t);
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

// ----------------------------------------------------------------
// 单例
// ----------------------------------------------------------------
ParallelTick& ParallelTick::getInstance() {
    static ParallelTick inst;
    return inst;
}

// ----------------------------------------------------------------
// 统计
// ----------------------------------------------------------------
void ParallelTick::startStatsTask() {
    if (mStatsTaskRunning.exchange(true)) return;
    ll::coro::keepThis([this]() -> ll::coro::CoroTask<> {
        while (mStatsTaskRunning.load()) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([this] {
                if (!mConfig.stats) return;
                size_t tot = mTotalStats.exchange(0);
                size_t par = mParallelStats.exchange(0);
                if (tot > 0)
                    getSelf().getLogger().info(
                        "[{}][ParallelStats] Total={}, Tasks={}",
                        currentTimeString(), tot, par);
            });
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

void ParallelTick::stopStatsTask() { mStatsTaskRunning.store(false); }

// ----------------------------------------------------------------
// 生命周期
// ----------------------------------------------------------------
bool ParallelTick::load() {
    auto path = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(mConfig, path))
        ll::config::saveConfig(mConfig, path);
    mAutoMaxEntities.store(mConfig.maxEntitiesPerTask);
    getSelf().getLogger().info(
        "[{}][ParallelTick::load] enabled={} debug={} threads={} "
        "maxEntities={} gridBase={} autoAdjust={} targetMs={}",
        currentTimeString(), mConfig.enabled, mConfig.debug,
        mConfig.threadCount, mConfig.maxEntitiesPerTask,
        mConfig.gridSizeBase, mConfig.autoAdjust, mConfig.targetTickMs);
    return true;
}

bool ParallelTick::enable() {
    int n = mConfig.threadCount;
    if (n <= 0) n = std::max(1u, std::thread::hardware_concurrency() - 1);
    mPool = std::make_unique<FixedThreadPool>(n, 8*1024*1024);
    registerHooks();
    getSelf().getLogger().info(
        "[{}][ParallelTick::enable] threads={}", currentTimeString(), n);
    if (mConfig.stats || mConfig.debug) startStatsTask();
    return true;
}

bool ParallelTick::disable() {
    unregisterHooks();
    stopStatsTask();
    mPool.reset();
    getSelf().getLogger().info(
        "[{}][ParallelTick::disable] Done", currentTimeString());
    return true;
}

// ================================================================
// Hook 实现
// ================================================================

// ----------------------------------------------------------------
// removeEntity（两个重载）
// 直接从 liveActors 移除，无论是否在并行期间
// 因为：并行期间工作线程会在 tick 前检查 isInWorld()
// ----------------------------------------------------------------
LL_TYPE_INSTANCE_HOOK(
    ParallelRemoveActorLock,
    ll::memory::HookPriority::Normal,
    Level,
    static_cast<LevelRemoveByActor>(&Level::$removeEntity),
    ::OwnerPtr<::EntityContext>,
    ::Actor& actor
) {
    parallel_tick::ParallelTick::getInstance().onActorRemoved(&actor);
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
    if (Actor* a = tryGetActorFromWeakRef(entityRef))
        parallel_tick::ParallelTick::getInstance().onActorRemoved(a);
    return origin(std::move(entityRef));
}

// ----------------------------------------------------------------
// Actor::tick Hook
// 收集阶段：拦截所有非玩家实体，收集入队，返回 true 阻止 origin
// 非收集阶段：直接走 origin（不做任何干预）
// ----------------------------------------------------------------
LL_TYPE_INSTANCE_HOOK(
    ParallelActorTickHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::tick,
    bool,
    ::BlockSource& region
) {
    auto& pt   = parallel_tick::ParallelTick::getInstance();
    auto& conf = pt.getConfig();

    if (!conf.enabled || !pt.isCollecting())
        return origin(region);

    // 玩家/模拟玩家始终串行
    if (this->isPlayer() || this->isSimulatedPlayer())
        return origin(region);

    // 收集入队，不调用 origin
    pt.collectActor(this, region);

    if (conf.debug)
        pt.getSelf().getLogger().info(
            "[{}][ActorTickHook] Collected actor={:p} typeId={} region={:p}",
            currentTimeString(), (void*)this,
            static_cast<int>(this->getEntityTypeId()), (void*)&region);

    return true;
}

// ----------------------------------------------------------------
// Level::$tick Hook
//
// 关键设计：
//   1. 先执行 origin()（期间收集实体，阻止其 tick）
//      origin() 完成后，MCBE 已完成所有非实体的 Level/Dimension 逻辑
//   2. 工作线程并行 tick 收集到的实体
//   3. waitAll() 确保全部完成
//
// 为什么这样是安全的：
//   - origin() 执行时，实体 tick 全部被 Hook 拦截，没有任何实体实际被 tick
//   - 工作线程 tick 时，origin() 已返回，主线程不再访问 BlockSource
//   - 两个阶段完全串行，不存在并发访问 BlockSource
// ----------------------------------------------------------------
LL_TYPE_INSTANCE_HOOK(
    ParallelLevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    static thread_local bool inTick = false;
    if (inTick) { origin(); return; }
    inTick = true;

    auto& pt        = parallel_tick::ParallelTick::getInstance();
    auto  conf      = pt.getConfig();
    auto  tickStart = std::chrono::steady_clock::now();

    if (!conf.enabled) {
        origin();
        inTick = false;
        return;
    }

    // ── 阶段1：执行 origin()，收集实体 ──────────────────────────
    // Actor::tick Hook 在此期间拦截所有非玩家实体入队
    // origin() 内部的所有其他逻辑（红石、区块、天气等）正常执行
    // 实体 tick 被完全阻止，工作线程此时未启动
    pt.setCollecting(true);
    origin();
    pt.setCollecting(false);
    // ← 此处 origin() 已返回，主线程不再持有任何 BlockSource 操作

    auto list = pt.takeQueue();

    if (list.empty()) {
        pt.clearAll();
        inTick = false;
        return;
    }

    // ── 阶段2：快照过滤 ──────────────────────────────────────────
    // 过滤在收集阶段被移除的实体（onActorRemoved 已从 liveActors 擦除）
    std::vector<ActorTickEntry> snapshot;
    snapshot.reserve(list.size());
    {
        std::unique_lock<std::recursive_mutex> lk(pt.getLifecycleMutex());
        for (auto& e : list)
            if (e.actor && pt.isActorAlive(e.actor))
                snapshot.push_back(e);
    }

    if (conf.debug)
        pt.getSelf().getLogger().info(
            "[{}][ParallelTick] snapshot={} / queued={}",
            currentTimeString(), snapshot.size(), list.size());

    if (snapshot.empty()) {
        pt.clearAll();
        inTick = false;
        return;
    }

    // ── 阶段3：按 BlockSource 分组 ───────────────────────────────
    std::unordered_map<BlockSource*, std::vector<ActorTickEntry>> bsMap;
    for (auto& e : snapshot) bsMap[e.region].push_back(e);

    // ── 阶段4：网格分区 + BFS 合并成任务 ────────────────────────
    int curMax = conf.autoAdjust
               ? pt.getAutoMaxEntities()
               : conf.maxEntitiesPerTask;

    struct Task { BlockSource* bs; std::vector<ActorTickEntry> entries; };
    std::vector<Task> taskList;

    for (auto& [bs, entries] : bsMap) {
        std::unordered_map<GridPos, std::vector<ActorTickEntry>, GridPosHash> gridMap;
        for (auto& e : entries) {
            auto pos = e.actor->getPosition();
            gridMap[{
                static_cast<int>(std::floor(pos.x / conf.gridSizeBase)),
                static_cast<int>(std::floor(pos.z / conf.gridSizeBase))
            }].push_back(e);
        }

        std::unordered_set<GridPos, GridPosHash> visited;
        for (auto& [gp, ge] : gridMap) {
            if (visited.count(gp)) continue;
            std::queue<GridPos> q;
            q.push(gp); visited.insert(gp);
            std::vector<ActorTickEntry> block(ge.begin(), ge.end());
            while (!q.empty()) {
                auto cur = q.front(); q.pop();
                for (auto& nb : std::initializer_list<GridPos>{
                    {cur.x+1,cur.z},{cur.x-1,cur.z},
                    {cur.x,cur.z+1},{cur.x,cur.z-1}}) {
                    auto it = gridMap.find(nb);
                    if (it != gridMap.end() && !visited.count(nb)
                        && block.size()+it->second.size()
                           <= static_cast<size_t>(curMax)) {
                        block.insert(block.end(),
                            it->second.begin(), it->second.end());
                        visited.insert(nb);
                        q.push(nb);
                    }
                }
            }
            taskList.push_back({bs, std::move(block)});
        }
    }

    pt.addStats(snapshot.size(), taskList.size());

    if (conf.debug)
        pt.getSelf().getLogger().info(
            "[{}][ParallelTick] Submitting {} tasks / {} entities / {} BS",
            currentTimeString(), taskList.size(),
            snapshot.size(), bsMap.size());

    // ── 阶段5：并行 tick ─────────────────────────────────────────
    // 此时 origin() 已返回，主线程不再访问 BlockSource
    // 工作线程可以安全地并行 tick 实体
    auto& pool = pt.getPool();
    for (auto& task : taskList) {
        pool.submit([&pt, conf, task = std::move(task)]() mutable {
            for (auto& e : task.entries) {
                if (!e.actor || !e.actor->isInWorld()) continue;

                if (conf.debug)
                    pt.getSelf().getLogger().info(
                        "[{}][Task] Tick actor={:p} typeId={} id={}",
                        currentTimeString(), (void*)e.actor,
                        static_cast<int>(e.actor->getEntityTypeId()),
                        e.actor->getRuntimeID().rawID);

                int ex = tickActorSafe(e.actor, *e.region);
                if (ex != 0) {
                    fprintf(stderr,
                        "[ParallelTick][Task] SEH 0x%08X actor=%p\n",
                        static_cast<unsigned>(ex), (void*)e.actor);
                    if (conf.debug)
                        pt.getSelf().getLogger().warn(
                            "[Task] SEH 0x{:08X} actor={:p} skipped",
                            static_cast<unsigned>(ex), (void*)e.actor);
                } else if (conf.debug) {
                    pt.getSelf().getLogger().info(
                        "[{}][Task] Done actor={:p}",
                        currentTimeString(), (void*)e.actor);
                }
            }
        });
    }

    // ── 阶段6：等待所有工作线程完成 ─────────────────────────────
    pool.waitAll();

    // ── 阶段7：清理 ──────────────────────────────────────────────
    pt.clearAll();

    // ── 阶段8：自动调整 maxEntitiesPerTask ───────────────────────
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tickStart).count();

    if (conf.autoAdjust) {
        int oldVal = pt.getAutoMaxEntities();
        int newVal = oldVal;
        if      (elapsed > conf.targetTickMs + 5)
            newVal = std::max(conf.minEntitiesPerTask, oldVal - conf.adjustStep);
        else if (elapsed < conf.targetTickMs - 5)
            newVal = std::min(conf.maxEntitiesPerTaskLimit, oldVal + conf.adjustStep);
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
            "[{}][ParallelLevelTickHook] Exit elapsed={}ms",
            currentTimeString(), elapsed);

    inTick = false;
}

// ----------------------------------------------------------------
// 注册 / 注销
// ----------------------------------------------------------------
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

LL_REGISTER_MOD(parallel_tick::ParallelTick, parallel_tick::ParallelTick::getInstance());

} // namespace parallel_tick
