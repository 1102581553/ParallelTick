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
#include <mc/world/level/dimension/Dimension.h>  // 提供 Dimension 完整定义

#include <cmath>
#include <vector>

using LevelRemoveByActor   = ::OwnerPtr<::EntityContext> (Level::*)(::Actor&);
using LevelRemoveByWeakRef = ::OwnerPtr<::EntityContext> (Level::*)(::WeakEntityRef);

namespace parallel_tick {

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
                        "Parallel Stats (5s): Total={}, Parallel={}({}/{}/{}/{}), Serial={}",
                        total, (p0+p1+p2+p3), p0, p1, p2, p3, u
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
    return true;
}

bool ParallelTick::enable() {
    registerHooks();
    if (mConfig.debug) startDebugTask();
    return true;
}

bool ParallelTick::disable() {
    unregisterHooks();
    stopDebugTask();
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
    std::unique_lock lock(parallel_tick::ParallelTick::getInstance().getLifecycleMutex());
    return origin(region, std::move(entity));
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
    pt.onActorRemoved(&actor);
    std::unique_lock lock(pt.getLifecycleMutex());
    return origin(actor);
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
    if (Actor* actor = tryGetActorFromWeakRef(entityRef)) {
        pt.onActorRemoved(actor);
    }
    std::unique_lock lock(pt.getLifecycleMutex());
    return origin(std::move(entityRef));
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

    if (!conf.enabled || !pt.isCollecting()) {
        return origin(region);
    }

    if (this->isPlayer() || this->isSimulatedPlayer()) {
        return origin(region);
    }

    pt.collectActor(this, region);
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

    if (!conf.enabled) {
        origin();
        return;
    }

    pt.setCollecting(true);
    origin();
    pt.setCollecting(false);

    auto list = pt.takeQueue();

    if (conf.debug) {
        pt.getSelf().getLogger().info("Queue size: {}", list.size());
    }

    if (list.empty()) {
        pt.clearLive();
        return;
    }

    std::vector<parallel_tick::ActorTickEntry> valid;
    valid.reserve(list.size());
    for (auto& e : list) {
        if (e.actor && pt.isActorAlive(e.actor)) {
            valid.push_back(e);
        }
    }

    if (valid.empty()) {
        pt.clearLive();
        return;
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
    }

    if (conf.debug) {
        pt.addStats(
            (int)groups.phase[0].size(), (int)groups.phase[1].size(),
            (int)groups.phase[2].size(), (int)groups.phase[3].size(), 0
        );
    }

    auto& pool = pt.getPool();

    for (int p = 0; p < 4; ++p) {
        auto& phaseList = groups.phase[p];
        if (phaseList.empty()) continue;

        for (size_t i = 0; i < phaseList.size(); i += (size_t)conf.batchSize) {
            size_t end        = std::min(i + (size_t)conf.batchSize, phaseList.size());
            auto*  batchBegin = phaseList.data() + i;
            size_t batchCount = end - i;

            pool.submit([&pt, conf, batchBegin, batchCount] {
                std::shared_lock lock(pt.getLifecycleMutex());

                for (size_t j = 0; j < batchCount; ++j) {
                    auto& entry = batchBegin[j];
                    if (!entry.actor || !pt.isActorAlive(entry.actor)) continue;

                    // 动态获取当前有效的 BlockSource（使用 Dimension 的 getBlockSourceFromMainChunkSource）
                    BlockSource& region = entry.actor->getDimension().getBlockSourceFromMainChunkSource();

                    auto typeId   = (int)entry.actor->getEntityTypeId();
                    auto entityId = entry.actor->getRuntimeID().rawID;
                    pt.getSelf().getLogger().info(
                        "[ParallelTick] Starting tick: typeId={}, id={}", typeId, entityId
                    );

                    try {
                        entry.actor->tick(region);
                    } catch (...) {
                        pt.getSelf().getLogger().error(
                            "[ParallelTick] Exception during tick: typeId={}, id={}", typeId, entityId
                        );
                        throw;
                    }

                    if (conf.debug) {
                        pt.getSelf().getLogger().info(
                            "[ParallelTick] Finished tick: typeId={}, id={}", typeId, entityId
                        );
                    }
                }
            });
        }

        pool.waitAll();
    }

    pt.clearLive();
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
