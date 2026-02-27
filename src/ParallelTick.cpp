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
#include <mc/world/level/dimension/Dimension.h>

#include <windows.h>
#include <cmath>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <queue>
#include <unordered_map>
#include <cstdio>

using LevelRemoveByActor   = ::OwnerPtr<::EntityContext> (Level::*)(::Actor&);
using LevelRemoveByWeakRef = ::OwnerPtr<::EntityContext> (Level::*)(::WeakEntityRef);

// SEH 安全 tick，独立函数，内部不能有 C++ 析构对象
static int tickActorSafe(Actor* actor, BlockSource& region) {
    __try {
        actor->tick(region);
        return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

namespace parallel_tick {

static std::string currentTimeString() {
    auto now       = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    tm tm;
    localtime_s(&tm, &in_time_t);
    ss << std::put_time(&tm, "%H:%M:%S");
    return ss.str();
}

static Actor* tryGetActorFromWeakRef(WeakEntityRef const& ref) {
    if (auto stackRef = ref.lock()) {
        if (EntityContext* ctx = stackRef.operator->()) {
            return Actor::tryGetFromEntity(*ctx, false);
        }
    }
    return nullptr;
}

ParallelTick& ParallelTick::getInstance() {
    static ParallelTick instance;
    return instance;
}

void ParallelTick::startStatsTask() {
    if (mStatsTaskRunning.exchange(true)) return;
    ll::coro::keepThis([this]() -> ll::coro::CoroTask<> {
        while (mStatsTaskRunning.load()) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([this] {
                if (!mConfig.stats) return;
                size_t total    = mTotalStats.exchange(0);
                size_t parallel = mParallelStats.exchange(0);
                size_t serial   = mSerialStats.exchange(0);
                if (total > 0) {
                    getSelf().getLogger().info(
                        "[{}][ParallelStats] Total={}, Tasks={}, Serial={}",
                        currentTimeString(), total, parallel, serial
                    );
                }
            });
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

void ParallelTick::stopStatsTask() { mStatsTaskRunning.store(false); }

bool ParallelTick::load() {
    auto path = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(mConfig, path))
        ll::config::saveConfig(mConfig, path);
    mAutoMaxEntities.store(mConfig.maxEntitiesPerTask);
    getSelf().getLogger().info(
        "[{}][ParallelTick::load] enabled={}, debug={}, stats={}, "
        "threadCount={}, maxEntities={}, gridBase={}, autoAdjust={}, targetMs={}",
        currentTimeString(),
        mConfig.enabled, mConfig.debug, mConfig.stats,
        mConfig.threadCount, mConfig.maxEntitiesPerTask,
        mConfig.gridSizeBase, mConfig.autoAdjust, mConfig.targetTickMs
    );
    return true;
}

bool ParallelTick::enable() {
    int threadCount = mConfig.threadCount;
    if (threadCount <= 0)
        threadCount = std::max(1u, std::thread::hardware_concurrency() - 1);
    mPool = std::make_unique<FixedThreadPool>(
        static_cast<size_t>(threadCount), 8 * 1024 * 1024
    );
    registerHooks();
    getSelf().getLogger().info(
        "[{}][ParallelTick::enable] Hooks registered, threads={}, stack=8MB",
        currentTimeString(), threadCount
    );
    if (mConfig.stats || mConfig.debug) startStatsTask();
    return true;
}

bool ParallelTick::disable() {
    unregisterHooks();
    stopStatsTask();
    mPool.reset();
    getSelf().getLogger().info("[{}][ParallelTick::disable] Done", currentTimeString());
    return true;
}

// ----------------------------------------------------------------
// removeEntity Hooks
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
    if (Actor* actor = tryGetActorFromWeakRef(entityRef)) {
        parallel_tick::ParallelTick::getInstance().onActorRemoved(actor);
    }
    return origin(std::move(entityRef));
}

// ----------------------------------------------------------------
// Actor::tick Hook
// 收集阶段：入队，阻止 origin（返回 true）
// 非收集阶段：若已被并行 tick，阻止重复执行；否则走原始 tick
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

    if (!conf.enabled) return origin(region);

    // 收集阶段
    if (pt.isCollecting()) {
        if (this->isPlayer() || this->isSimulatedPlayer())
            return origin(region);
        pt.collectActor(this, region);
        return true;  // 阻止 origin，由工作线程稍后执行
    }

    // 非收集阶段（MCBE 其他路径触发的 tick）
    // 若该实体已被我们并行 tick 过，跳过，防止双重 tick 导致状态损坏
    if (pt.wasTicked(this)) {
        if (conf.debug) {
            pt.getSelf().getLogger().info(
                "[{}][ActorTickHook] Skip already-ticked actor={:p}",
                currentTimeString(), (void*)this
            );
        }
        return true;
    }

    return origin(region);
}

// ----------------------------------------------------------------
// Level::$tick Hook
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

    auto& pt   = parallel_tick::ParallelTick::getInstance();
    auto  conf = pt.getConfig();
    auto  tickStart = std::chrono::steady_clock::now();

    if (!conf.enabled) {
        origin();
        inTick = false;
        return;
    }

    // ── 阶段1：收集 ──────────────────────────────────────────────
    pt.setCollecting(true);
    origin();
    pt.setCollecting(false);

    auto list = pt.takeQueue();

    if (list.empty()) {
        pt.clearLive();
        pt.clearTicked();
        inTick = false;
        return;
    }

    // ── 阶段2：快照过滤（主线程持锁）────────────────────────────
    std::vector<ActorTickEntry> snapshot;
    snapshot.reserve(list.size());
    {
        std::unique_lock<std::recursive_mutex> lock(pt.getLifecycleMutex());
        for (auto& e : list) {
            if (e.actor && pt.isActorAlive(e.actor))
                snapshot.push_back(e);
        }
    }

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ParallelTick] snapshot={} / queued={}",
            currentTimeString(), snapshot.size(), list.size()
        );
    }

    if (snapshot.empty()) {
        pt.clearLive();
        pt.clearTicked();
        inTick = false;
        return;
    }

    // ── 阶段3：标记进入并行 tick ──────────────────────────────────
    pt.setTickingNow(true);

    // ── 阶段4：按 BlockSource 分组 ──────────────────────────────
    std::unordered_map<BlockSource*, std::vector<ActorTickEntry>> bsMap;
    for (auto& entry : snapshot) bsMap[entry.region].push_back(entry);

    // ── 阶段5：网格分区 + BFS 合并 ───────────────────────────────
    int currentMax = conf.autoAdjust
                   ? pt.getAutoMaxEntities()
                   : conf.maxEntitiesPerTask;

    struct Task {
        BlockSource*                bs;
        std::vector<ActorTickEntry> entries;
    };
    std::vector<Task> taskList;

    for (auto& [bs, entries] : bsMap) {
        std::unordered_map<GridPos, std::vector<ActorTickEntry>, GridPosHash> gridMap;
        for (auto& entry : entries) {
            auto pos = entry.actor->getPosition();
            int gx   = static_cast<int>(std::floor(pos.x / conf.gridSizeBase));
            int gz   = static_cast<int>(std::floor(pos.z / conf.gridSizeBase));
            gridMap[{gx, gz}].push_back(entry);
        }

        std::unordered_set<GridPos, GridPosHash> visited;
        for (auto& [gridPos, gridEntries] : gridMap) {
            if (visited.count(gridPos)) continue;
            std::queue<GridPos> q;
            q.push(gridPos);
            visited.insert(gridPos);
            std::vector<ActorTickEntry> block;
            block.insert(block.end(), gridEntries.begin(), gridEntries.end());
            while (!q.empty()) {
                GridPos cur = q.front(); q.pop();
                const GridPos neighbors[4] = {
                    {cur.x+1,cur.z},{cur.x-1,cur.z},
                    {cur.x,cur.z+1},{cur.x,cur.z-1}
                };
                for (auto& nb : neighbors) {
                    auto it = gridMap.find(nb);
                    if (it != gridMap.end() && !visited.count(nb)) {
                        if (block.size() + it->second.size()
                                <= static_cast<size_t>(currentMax)) {
                            block.insert(block.end(),
                                it->second.begin(), it->second.end());
                            visited.insert(nb);
                            q.push(nb);
                        }
                    }
                }
            }
            taskList.push_back({bs, std::move(block)});
        }
    }

    pt.addStats(snapshot.size(), taskList.size(), 0);

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ParallelTick] Submitting {} tasks, {} entities",
            currentTimeString(), taskList.size(), snapshot.size()
        );
    }

    // ── 阶段6：提交线程池 ────────────────────────────────────────
    auto& pool = pt.getPool();

    for (auto& task : taskList) {
        pool.submit([&pt, conf, task = std::move(task)]() mutable {
            for (auto& entry : task.entries) {
                if (!entry.actor) continue;
                if (!entry.actor->isInWorld()) continue;

                int exCode = tickActorSafe(entry.actor, *entry.region);

                if (exCode == 0) {
                    // tick 成功，标记防止主线程重复 tick
                    pt.markTicked(entry.actor);
                    if (conf.debug) {
                        pt.getSelf().getLogger().info(
                            "[{}][ParallelTick][Task] Done actor={:p}",
                            currentTimeString(), (void*)entry.actor
                        );
                    }
                } else {
                    fprintf(stderr,
                        "[ParallelTick][Task] SEH 0x%08X on actor=%p, skipped\n",
                        static_cast<unsigned>(exCode),
                        static_cast<void*>(entry.actor));
                    if (conf.debug) {
                        pt.getSelf().getLogger().warn(
                            "[ParallelTick][Task] SEH 0x{:08X} actor={:p} skipped",
                            static_cast<unsigned>(exCode), (void*)entry.actor
                        );
                    }
                }
            }
        });
    }

    // ── 阶段7：等待所有任务完成 ──────────────────────────────────
    pool.waitAll();

    // ── 阶段8：退出并行 tick，清理 ───────────────────────────────
    pt.setTickingNow(false);
    pt.flushPendingRemove();
    pt.clearTicked();   // waitAll 后清理，主线程此后可以重新 tick 新实体
    pt.clearLive();

    // ── 阶段9：自动调整 ──────────────────────────────────────────
    auto tickEnd = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       tickEnd - tickStart).count();

    if (conf.autoAdjust && conf.enabled) {
        int oldVal = pt.getAutoMaxEntities();
        int newVal = oldVal;
        if (elapsed > conf.targetTickMs + 5)
            newVal = std::max(conf.minEntitiesPerTask, oldVal - conf.adjustStep);
        else if (elapsed < conf.targetTickMs - 5)
            newVal = std::min(conf.maxEntitiesPerTaskLimit, oldVal + conf.adjustStep);
        if (newVal != oldVal) {
            pt.setAutoMaxEntities(newVal);
            if (conf.debug) {
                pt.getSelf().getLogger().info(
                    "[{}][AutoAdjust] {} -> {} ({}ms)",
                    currentTimeString(), oldVal, newVal, elapsed
                );
            }
        }
    }

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ParallelLevelTickHook] Exit, elapsed={}ms",
            currentTimeString(), elapsed
        );
    }

    inTick = false;
}

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
