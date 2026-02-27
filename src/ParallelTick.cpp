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
                bool outputStats = mConfig.stats;
                if (!outputStats) return;  // 仅当 stats 开启时输出

                size_t p0 = mPhaseStats[0].exchange(0);
                size_t p1 = mPhaseStats[1].exchange(0);
                size_t p2 = mPhaseStats[2].exchange(0);
                size_t p3 = mPhaseStats[3].exchange(0);
                size_t u  = mUnsafeStats.exchange(0);
                size_t total = p0 + p1 + p2 + p3 + u;

                if (total > 0) {
                    getSelf().getLogger().info(
                        "[{}][ParallelStats] Total={}, Parallel={}({}/{}/{}/{}), Serial={}",
                        currentTimeString(), total, (p0+p1+p2+p3), p0, p1, p2, p3, u
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
    getSelf().getLogger().info("[{}][ParallelTick::load] Config loaded, enabled={}, debug={}, stats={}, threadCount={}",
                               currentTimeString(), mConfig.enabled, mConfig.debug, mConfig.stats, mConfig.threadCount);
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
// Hooks
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

LL_TYPE_INSTANCE_HOOK(
    ParallelLevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
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
        return;
    }

    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelTick] Valid actors after filtering: {}", currentTimeString(), valid.size());
    }

    struct Groups {
        std::vector<parallel_tick::ActorTickEntry> phase[4];
    } groups;

    for (auto& entry : valid) {
        auto const& pos = entry.actor->getPosition();
        int gx    = static_cast<int>(std::floor(pos.x / conf.gridSize));
        int gz    = static_cast<int>(std::floor(pos.z / conf.gridSize));
        int color = (std::abs(gx) % 2) | ((std::abs(gz) % 2) << 1);
        groups.phase[color].push_back(entry);
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelTick] Grouping: actor={:p}, pos=({},{},{}) -> color={}", 
                                          currentTimeString(), (void*)entry.actor, pos.x, pos.y, pos.z, color);
        }
    }

    pt.addStats(
        (int)groups.phase[0].size(), (int)groups.phase[1].size(),
        (int)groups.phase[2].size(), (int)groups.phase[3].size(), 0
    );
    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelTick] Phase sizes: {} {} {} {}", 
                                      currentTimeString(), groups.phase[0].size(), groups.phase[1].size(), groups.phase[2].size(), groups.phase[3].size());
    }

    auto& pool = pt.getPool();

    for (int p = 0; p < 4; ++p) {
        auto& phaseList = groups.phase[p];
        if (phaseList.empty()) {
            if (conf.debug) {
                pt.getSelf().getLogger().info("[{}][ParallelTick] Phase {} empty, skipping", currentTimeString(), p);
            }
            continue;
        }

        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelTick] Processing phase {} with {} actors", currentTimeString(), p, phaseList.size());
        }

        for (size_t i = 0; i < phaseList.size(); i += (size_t)conf.batchSize) {
            size_t end        = std::min(i + (size_t)conf.batchSize, phaseList.size());
            auto*  batchBegin = phaseList.data() + i;
            size_t batchCount = end - i;

            if (conf.debug) {
                pt.getSelf().getLogger().info("[{}][ParallelTick] Submitting batch: phase={}, index={}-{}, count={}", 
                                              currentTimeString(), p, i, end-1, batchCount);
            }

            pool.submit([&pt, conf, batchBegin, batchCount, p, i] {
                std::unique_lock<std::recursive_mutex> lock(pt.getLifecycleMutex());
                if (conf.debug) {
                    pt.getSelf().getLogger().info("[{}][ParallelTick][Task] Batch started: phase={}, index={}, count={}", 
                                                  currentTimeString(), p, i, batchCount);
                }

                for (size_t j = 0; j < batchCount; ++j) {
                    auto& entry = batchBegin[j];
                    if (!entry.actor) {
                        if (conf.debug) {
                            pt.getSelf().getLogger().info("[{}][ParallelTick][Task] Skipping null actor in batch", currentTimeString());
                        }
                        continue;
                    }

                    bool alive = pt.isActorAlive(entry.actor);
                    if (conf.debug) {
                        pt.getSelf().getLogger().info("[{}][ParallelTick][Task] Before tick: actor={:p}, alive={}", 
                                                      currentTimeString(), (void*)entry.actor, alive);
                    }
                    if (!alive) continue;

                    BlockSource& region = entry.actor->getDimension().getBlockSourceFromMainChunkSource();

                    auto typeId   = (int)entry.actor->getEntityTypeId();
                    auto entityId = entry.actor->getRuntimeID().rawID;
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
                            "[{}][ParallelTick][Task] Exception during tick: typeId={}, id={}, what={}",
                            currentTimeString(), typeId, entityId, e.what()
                        );
                        throw;
                    } catch (...) {
                        pt.getSelf().getLogger().error(
                            "[{}][ParallelTick][Task] Unknown exception during tick: typeId={}, id={}",
                            currentTimeString(), typeId, entityId
                        );
                        throw;
                    }
                }
                if (conf.debug) {
                    pt.getSelf().getLogger().info("[{}][ParallelTick][Task] Batch finished: phase={}, index={}", currentTimeString(), p, i);
                }
            });
        }

        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelTick] Waiting for phase {} tasks to complete", currentTimeString(), p);
        }
        pool.waitAll();
        if (conf.debug) {
            pt.getSelf().getLogger().info("[{}][ParallelTick] Phase {} completed", currentTimeString(), p);
        }
    }

    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelTick] All phases done, clearing live", currentTimeString());
    }
    pt.clearLive();
    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelLevelTickHook] Exit", currentTimeString());
    }
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
