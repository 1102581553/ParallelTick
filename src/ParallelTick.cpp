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
#include <mc/deps/ecs/EntityRegistry.h>
#include <mc/world/actor/ActorTickNeededComponent.h>

namespace parallel_tick {

ParallelTick& ParallelTick::getInstance() {
    static ParallelTick instance;
    return instance;
}

// 调试任务：每5秒输出一次统计信息 [cite: 497, 498]
void ParallelTick::startDebugTask() {
    if (mDebugTaskRunning) return;
    mDebugTaskRunning = true;

    ll::coro::keepThis([this]() -> ll::coro::CoroTask<> {
        while (mDebugTaskRunning) {
            co_await std::chrono::seconds(5); // 等待5秒
            
            // 切换到主线程执行日志输出，避免竞争
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

void ParallelTick::stopDebugTask() { mDebugTaskRunning = false; }

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

// --- 生命周期锁 Hook ---
LL_AUTO_TYPE_INSTANCE_HOOK(ParallelAddEntityLock, ll::memory::HookPriority::Normal, Level, &Level::addEntity, 
    Actor*, BlockSource& region, OwnerPtr<EntityContext> entity) {
    std::lock_guard<std::mutex> lock(parallel_tick::ParallelTick::getInstance().getLifecycleMutex());
    return origin(region, std::move(entity));
}

// --- 并行派发核心 Hook ---
LL_AUTO_TYPE_INSTANCE_HOOK(ParallelTickDispatchHook, ll::memory::HookPriority::Normal, ActorLegacyTickSystem, &ActorLegacyTickSystem::$tick, 
    void, EntityRegistry& registry) {
    
    auto& pt = parallel_tick::ParallelTick::getInstance();
    auto& conf = pt.getConfig();

    if (!conf.enabled) {
        origin(registry);
        return;
    }

    parallel_tick::ParallelGroups groups;
    registry.forEach<Actor, ActorTickNeededComponent>([&](EntityContext& ctx, Actor& actor, ActorTickNeededComponent&) {
        bool isDangerous = actor.isPlayer() || actor.isSimulatedPlayer();
        if (conf.parallelItemsOnly && actor.getEntityTypeId() != ActorType::Item && actor.getEntityTypeId() != ActorType::ExperienceOrb) {
            isDangerous = true;
        }

        if (isDangerous) {
            groups.unsafe.push_back(&ctx);
        } else {
            auto const& pos = actor.getPosition();
            int gx = static_cast<int>(std::floor(pos.x / conf.gridSize));
            int gz = static_cast<int>(std::floor(pos.z / conf.gridSize));
            int color = (std::abs(gx) % 2) | ((std::abs(gz) % 2) << 1);
            groups.phase[color].push_back(&ctx);
        }
    });

    // 记录本 Tick 统计数据 
    if (conf.debug) {
        pt.addStats((int)groups.phase[0].size(), (int)groups.phase[1].size(), 
                    (int)groups.phase[2].size(), (int)groups.phase[3].size(), (int)groups.unsafe.size());
    }

    Level* level = ll::service::getLevel().get();
    if (!level) return origin(registry);
    TaskGroup& taskGroup = level->getSyncTasksGroup();

    for (int p = 0; p < 4; ++p) {
        auto& list = groups.phase[p];
        if (list.empty()) continue;

        for (size_t i = 0; i < list.size(); i += conf.batchSize) {
            size_t end = std::min(i + conf.batchSize, list.size());
            std::vector<EntityContext*> batch(list.begin() + i, list.begin() + end);

            taskGroup.queueSync_DEPRECATED(TaskStartInfo{}, [this, &registry, batch]() -> TaskResult {
                for (auto* ctx : batch) {
                    this->singleTick(registry, *ctx); 
                }
                return {};
            });
        }
        taskGroup.sync_DEPRECATED_ASK_TOMMO([](){});
    }

    for (auto* ctx : groups.unsafe) {
        this->singleTick(registry, *ctx);
    }
}

LL_REGISTER_MOD(parallel_tick::ParallelTick, parallel_tick::ParallelTick::getInstance());
