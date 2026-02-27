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

// 辅助函数：获取当前时间字符串（用于日志）
static std::string currentTimeString() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%H:%M:%S");
    return ss.str();
}

ParallelTick& ParallelTick::getInstance() {
    static ParallelTick instance;
    return instance;
}

void ParallelTick::startDebugTask() {
    if (mDebugTaskRunning.exchange(true)) return;
    ll::coro::keepThis([this]() -> ll::coro::CoroTask<> {
        while (mDebugTaskRunning.load()) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([this] {
                if (!mConfig.debug) return;
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

void ParallelTick::stopDebugTask() { mDebugTaskRunning.store(false); }

bool ParallelTick::load() {
    auto path = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(mConfig, path))
        ll::config::saveConfig(mConfig, path);
    getSelf().getLogger().info("[{}][ParallelTick::load] Config loaded, enabled={}, debug={}", 
                               currentTimeString(), mConfig.enabled, mConfig.debug);
    return true;
}

bool ParallelTick::enable() {
    registerHooks();
    getSelf().getLogger().info("[{}][ParallelTick::enable] Hooks registered", currentTimeString());
    if (mConfig.debug) startDebugTask();
    return true;
}

bool ParallelTick::disable() {
    unregisterHooks();
    stopDebugTask();
    getSelf().getLogger().info("[{}][ParallelTick::disable] Hooks unregistered", currentTimeString());
    return true;
}

// ----------------------------------------------------------------
// 生命周期写锁 Hooks
// ----------------------------------------------------------------

LL_TYPE_INSTANCE_HOOK(
    ParallelAddEntityLock,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$addEntity,
    ::Actor*,
    ::BlockSource& region,
    ::OwnerPtr<::EntityContext> entity
) {
    auto& pt = parallel_tick::ParallelTick::getInstance();
    pt.getSelf().getLogger().debug("[{}][ParallelAddEntityLock] Enter, acquiring write lock", currentTimeString());
    std::unique_lock lock(pt.getLifecycleMutex());
    auto result = origin(region, std::move(entity));
    pt.getSelf().getLogger().debug("[{}][ParallelAddEntityLock] Exit, lock released", currentTimeString());
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    ParallelRemoveActorLock,
    ll::memory::HookPriority::Normal,
    Level,
    static_cast<LevelRemoveByActor>(&Level::$removeEntity),
    ::OwnerPtr<::EntityContext>,
    ::Actor& actor
) {
    auto& pt = parallel_tick::ParallelTick::getInstance();
    pt.getSelf().getLogger().debug("[{}][ParallelRemoveActorLock] Enter, actor={:p}", currentTimeString(), (void*)&actor);
    pt.onActorRemoved(&actor);
    std::unique_lock lock(pt.getLifecycleMutex());
    auto result = origin(actor);
    pt.getSelf().getLogger().debug("[{}][ParallelRemoveActorLock] Exit", currentTimeString());
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
    pt.getSelf().getLogger().debug("[{}][ParallelRemoveWeakRefLock] Enter", currentTimeString());
    if (Actor* actor = tryGetActorFromWeakRef(entityRef)) {
        pt.getSelf().getLogger().debug("[{}][ParallelRemoveWeakRefLock] Found actor={:p}, removing", currentTimeString(), (void*)actor);
        pt.onActorRemoved(actor);
    } else {
        pt.getSelf().getLogger().debug("[{}][ParallelRemoveWeakRefLock] Failed to get actor from WeakEntityRef", currentTimeString());
    }
    std::unique_lock lock(pt.getLifecycleMutex());
    auto result = origin(std::move(entityRef));
    pt.getSelf().getLogger().debug("[{}][ParallelRemoveWeakRefLock] Exit", currentTimeString());
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

    pt.getSelf().getLogger().debug("[{}][ParallelActorTickHook] Enter, this={:p}, isPlayer={}, isSimulatedPlayer={}, enabled={}, collecting={}",
                                   currentTimeString(), (void*)this, this->isPlayer(), this->isSimulatedPlayer(), conf.enabled, pt.isCollecting());

    if (!conf.enabled || !pt.isCollecting()) {
        auto result = origin(region);
        pt.getSelf().getLogger().debug("[{}][ParallelActorTickHook] Skipped (disabled or not collecting), returning", currentTimeString());
        return result;
    }

    if (this->isPlayer() || this->isSimulatedPlayer()) {
        auto result = origin(region);
        pt.getSelf().getLogger().debug("[{}][ParallelActorTickHook] Player tick, calling origin", currentTimeString());
        return result;
    }

    pt.collectActor(this, region);
    pt.getSelf().getLogger().debug("[{}][ParallelActorTickHook] Collected actor={:p}, region={:p}", currentTimeString(), (void*)this, (void*)&region);
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
    auto& pt   = parallel_tick::ParallelTick::getInstance();
    auto  conf = pt.getConfig();

    pt.getSelf().getLogger().debug("[{}][ParallelLevelTickHook] Enter, enabled={}", currentTimeString(), conf.enabled);

    if (!conf.enabled) {
        origin();
        pt.getSelf().getLogger().debug("[{}][ParallelLevelTickHook] Disabled, exit", currentTimeString());
        return;
    }

    pt.setCollecting(true);
    pt.getSelf().getLogger().debug("[{}][ParallelLevelTickHook] Collecting set to true, calling origin", currentTimeString());
    origin();
    pt.setCollecting(false);
    pt.getSelf().getLogger().debug("[{}][ParallelLevelTickHook] Collecting set to false, taking queue", currentTimeString());

    auto list = pt.takeQueue();

    if (conf.debug) {
        pt.getSelf().getLogger().info("[{}][ParallelTick] Queue size: {}", currentTimeString(), list.size());
    } else {
        pt.getSelf().getLogger().debug("[{}][ParallelTick] Queue size: {}", currentTimeString(), list.size());
    }

    if (list.empty()) {
        pt.getSelf().getLogger().debug("[{}][ParallelTick] Queue empty, clearing live", currentTimeString());
        pt.clearLive();
        pt.getSelf().getLogger().debug("[{}][ParallelLevelTickHook] Exit (empty)", currentTimeString());
        return;
    }

    std::vector<parallel_tick::ActorTickEntry> valid;
    valid.reserve(list.size());
    for (auto& e : list) {
        bool alive = e.actor && pt.isActorAlive(e.actor);
        pt.getSelf().getLogger().debug("[{}][ParallelTick] Filtering actor={:p}, alive={}", currentTimeString(), (void*)e.actor, alive);
        if (alive) {
            valid.push_back(e);
        }
    }

    if (valid.empty()) {
        pt.getSelf().getLogger().debug("[{}][ParallelTick] No valid actors, clearing live", currentTimeString());
        pt.clearLive();
        pt.getSelf().getLogger().debug("[{}][ParallelLevelTickHook] Exit (no valid)", currentTimeString());
        return;
    }

    pt.getSelf().getLogger().info("[{}][ParallelTick] Valid actors after filtering: {}", currentTimeString(), valid.size());

    struct Groups {
        std::vector<parallel_tick::ActorTickEntry> phase[4];
    } groups;

    for (auto& entry : valid) {
        auto const& pos = entry.actor->getPosition();
        int gx    = static_cast<int>(std::floor(pos.x / conf.gridSize));
        int gz    = static_cast<int>(std::floor(pos.z / conf.gridSize));
        int color = (std::abs(gx) % 2) | ((std::abs(gz) % 2) << 1);
        groups.phase[color].push_back(entry);
        pt.getSelf().getLogger().debug("[{}][ParallelTick] Grouping: actor={:p}, pos=({},{},{}) -> color={}", 
                                       currentTimeString(), (void*)entry.actor, pos.x, pos.y, pos.z, color);
    }

    if (conf.debug) {
        pt.addStats(
            (int)groups.phase[0].size(), (int)groups.phase[1].size(),
            (int)groups.phase[2].size(), (int)groups.phase[3].size(), 0
        );
        pt.getSelf().getLogger().info("[{}][ParallelTick] Phase sizes: {} {} {} {}", 
                                      currentTimeString(), groups.phase[0].size(), groups.phase[1].size(), groups.phase[2].size(), groups.phase[3].size());
    }

    auto& pool = pt.getPool();

    for (int p = 0; p < 4; ++p) {
        auto& phaseList = groups.phase[p];
        if (phaseList.empty()) {
            pt.getSelf().getLogger().debug("[{}][ParallelTick] Phase {} empty, skipping", currentTimeString(), p);
            continue;
        }

        pt.getSelf().getLogger().info("[{}][ParallelTick] Processing phase {} with {} actors", currentTimeString(), p, phaseList.size());

        for (size_t i = 0; i < phaseList.size(); i += (size_t)conf.batchSize) {
            size_t end        = std::min(i + (size_t)conf.batchSize, phaseList.size());
            auto*  batchBegin = phaseList.data() + i;
            size_t batchCount = end - i;

            pt.getSelf().getLogger().debug("[{}][ParallelTick] Submitting batch: phase={}, index={}-{}, count={}", 
                                           currentTimeString(), p, i, end-1, batchCount);

            pool.submit([&pt, conf, batchBegin, batchCount, p, i] {
                std::shared_lock lock(pt.getLifecycleMutex());
                pt.getSelf().getLogger().debug("[{}][ParallelTick][Task] Batch started: phase={}, index={}, count={}", 
                                               currentTimeString(), p, i, batchCount);

                for (size_t j = 0; j < batchCount; ++j) {
                    auto& entry = batchBegin[j];
                    if (!entry.actor) {
                        pt.getSelf().getLogger().warn("[{}][ParallelTick][Task] Skipping null actor in batch", currentTimeString());
                        continue;
                    }

                    bool alive = pt.isActorAlive(entry.actor);
                    pt.getSelf().getLogger().debug("[{}][ParallelTick][Task] Before tick: actor={:p}, alive={}", 
                                                   currentTimeString(), (void*)entry.actor, alive);
                    if (!alive) continue;

                    // 动态获取当前有效的 BlockSource
                    BlockSource& region = entry.actor->getDimension().getBlockSourceFromMainChunkSource();

                    auto typeId   = (int)entry.actor->getEntityTypeId();
                    auto entityId = entry.actor->getRuntimeID().rawID;
                    pt.getSelf().getLogger().info(
                        "[{}][ParallelTick][Task] Starting tick: typeId={}, id={}, actor={:p}, region={:p}",
                        currentTimeString(), typeId, entityId, (void*)entry.actor, (void*)&region
                    );

                    try {
                        entry.actor->tick(region);
                        pt.getSelf().getLogger().debug(
                            "[{}][ParallelTick][Task] Finished tick: typeId={}, id={}",
                            currentTimeString(), typeId, entityId
                        );
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
                pt.getSelf().getLogger().debug("[{}][ParallelTick][Task] Batch finished: phase={}, index={}", currentTimeString(), p, i);
            });
        }

        pt.getSelf().getLogger().debug("[{}][ParallelTick] Waiting for phase {} tasks to complete", currentTimeString(), p);
        pool.waitAll();
        pt.getSelf().getLogger().debug("[{}][ParallelTick] Phase {} completed", currentTimeString(), p);
    }

    pt.getSelf().getLogger().debug("[{}][ParallelTick] All phases done, clearing live", currentTimeString());
    pt.clearLive();
    pt.getSelf().getLogger().debug("[{}][ParallelLevelTickHook] Exit", currentTimeString());
}

// ----------------------------------------------------------------
// 注册 / 注销
// ----------------------------------------------------------------

void parallel_tick::registerHooks() {
    ParallelAddEntityLock::hook();
    ParallelRemoveActorLock::hook();
    ParallelRemoveWeakRefLock::hook();
    ParallelActorTickHook::hook();
    ParallelLevelTickHook::hook();
}

void parallel_tick::unregisterHooks() {
    ParallelAddEntityLock::unhook();
    ParallelRemoveActorLock::unhook();
    ParallelRemoveWeakRefLock::unhook();
    ParallelActorTickHook::unhook();
    ParallelLevelTickHook::unhook();
}

LL_REGISTER_MOD(parallel_tick::ParallelTick, parallel_tick::ParallelTick::getInstance());

} // namespace parallel_tick
