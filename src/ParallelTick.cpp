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

#include <cmath>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <queue>
#include <unordered_map>

using LevelRemoveByActor   = ::OwnerPtr<::EntityContext> (Level::*)(::Actor&);
using LevelRemoveByWeakRef = ::OwnerPtr<::EntityContext> (Level::*)(::WeakEntityRef);

namespace parallel_tick {

static std::string currentTimeString() {
    auto now        = std::chrono::system_clock::now();
    auto in_time_t  = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    tm tm;
    localtime_s(&tm, &in_time_t);
    ss << std::put_time(&tm, "%H:%M:%S");
    return ss.str();
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
                        "[{}][ParallelStats] Total={}, Parallel={}, Serial={}",
                        currentTimeString(), total, parallel, serial
                    );
                }
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
        "[{}][ParallelTick::load] Config loaded, enabled={}, debug={}, stats={}, "
        "threadCount={}, maxEntities={}, gridBase={}, autoAdjust={}, targetMs={}",
        currentTimeString(), mConfig.enabled, mConfig.debug, mConfig.stats,
        mConfig.threadCount, mConfig.maxEntitiesPerTask, mConfig.gridSizeBase,
        mConfig.autoAdjust, mConfig.targetTickMs
    );
    return true;
}

bool ParallelTick::enable() {
    int threadCount = mConfig.threadCount;
    if (threadCount <= 0) {
        threadCount = std::max(1u, std::thread::hardware_concurrency() - 1);
    }
    // 8MB 栈，防止深调用栈溢出
    mPool = std::make_unique<FixedThreadPool>(threadCount, 8 * 1024 * 1024);

    registerHooks();
    getSelf().getLogger().info(
        "[{}][ParallelTick::enable] Hooks registered, thread pool size={}, stack=8MB",
        currentTimeString(), threadCount
    );

    if (mConfig.stats || mConfig.debug) {
        startStatsTask();
    }
    return true;
}

bool ParallelTick::disable() {
    unregisterHooks();
    stopStatsTask();
    mPool.reset();
    getSelf().getLogger().info("[{}][ParallelTick::disable] Hooks unregistered", currentTimeString());
    return true;
}

// ----------------------------------------------------------------
// 移除实体 Hook（延迟移除）
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
            "[{}][ParallelRemoveActorLock] Enter, actor={:p}",
            currentTimeString(), (void*)&actor
        );
    }

    // 延迟移除：打标记，不立即删除，防止工作线程持有的快照中出现野指针
    pt.onActorRemoved(&actor);

    auto result = origin(actor);

    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelRemoveActorLock] Exit", currentTimeString());
    }
    return result;
}

static Actor* tryGetActorFromWeakRef(WeakEntityRef const& ref) {
    if (auto stackRef = ref.lock()) {
        if (EntityContext* ctx = stackRef.operator->()) {
            return Actor::tryGetFromEntity(*ctx, false);
        }
    }
    return nullptr;
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

    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelRemoveWeakRefLock] Enter", currentTimeString());
    }

    if (Actor* actor = tryGetActorFromWeakRef(entityRef)) {
        if (conf.debug) {
            pt.getSelf().getLogger().info(
                "[{}][ParallelRemoveWeakRefLock] Found actor={:p}, marking for removal",
                currentTimeString(), (void*)actor
            );
        }
        // 延迟移除
        pt.onActorRemoved(actor);
    } else {
        if (conf.debug) {
            pt.getSelf().getLogger().info(
                "[{}][ParallelRemoveWeakRefLock] Failed to get actor from WeakEntityRef",
                currentTimeString()
            );
        }
    }

    auto result = origin(std::move(entityRef));

    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelRemoveWeakRefLock] Exit", currentTimeString());
    }
    return result;
}

// ----------------------------------------------------------------
// Actor::tick 拦截 Hook
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

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ParallelActorTickHook] Enter, this={:p}, isPlayer={}, isSimulatedPlayer={}, "
            "enabled={}, collecting={}",
            currentTimeString(), (void*)this,
            this->isPlayer(), this->isSimulatedPlayer(),
            conf.enabled, pt.isCollecting()
        );
    }

    if (!conf.enabled || !pt.isCollecting()) {
        if (conf.debug) {
            pt.getSelf().getLogger().info(
                "[{}][ParallelActorTickHook] Skipped (disabled or not collecting)",
                currentTimeString()
            );
        }
        return origin(region);
    }

    // 玩家和模拟玩家走原始 tick
    if (this->isPlayer() || this->isSimulatedPlayer()) {
        if (conf.debug) {
            pt.getSelf().getLogger().info(
                "[{}][ParallelActorTickHook] Player tick, calling origin",
                currentTimeString()
            );
        }
        return origin(region);
    }

    // 收集入队，跳过原始 tick
    pt.collectActor(this, region);

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ParallelActorTickHook] Collected actor={:p}, region={:p}",
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
    if (inTick) {
        origin();
        return;
    }
    inTick = true;

    auto& pt   = parallel_tick::ParallelTick::getInstance();
    auto  conf = pt.getConfig();  // 按值拷贝，线程安全

    auto tickStart = std::chrono::steady_clock::now();

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ParallelLevelTickHook] Enter, enabled={}",
            currentTimeString(), conf.enabled
        );
    }

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

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ParallelTick] Queue size: {}",
            currentTimeString(), list.size()
        );
    }

    if (list.empty()) {
        pt.clearLive();
        inTick = false;
        return;
    }

    // ── 阶段2：主线程建立快照（持锁过滤，工作线程不再查询存活状态）──
    std::vector<ActorTickEntry> snapshot;
    snapshot.reserve(list.size());
    {
        std::unique_lock<std::recursive_mutex> lock(pt.getLifecycleMutex());
        for (auto& e : list) {
            if (e.actor && pt.isActorAlive(e.actor)) {
                snapshot.push_back(e);
            }
        }
    }   // 锁在这里释放，快照建立完成

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ParallelTick] Snapshot size: {} (filtered from {})",
            currentTimeString(), snapshot.size(), list.size()
        );
    }

    if (snapshot.empty()) {
        pt.clearLive();
        inTick = false;
        return;
    }

    // ── 阶段3：按 BlockSource 分组，同一 BlockSource 内串行，不同 BlockSource 并行 ──
    std::unordered_map<BlockSource*, std::vector<ActorTickEntry>> bsMap;
    for (auto& entry : snapshot) {
        // 使用 collectActor 时保存的原始 region，不重新获取
        bsMap[entry.region].push_back(entry);
    }

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ParallelTick] BlockSource groups: {}",
            currentTimeString(), bsMap.size()
        );
    }

    // ── 阶段4：在每个 BlockSource 组内再做网格合并（可选，保留原有逻辑）──
    int currentMax = conf.autoAdjust ? pt.getAutoMaxEntities() : conf.maxEntitiesPerTask;

    // 收集所有最终任务块
    std::vector<std::pair<BlockSource*, std::vector<ActorTickEntry>>> taskList;

    for (auto& [bs, entries] : bsMap) {
        // 对该 BlockSource 内的实体做网格分区 + BFS 合并
        std::unordered_map<GridPos, std::vector<ActorTickEntry>, GridPosHash> gridMap;
        for (auto& entry : entries) {
            auto pos = entry.actor->getPosition();
            int gx = static_cast<int>(std::floor(pos.x / conf.gridSizeBase));
            int gz = static_cast<int>(std::floor(pos.z / conf.gridSizeBase));
            gridMap[{gx, gz}].push_back(entry);
        }

        std::unordered_set<GridPos, GridPosHash> visited;

        for (auto& [gridPos, gridEntries] : gridMap) {
            if (visited.count(gridPos)) continue;

            std::queue<GridPos> q;
            q.push(gridPos);
            visited.insert(gridPos);

            std::vector<ActorTickEntry> taskBlock;
            taskBlock.insert(taskBlock.end(), gridEntries.begin(), gridEntries.end());

            while (!q.empty()) {
                GridPos cur = q.front(); q.pop();

                // 检查四邻域（±x, ±z）
                const GridPos neighbors[4] = {
                    {cur.x + 1, cur.z},
                    {cur.x - 1, cur.z},
                    {cur.x, cur.z + 1},
                    {cur.x, cur.z - 1}
                };
                for (auto& neighbor : neighbors) {
                    auto it = gridMap.find(neighbor);
                    if (it != gridMap.end() && !visited.count(neighbor)) {
                        if (taskBlock.size() + it->second.size() <= (size_t)currentMax) {
                            taskBlock.insert(taskBlock.end(), it->second.begin(), it->second.end());
                            visited.insert(neighbor);
                            q.push(neighbor);
                        }
                    }
                }
            }
            taskList.emplace_back(bs, std::move(taskBlock));
        }
    }

    // ── 阶段5：统计 ──────────────────────────────────────────────
    pt.addStats(snapshot.size(), taskList.size(), 0);

    if (conf.debug) {
        pt.getSelf().getLogger().info(
            "[{}][ParallelTick] Submitting {} tasks from {} entities",
            currentTimeString(), taskList.size(), snapshot.size()
        );
    }

    // ── 阶段6：提交线程池，工作线程不持全局锁 ──────────────────
    auto& pool = pt.getPool();
    for (auto& [bs, taskBlock] : taskList) {
        pool.submit([&pt, conf, bs, taskBlock = std::move(taskBlock)]() mutable {
            // 工作线程不持任何全局锁
            // 同一 BlockSource 内的实体在此任务中串行 tick
            for (auto& entry : taskBlock) {
                if (!entry.actor) continue;

                // 使用收集时保存的原始 region（entry.region == bs，保持一致）
                BlockSource& region  = *entry.region;
                auto typeId          = (int)entry.actor->getEntityTypeId();
                auto entityId        = entry.actor->getRuntimeID().rawID;
                auto pos             = entry.actor->getPosition();
                auto dimId           = entry.actor->getDimensionId();

                if (conf.debug) {
                    pt.getSelf().getLogger().info(
                        "[{}][ParallelTick][Task] Tick: typeId={}, id={}, actor={:p}, bs={:p}",
                        currentTimeString(), typeId, entityId, (void*)entry.actor, (void*)bs
                    );
                }

                // 异常被 FixedThreadPool 的包装层捕获，不会再传出
                entry.actor->tick(region);

                if (conf.debug) {
                    pt.getSelf().getLogger().info(
                        "[{}][ParallelTick][Task] Done: typeId={}, id={}",
                        currentTimeString(), typeId, entityId
                    );
                }
            }
        });
    }

    // ── 阶段7：等待所有任务完成 ──────────────────────────────────
    pool.waitAll();

    // ── 阶段8：统一清理延迟移除的实体 ────────────────────────────
    pt.flushPendingRemove();
    pt.clearLive();

    // ── 阶段9：自动调整 maxEntitiesPerTask ───────────────────────
    auto tickEnd = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tickEnd - tickStart).count();

    if (conf.autoAdjust && conf.enabled) {
        int oldVal = pt.getAutoMaxEntities();
        int newVal = oldVal;

        if (elapsed > conf.targetTickMs + 5) {
            newVal = std::max(conf.minEntitiesPerTask, oldVal - conf.adjustStep);
        } else if (elapsed < conf.targetTickMs - 5) {
            newVal = std::min(conf.maxEntitiesPerTaskLimit, oldVal + conf.adjustStep);
        }

        if (newVal != oldVal) {
            pt.setAutoMaxEntities(newVal);
            if (conf.debug) {
                pt.getSelf().getLogger().info(
                    "[{}][AutoAdjust] maxEntities: {} -> {} (elapsed={}ms)",
                    currentTimeString(), oldVal, newVal, elapsed
                );
            }
        } else if (conf.debug) {
            pt.getSelf().getLogger().info(
                "[{}][AutoAdjust] No change (elapsed={}ms)",
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

void parallel_tick::registerHooks() {
    ParallelRemoveActorLock::hook();
    ParallelRemoveWeakRefLock::hook();
    ParallelActorTickHook::hook();
    ParallelLevelTickHook::hook();
}

void parallel_tick::unregisterHooks() {
    ParallelRemoveActorLock::unhook();
    ParallelRemoveWeakRefLock::unhook();
    ParallelActorTickHook::unhook();
    ParallelLevelTickHook::unhook();
}

LL_REGISTER_MOD(parallel_tick::ParallelTick, parallel_tick::ParallelTick::getInstance());

} // namespace parallel_tick
