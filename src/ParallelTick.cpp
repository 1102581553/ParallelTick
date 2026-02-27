#include "ParallelTick.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <ll/api/mod/RegisterHelper.h>

#include <mc/world/level/Level.h>
#include <mc/server/ServerLevel.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/actor/ActorType.h>
#include <mc/deps/ecs/gamerefs_entity/EntityContext.h>
#include <mc/deps/ecs/gamerefs_entity/GameRefsEntity.h>
#include <mc/deps/ecs/WeakEntityRef.h>
#include <mc/legacy/ActorRuntimeID.h>

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
    if (mConfig.debug) startDebugTask();
    return true;
}

bool ParallelTick::disable() {
    stopDebugTask();
    return true;
}

} // namespace parallel_tick

// --- 生命周期写锁 Hooks ---

LL_AUTO_TYPE_INSTANCE_HOOK(
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

LL_AUTO_TYPE_INSTANCE_HOOK(
    ParallelRemoveActorLock,
    ll::memory::HookPriority::Normal,
    Level,
    static_cast<LevelRemoveByActor>(&Level::$removeEntity),
    ::OwnerPtr<::EntityContext>,
    ::Actor& actor
) {
    std::unique_lock lock(parallel_tick::ParallelTick::getInstance().getLifecycleMutex());
    return origin(actor);
}

LL_AUTO_TYPE_INSTANCE_HOOK(
    ParallelRemoveWeakRefLock,
    ll::memory::HookPriority::Normal,
    Level,
    static_cast<LevelRemoveByWeakRef>(&Level::$removeEntity),
    ::OwnerPtr<::EntityContext>,
    ::WeakEntityRef entityRef
) {
    std::unique_lock lock(parallel_tick::ParallelTick::getInstance().getLifecycleMutex());
    return origin(std::move(entityRef));
}

// --- normalTick 拦截 Hook ---

LL_AUTO_TYPE_INSTANCE_HOOK(
    ParallelNormalTickHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$normalTick,
    void
) {
    auto& pt   = parallel_tick::ParallelTick::getInstance();
    auto& conf = pt.getConfig();

    pt.getSelf().getLogger().info(
        "NormalTick hook hit, collecting={}, isPlayer={}",
        pt.isCollecting(), this->isPlayer()
    );

    if (!conf.enabled || !pt.isCollecting()) {
        origin();
        return;
    }

    if (this->isPlayer() || this->isSimulatedPlayer()) {
        origin();
        return;
    }

    pt.collectActor(this);
}

// --- 并行派发核心 Hook ---

LL_AUTO_TYPE_INSTANCE_HOOK(
    ParallelTickDispatchHook,
    ll::memory::HookPriority::Normal,
    ServerLevel,
    &ServerLevel::$tickEntities,
    void
) {
    auto& pt   = parallel_tick::ParallelTick::getInstance();
    auto  conf = pt.getConfig();

    pt.getSelf().getLogger().info("tickEntities called"); // 临时

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

    if (list.empty()) return;

    struct Groups { std::vector<Actor*> phase[4]; } groups;

    for (Actor* actor : list) {
        if (!actor) continue;
        auto const& pos = actor->getPosition();
        int gx    = static_cast<int>(std::floor(pos.x / conf.gridSize));
        int gz    = static_cast<int>(std::floor(pos.z / conf.gridSize));
        int color = (std::abs(gx) % 2) | ((std::abs(gz) % 2) << 1);
        groups.phase[color].push_back(actor);
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

        for (size_t i = 0; i < phaseList.size(); i += conf.batchSize) {
            size_t  end        = std::min(i + (size_t)conf.batchSize, phaseList.size());
            Actor** batchBegin = phaseList.data() + i;
            size_t  batchCount = end - i;

            pool.submit([&pt, conf, batchBegin, batchCount] {
                std::shared_lock lock(pt.getLifecycleMutex());
                for (size_t j = 0; j < batchCount; ++j) {
                    Actor* actor = batchBegin[j];
                    if (!actor) continue;
                    if (conf.debug) {
                        auto typeId   = (int)actor->getEntityTypeId();
                        auto entityId = actor->getRuntimeID().rawID;
                        pt.getSelf().getLogger().info(
                            "Ticking typeId={} id={}", typeId, entityId
                        );
                        actor->normalTick();
                        pt.getSelf().getLogger().info(
                            "Done    typeId={} id={}", typeId, entityId
                        );
                    } else {
                        actor->normalTick();
                    }
                }
            });
        }

        pool.waitAll();
    }
}

LL_REGISTER_MOD(parallel_tick::ParallelTick, parallel_tick::ParallelTick::getInstance());
