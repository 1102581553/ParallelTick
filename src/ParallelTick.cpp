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

// ----------------------------------------------------------------
// SEH 安全 tick
// 必须是独立的非内联函数，内部不能有任何 C++ 对象析构
// 返回 0 表示成功，非 0 为 SEH 异常码
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
// 工具函数
// ----------------------------------------------------------------

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

// ----------------------------------------------------------------
// 单例
// ----------------------------------------------------------------

ParallelTick& ParallelTick::getInstance() {
    static ParallelTick instance;
    return instance;
}

// ----------------------------------------------------------------
// 统计任务
// ----------------------------------------------------------------

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

void ParallelTick::stopStatsTask() {
    mStatsTaskRunning.store(false);
}

// ----------------------------------------------------------------
// 生命周期
// ----------------------------------------------------------------

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

    // 8MB 栈，防止深调用栈溢出
    mPool = std::make_unique<FixedThreadPool>(
        static_cast<size_t>(threadCount),
        8 * 1024 * 1024
    );

    registerHooks();

    getSelf().getLogger().info(
        "[{}][ParallelTick::enable] Hooks registered, threads={}, stack=8MB",
        currentTimeString(), threadCount
    );

    if (mConfig.stats || mConfig.debug)
        startStatsTask();

    return true;
}

bool ParallelTick::disable() {
    unregisterHooks();
    stopStatsTask();
    mPool.reset();
    getSelf().getLogger().info(
        "[{}][ParallelTick::disable] Done",
        currentTimeString()
    );
    return true;
}

// ----------------------------------------------------------------
// removeEntity Hook（两个重载）
// 并行 tick 期间只挂起，非 tick 期间直接移除
// MCBE 的内存释放照常发生，我们只维护指针集合
// ----------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    ParallelRemoveActorLock,
    ll::memory::HookPriority::Normal,
    Level,
    static_cast<LevelRemoveByActor>(&Level::$removeEntity),
    ::OwnerPtr<::EntityContext>,
    ::Actor& actor
) {
    auto& pt   = parallel_tick::ParallelTick::getInstance();
    auto& conf = pt.getConfig();

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][RemoveActor] actor={:p}, tickingNow={}",
            currentTimeString(), (void*)&actor, pt.isTickingNow()
        );
    }

    pt.onActorRemoved(&actor);
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
    auto& pt   = parallel_tick::ParallelTick::getInstance();
    auto& conf = pt.getConfig();

    if (Actor* actor = tryGetActorFromWeakRef(entityRef)) {
        if (conf.debug) {
            pt.getSelf().getLogger().info(
                "[{}][RemoveWeakRef] actor={:p}, tickingNow={}",
                currentTimeString(), (void*)actor, pt.isTickingNow()
            );
        }
        pt.onActorRemoved(actor);
    }

    return origin(std::move(entityRef));
}

// ----------------------------------------------------------------
// Actor::tick 拦截 Hook
// 玩家和模拟玩家走原始 tick，其余实体收集入队
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

    if (this->isPlayer() || this->isSimulatedPlayer())
        return origin(region);

    pt.collectActor(this, region);

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ActorTickHook] Collected actor={:p}, region={:p}",
            currentTimeString(), (void*)this, (void*)&region
        );
    }
    return true;
}

// ----------------------------------------------------------------
// Level::$tick 外层 Hook
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
    auto  conf = pt.getConfig();   // 按值拷贝，线程安全

    auto tickStart = std::chrono::steady_clock::now();

    if (!conf.enabled) {
        origin();
        inTick = false;
        return;
    }

    // ── 阶段1：收集 ──────────────────────────────────────────────
    pt.setCollecting(true);
    origin();   // 触发所有 Actor::tick Hook，收集入队
    pt.setCollecting(false);

    auto list = pt.takeQueue();

    if (list.empty()) {
        pt.clearLive();
        inTick = false;
        return;
    }

    // ── 阶段2：主线程建立快照（持锁过滤）────────────────────────
    // 过滤掉在收集阶段已经被移除的实体
    std::vector<ActorTickEntry> snapshot;
    snapshot.reserve(list.size());
    {
        std::unique_lock<std::recursive_mutex> lock(pt.getLifecycleMutex());
        for (auto& e : list) {
            if (e.actor && pt.isActorAlive(e.actor)) {
                snapshot.push_back(e);
            }
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
        inTick = false;
        return;
    }

    // ── 阶段3：标记进入并行 tick ──────────────────────────────────
    // 此后 removeEntity Hook 只写 mPendingRemove，不动 mLiveActors
    pt.setTickingNow(true);

    // ── 阶段4：按 BlockSource 分组 ──────────────────────────────
    // 同一 BlockSource 内串行，不同 BlockSource 之间并行
    std::unordered_map<BlockSource*, std::vector<ActorTickEntry>> bsMap;
    for (auto& entry : snapshot) {
        bsMap[entry.region].push_back(entry);
    }

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ParallelTick] BlockSource groups={}",
            currentTimeString(), bsMap.size()
        );
    }

    // ── 阶段5：组内网格分区 + BFS 合并 ──────────────────────────
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
                    {cur.x + 1, cur.z}, {cur.x - 1, cur.z},
                    {cur.x, cur.z + 1}, {cur.x, cur.z - 1}
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
            "[{}][ParallelTick] Submitting {} tasks, {} entities, {} BlockSources",
            currentTimeString(), taskList.size(), snapshot.size(), bsMap.size()
        );
    }

    // ── 阶段6：提交线程池 ────────────────────────────────────────
    // 工作线程不持任何全局锁
    // tickActorSafe 用 SEH 捕获硬件异常，工作线程不崩溃
    auto& pool = pt.getPool();

    for (auto& task : taskList) {
        pool.submit([&pt, conf, task = std::move(task)]() mutable {
            for (auto& entry : task.entries) {
                if (!entry.actor) continue;

                // isInWorld 作为轻量级二次过滤
                // 捕获快照建立之后、tick 执行之前被移除的实体
                if (!entry.actor->isInWorld()) {
                    if (conf.debug) {
                        pt.getSelf().getLogger().info(
                            "[{}][ParallelTick][Task] Skip actor={:p}: not in world",
                            currentTimeString(), (void*)entry.actor
                        );
                    }
                    continue;
                }

                if (conf.debug) {
                    pt.getSelf().getLogger().info(
                        "[{}][ParallelTick][Task] Tick actor={:p}, typeId={}, id={}",
                        currentTimeString(),
                        (void*)entry.actor,
                        (int)entry.actor->getEntityTypeId(),
                        entry.actor->getRuntimeID().rawID
                    );
                }

                // SEH 安全调用：捕获空指针、非法内存访问等硬件异常
                // 使用收集时保存的原始 region，不重新获取
                int exCode = tickActorSafe(entry.actor, *entry.region);

                if (exCode != 0) {
                    fprintf(stderr,
                        "[ParallelTick][Task] SEH 0x%08X on actor=%p, skipped\n",
                        static_cast<unsigned>(exCode),
                        static_cast<void*>(entry.actor));
                    if (conf.debug) {
                        pt.getSelf().getLogger().warn(
                            "[ParallelTick][Task] SEH 0x{:08X} actor={:p} skipped",
                            static_cast<unsigned>(exCode),
                            (void*)entry.actor
                        );
                    }
                } else if (conf.debug) {
                    pt.getSelf().getLogger().info(
                        "[{}][ParallelTick][Task] Done actor={:p}",
                        currentTimeString(), (void*)entry.actor
                    );
                }
            }
        });
    }

    // ── 阶段7：等待所有任务完成 ──────────────────────────────────
    pool.waitAll();

    // ── 阶段8：退出并行 tick，统一清理挂起的移除请求 ─────────────
    pt.setTickingNow(false);
    pt.flushPendingRemove();
    pt.clearLive();

    // ── 阶段9：自动调整 maxEntitiesPerTask ───────────────────────
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
        } else if (conf.debug) {
            pt.getSelf().getLogger().info(
                "[{}][AutoAdjust] No change ({}ms)",
                currentTimeString(), elapsed
            );
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
// Hook 注册 / 注销
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
