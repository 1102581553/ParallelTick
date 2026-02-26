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

#include <cmath>
#include <thread>
#include <vector>
#include <future>

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
                        total, (p0 + p1 + p2 + p3), p0, p1, p2, p3, u
                    );
                }
            });
        }
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

void ParallelTick::stopDebugTask() { mDebugTaskRunning.store(false); }

bool ParallelTick::load() {
    auto path = getSelf().getConfigDir() / "config.json";
    if (!ll::config::loadConfig(mConfig, path)) {
        ll::config::saveConfig(mConfig, path);
    }
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

// --- 并行派发核心 Hook ---
// Hook ServerLevel::$tickEntities，这是专门负责 tick 所有实体的函数
// 符号为 MCAPI，链接不会失败
#pragma warning(push)
#pragma warning(disable: 4996)
LL_AUTO_TYPE_INSTANCE_HOOK(
    ParallelTickDispatchHook,
    ll::memory::HookPriority::Normal,
    ServerLevel,
    &ServerLevel::$tickEntities,
    void
) {
    auto& pt   = parallel_tick::ParallelTick::getInstance();
    auto& conf = pt.getConfig();

    if (!conf.enabled) {
        origin();
        return;
    }

    // getRuntimeActorList() 直接从 this 调用，不需要 ll::service::getLevel()
    parallel_tick::ParallelGroups groups;

    {
        std::shared_lock lock(pt.getLifecycleMutex());
        for (Actor* actor : this->getRuntimeActorList()) {
            if (!actor) continue;

            bool isDangerous = actor->isPlayer() || actor->isSimulatedPlayer();
            if (conf.parallelItemsOnly
                && actor->getEntityTypeId() != ActorType::ItemEntity
                && actor->getEntityTypeId() != ActorType::Experience) {
                isDangerous = true;
            }

            if (isDangerous) {
                groups.unsafe.push_back(actor);
            } else {
                auto const& pos = actor->getPosition();
                int gx    = static_cast<int>(std::floor(pos.x / conf.gridSize));
                int gz    = static_cast<int>(std::floor(pos.z / conf.gridSize));
                int color = (std::abs(gx) % 2) | ((std::abs(gz) % 2) << 1);
                groups.phase[color].push_back(actor);
            }
        }
    }

    if (conf.debug) {
        pt.addStats(
            (int)groups.phase[0].size(), (int)groups.phase[1].size(),
            (int)groups.phase[2].size(), (int)groups.phase[3].size(),
            (int)groups.unsafe.size()
        );
    }

    // 并行阶段（同色格子间无空间相邻，可以安全并行）
    for (int p = 0; p < 4; ++p) {
        auto& list = groups.phase[p];
        if (list.empty()) continue;

        std::vector<std::future<void>> futures;

        for (size_t i = 0; i < list.size(); i += conf.batchSize) {
            size_t  end        = std::min(i + static_cast<size_t>(conf.batchSize), list.size());
            Actor** batchBegin = list.data() + i;
            size_t  batchCount = end - i;

            futures.push_back(std::async(std::launch::async,
                [&pt, batchBegin, batchCount]() {
                    std::shared_lock lock(pt.getLifecycleMutex());
                    for (size_t j = 0; j < batchCount; ++j) {
                        Actor* actor = batchBegin[j];
                        if (!actor) continue;
                        
                        // 崩溃前最后输出的就是问题实体
                        auto typeId   = (int)actor->getEntityTypeId();
                        auto entityId = actor->getRuntimeID().rawID;
                        
                        parallel_tick::ParallelTick::getInstance().getSelf().getLogger().debug(
                            "Ticking entity: typeId={} runtimeId={}", typeId, entityId
                        );
                        
                        actor->normalTick();
                        
                        parallel_tick::ParallelTick::getInstance().getSelf().getLogger().debug(
                            "Done entity: typeId={} runtimeId={}", typeId, entityId
                        );
                    }
                }
            ));

        }

        for (auto& f : futures) f.get();
    }

    // 串行阶段（玩家、模拟玩家等需要主线程的实体）
    for (auto* actor : groups.unsafe) {
        if (actor) actor->normalTick();
    }
}
#pragma warning(pop)

LL_REGISTER_MOD(parallel_tick::ParallelTick, parallel_tick::ParallelTick::getInstance());
