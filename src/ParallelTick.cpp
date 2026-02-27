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

using LevelRemoveByActor   = ::OwnerPtr<::EntityContext> (Level::*)(::Actor&);
using LevelRemoveByWeakRef = ::OwnerPtr<::EntityContext> (Level::*)(::WeakEntityRef);

namespace parallel_tick {

static std::string currentTimeString() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    tm tm;
    localtime_s(&tm, &in_time_t);
    ss << std::put_time(&tm, "%H:%M:%S");
    return ss.str();
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

                size_t total = mTotalStats.exchange(0);
                size_t parallel = mParallelStats.exchange(0);
                size_t serial = mSerialStats.exchange(0);

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

bool ParallelTick::load() {
    auto path = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(mConfig, path))
        ll::config::saveConfig(mConfig, path);
    getSelf().getLogger().info("[{}][ParallelTick::load] Config loaded, enabled={}, debug={}, stats={}, threadCount={}, maxEntities={}, gridBase={}",
                               currentTimeString(), mConfig.enabled, mConfig.debug, mConfig.stats, mConfig.threadCount,
                               mConfig.maxEntitiesPerTask, mConfig.gridSizeBase);
    return true;
}

bool ParallelTick::enable() {
    int threadCount = mConfig.threadCount;
    if (threadCount <= 0) {
        threadCount = std::max(1u, std::thread::hardware_concurrency() - 1);
    }
    mPool = std::make_unique<FixedThreadPool>(threadCount);

    registerHooks();
    getSelf().getLogger().info("[{}][ParallelTick::enable] Hooks registered, thread pool size={}",
                               currentTimeString(), threadCount);

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
// 移除锁 Hook
// ----------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    ParallelRemoveActorLock,
    ll::memory::HookPriority::Normal,
    Level,
    static_cast<LevelRemoveByActor>(&Level::$removeEntity),
    ::OwnerPtr<::EntityContext>,
    ::Actor& actor
) {
    auto& pt = parallel_tick::ParallelTick::getInstance();
    auto& conf = pt.getConfig();
    std::unique_lock<std::recursive_mutex> lock(pt.getLifecycleMutex());
    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelRemoveActorLock] Enter, actor={:p}", currentTimeString(), (void*)&actor);
    }
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
    auto& pt = parallel_tick::ParallelTick::getInstance();
    auto& conf = pt.getConfig();
    std::unique_lock<std::recursive_mutex> lock(pt.getLifecycleMutex());
    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelRemoveWeakRefLock] Enter", currentTimeString());
    }
    if (Actor* actor = tryGetActorFromWeakRef(entityRef)) {
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelRemoveWeakRefLock] Found actor={:p}, removing", currentTimeString(), (void*)actor);
        }
        pt.onActorRemoved(actor);
    } else {
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelRemoveWeakRefLock] Failed to get actor from WeakEntityRef", currentTimeString());
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
        pt.getSelf().getLogger().info("[{}][ParallelActorTickHook] Enter, this={:p}, isPlayer={}, isSimulatedPlayer={}, enabled={}, collecting={}",
                                      currentTimeString(), (void*)this, this->isPlayer(), this->isSimulatedPlayer(), conf.enabled, pt.isCollecting());
    }

    if (!conf.enabled || !pt.isCollecting()) {
        auto result = origin(region);
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelActorTickHook] Skipped (disabled or not collecting), returning", currentTimeString());
        }
        return result;
    }

    if (this->isPlayer() || this->isSimulatedPlayer()) {
        auto result = origin(region);
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelActorTickHook] Player tick, calling origin", currentTimeString());
        }
        return result;
    }

    pt.collectActor(this, region);
    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelActorTickHook] Collected actor={:p}, region={:p}", currentTimeString(), (void*)this, (void*)&region);
    }
    return true;
}

// ----------------------------------------------------------------
// Level::$tick 外层 Hook — 动态分区并行
// ----------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    ParallelLevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    // 线程局部重入保护
    static thread_local bool inTick = false;
    if (inTick) {
        origin();
        return;
    }
    inTick = true;

    auto& pt   = parallel_tick::ParallelTick::getInstance();
    auto  conf = pt.getConfig();

    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelLevelTickHook] Enter, enabled={}", currentTimeString(), conf.enabled);
    }

    if (!conf.enabled) {
        origin();
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelLevelTickHook] Disabled, exit", currentTimeString());
        }
        inTick = false;
        return;
    }

    pt.setCollecting(true);
    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelLevelTickHook] Collecting set to true, calling origin", currentTimeString());
    }
    origin();
    pt.setCollecting(false);
    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelLevelTickHook] Collecting set to false, taking queue", currentTimeString());
    }

    auto list = pt.takeQueue();

    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelTick] Queue size: {}", currentTimeString(), list.size());
    }

    if (list.empty()) {
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelTick] Queue empty, clearing live", currentTimeString());
        }
        pt.clearLive();
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelLevelTickHook] Exit (empty)", currentTimeString());
        }
        inTick = false;
        return;
    }

    std::vector<parallel_tick::ActorTickEntry> valid;
    valid.reserve(list.size());
    for (auto& e : list) {
        bool alive = e.actor && pt.isActorAlive(e.actor);
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelTick] Filtering actor={:p}, alive={}", currentTimeString(), (void*)e.actor, alive);
        }
        if (alive) {
            valid.push_back(e);
        }
    }

    if (valid.empty()) {
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelTick] No valid actors, clearing live", currentTimeString());
        }
        pt.clearLive();
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelLevelTickHook] Exit (no valid)", currentTimeString());
        }
        inTick = false;
        return;
    }

    // ============= 动态分区开始 =============
    // 1. 构建网格映射
    std::unordered_map<parallel_tick::GridPos, std::vector<parallel_tick::ActorTickEntry>, parallel_tick::GridPosHash> gridMap;
    for (auto& entry : valid) {
        auto pos = entry.actor->getPosition();
        int gx = static_cast<int>(std::floor(pos.x / conf.gridSizeBase));
        int gz = static_cast<int>(std::floor(pos.z / conf.gridSizeBase));
        gridMap[{gx, gz}].push_back(entry);
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelTick] Grid assign: actor={:p} -> ({},{})", currentTimeString(), (void*)entry.actor, gx, gz);
        }
    }

    // 2. 标记网格是否已分配
    std::unordered_set<parallel_tick::GridPos, parallel_tick::GridPosHash> visited;
    std::vector<std::vector<parallel_tick::ActorTickEntry>> tasks; // 每个元素是一个任务块

    for (auto& [gridPos, entries] : gridMap) {
        if (visited.count(gridPos)) continue;

        // BFS合并
        std::queue<parallel_tick::GridPos> q;
        q.push(gridPos);
        visited.insert(gridPos);
        std::vector<parallel_tick::ActorTickEntry> taskBlock;
        taskBlock.insert(taskBlock.end(), entries.begin(), entries.end());

        while (!q.empty()) {
            parallel_tick::GridPos cur = q.front(); q.pop();
            // 检查四邻域
            for (int dx = -1; dx <= 1; dx += 2) {
                parallel_tick::GridPos neighbor{cur.x + dx, cur.z};
                auto it = gridMap.find(neighbor);
                if (it != gridMap.end() && !visited.count(neighbor)) {
                    if (taskBlock.size() + it->second.size() <= (size_t)conf.maxEntitiesPerTask) {
                        taskBlock.insert(taskBlock.end(), it->second.begin(), it->second.end());
                        visited.insert(neighbor);
                        q.push(neighbor);
                    }
                }
            }
            for (int dz = -1; dz <= 1; dz += 2) {
                parallel_tick::GridPos neighbor{cur.x, cur.z + dz};
                auto it = gridMap.find(neighbor);
                if (it != gridMap.end() && !visited.count(neighbor)) {
                    if (taskBlock.size() + it->second.size() <= (size_t)conf.maxEntitiesPerTask) {
                        taskBlock.insert(taskBlock.end(), it->second.begin(), it->second.end());
                        visited.insert(neighbor);
                        q.push(neighbor);
                    }
                }
            }
        }
        tasks.push_back(std::move(taskBlock));
    }

    // 3. 统计
    pt.addStats(valid.size(), tasks.size(), 0);

    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelTick] Generated {} tasks from {} entities", currentTimeString(), tasks.size(), valid.size());
    }

    // 4. 提交任务
    auto& pool = pt.getPool();
    for (auto& taskBlock : tasks) {
        pool.submit([&pt, conf, taskBlock = std::move(taskBlock)]() mutable {
            // 任务块内串行tick，持有递归锁防止实体被移除
            std::unique_lock<std::recursive_mutex> lock(pt.getLifecycleMutex());

            for (auto& entry : taskBlock) {
                if (!entry.actor || !pt.isActorAlive(entry.actor)) continue;

                BlockSource& region = entry.actor->getDimension().getBlockSourceFromMainChunkSource();
                auto typeId   = (int)entry.actor->getEntityTypeId();
                auto entityId = entry.actor->getRuntimeID().rawID;
                auto pos      = entry.actor->getPosition();
                auto dimId    = entry.actor->getDimensionId();

                if (conf.debug) {
                    pt.getSelf().getLogger().info(
                        "[{}][ParallelTick][Task] Starting tick: typeId={}, id={}, actor={:p}, region={:p}",
                        currentTimeString(), typeId, entityId, (void*)entry.actor, (void*)&region
                    );
                }

                try {
                    entry.actor->tick(region);
                    if (conf.debug) {
                        pt.getSelf().getLogger().info(
                            "[{}][ParallelTick][Task] Finished tick: typeId={}, id={}",
                            currentTimeString(), typeId, entityId
                        );
                    }
                } catch (const std::exception& e) {
                    pt.getSelf().getLogger().error(
                        "[{}][ParallelTick][Task] Exception during tick: typeId={}, id={}, what={}, pos=({:.2f},{:.2f},{:.2f}), dim={}",
                        currentTimeString(), typeId, entityId, e.what(), pos.x, pos.y, pos.z, (int)dimId
                    );
                } catch (...) {
                    pt.getSelf().getLogger().error(
                        "[{}][ParallelTick][Task] Unknown exception during tick: typeId={}, id={}, pos=({:.2f},{:.2f},{:.2f}), dim={}",
                        currentTimeString(), typeId, entityId, pos.x, pos.y, pos.z, (int)dimId
                    );
                }
            }
        });
    }

    // 5. 等待所有任务完成
    pool.waitAll();

    pt.clearLive();
    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelLevelTickHook] Exit", currentTimeString());
    }

    inTick = false;
}

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
