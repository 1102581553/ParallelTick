#include "ParallelTick.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <ll/api/mod/RegisterHelper.h>

#include <mc/world/level/Level.h>
#include <mc/world/actor/Actor.h>
#include <mc/entity/systems/ActorLegacyTickSystem.h>
#include <mc/deps/core/threading/TaskGroup.h>
#include <mc/deps/ecs/gamerefs_entity/EntityRegistry.h>
#include <mc/entity/components/ActorTickNeededComponent.h>
#include <mc/entity/components/ActorOwnerComponent.h>

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
LL_AUTO_TYPE_INSTANCE_HOOK(ParallelAddEntityLock, ll::memory::HookPriority::Normal, Level, &Level::addEntity,
    Actor*, BlockSource& region, OwnerPtr<EntityContext> entity) {
    std::unique_lock lock(parallel_tick::ParallelTick::getInstance().getLifecycleMutex());
    return origin(region, std::move(entity));
}

LL_AUTO_TYPE_INSTANCE_HOOK(ParallelRemoveEntityLock, ll::memory::HookPriority::Normal, Level,
    "?removeEntity@Level@@UEAA?AV?$OwnerPtr@VEntityContext@@@@AEAVActor@@@Z",
    OwnerPtr<EntityContext>, Actor& actor) {
    std::unique_lock lock(parallel_tick::ParallelTick::getInstance().getLifecycleMutex());
    return origin(actor);
}

// --- 并行派发核心 Hook ---
LL_AUTO_TYPE_INSTANCE_HOOK(ParallelTickDispatchHook, ll::memory::HookPriority::Normal,
    ActorLegacyTickSystem, &ActorLegacyTickSystem::$tick,
    void, EntityRegistry& registry) {

    auto& pt   = parallel_tick::ParallelTick::getInstance();
    auto& conf = pt.getConfig();

    if (!conf.enabled) {
        origin(registry);
        return;
    }

    // 通过 entt view 遍历需要 tick 的实体
    parallel_tick::ParallelGroups groups;
    auto view = registry.mRegistry.view<ActorOwnerComponent, ActorTickNeededComponent>();

    for (auto entity : view) {
        auto& ownerComp = view.get<ActorOwnerComponent>(entity);
        Actor* actor = ownerComp.mActor.get();
        if (!actor) continue;

        bool isDangerous = actor->isPlayer() || actor->isSimulatedPlayer();
        if (conf.parallelItemsOnly
            && actor->getEntityTypeId() != ActorType::Item
            && actor->getEntityTypeId() != ActorType::ExperienceOrb) {
            isDangerous = true;
        }

        if (isDangerous) {
            groups.unsafe.push_back(actor);
        } else {
            auto const& pos = actor->getPosition();
            int gx = static_cast<int>(std::floor(pos.x / conf.gridSize));
            int gz = static_cast<int>(std::floor(pos.z / conf.gridSize));
            int color = (std::abs(gx) % 2) | ((std::abs(gz) % 2) << 1);
            groups.phase[color].push_back(actor);
        }
    }

    if (conf.debug) {
        pt.addStats((int)groups.phase[0].size(), (int)groups.phase[1].size(),
                    (int)groups.phase[2].size(), (int)groups.phase[3].size(),
                    (int)groups.unsafe.size());
    }

    Level* level = ll::service::getLevel();
    if (!level) { origin(registry); return; }
    TaskGroup& taskGroup = level->getSyncTasksGroup();

    // 并行阶段：持读锁
    for (int p = 0; p < 4; ++p) {
        auto& list = groups.phase[p];
        if (list.empty()) continue;

        for (size_t i = 0; i < list.size(); i += conf.batchSize) {
            size_t end = std::min(i + (size_t)conf.batchSize, list.size());
            // 用 span 避免拷贝 vector
            auto batchBegin = list.data() + i;
            auto batchSize  = end - i;

            taskGroup.queueSync_DEPRECATED(TaskStartInfo{}, [&pt, batchBegin, batchSize]() -> TaskResult {
                std::shared_lock lock(pt.getLifecycleMutex());
                for (size_t j = 0; j < batchSize; ++j) {
                    Actor* actor = batchBegin[j];
                    if (actor) {
                        actor->normalTick();
                    }
                }
                return {};
            });
        }
        // 等待当前 phase 的所有任务完成，再进入下一个 phase
        taskGroup.sync_DEPRECATED_ASK_TOMMO([](){});
    }

    // 串行阶段：unsafe 实体在主线程 tick
    for (auto* actor : groups.unsafe) {
        if (actor) {
            actor->normalTick();
        }
    }
}

LL_REGISTER_MOD(parallel_tick::ParallelTick, parallel_tick::ParallelTick::getInstance());
