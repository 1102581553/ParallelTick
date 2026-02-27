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
    // 先从存活集合移除（写锁保护）
    pt.onActorRemoved(&actor);
    // 再写锁保护原始移除操作
    std::unique_lock lock(pt.getLifecycleMutex());
    return origin(actor);
}

// 辅助函数：从 WeakEntityRef 获取 Actor 指针
// 利用 WeakEntityRef::lock() 返回 StackRefResult<EntityContext>
static Actor* tryGetActorFromWeakRef(WeakEntityRef const& ref) {
    if (auto stackRef = ref.lock()) {
        // StackRefResult 提供 operator->() 返回 EntityContext*
        if (EntityContext* ctx = stackRef.operator->()) {
            // Actor::tryGetFromEntity 第二个参数 false 表示不包含已移除的实体
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
    // 尝试获取 Actor 指针并清理 mLiveActors
    if (Actor* actor = tryGetActorFromWeakRef(entityRef)) {
        pt.onActorRemoved(actor);
    }
    // 写锁保护原始移除操作
    std::unique_lock lock(pt.getLifecycleMutex());
    return origin(std::move(entityRef));
}

// ----------------------------------------------------------------
// Actor::tick 拦截 Hook — 收集阶段把 actor 放入队列
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

    pt.collectActor(this, region);  // 内部写锁
    return true;
}

// ----------------------------------------------------------------
// Level::$tick 外层 Hook — 开关收集窗口 + 并行派发
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

    // 开启收集窗口，执行原始 tick（Actor::tick hook 在此期间收集实体）
    pt.setCollecting(true);
    origin();
    pt.setCollecting(false);

    // 取走队列（写锁），mLiveActors 此时仍有效
    auto list = pt.takeQueue();

    if (conf.debug) {
        pt.getSelf().getLogger().info("Queue size: {}", list.size());
    }

    if (list.empty()) {
        pt.clearLive();  // 写锁清空
        return;
    }

    // 过滤掉在 origin() 执行期间被删除的 actor（使用读锁检查）
    std::vector<parallel_tick::ActorTickEntry> valid;
    valid.reserve(list.size());
    for (auto& e : list) {
        if (e.actor && pt.isActorAlive(e.actor)) {  // isActorAlive 内部读锁
            valid.push_back(e);
        }
    }

    if (valid.empty()) {
        pt.clearLive();
        return;
    }

    // 四色棋盘分组，同组内实体空间上不相邻，可安全并行
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
                // 在任务开始时获取读锁，确保在 tick 期间实体不会被移除
                std::shared_lock lock(pt.getLifecycleMutex());
                for (size_t j = 0; j < batchCount; ++j) {
                    auto& entry = batchBegin[j];
                    // 执行前再次确认 actor 仍然存活（此时已在读锁内）
                    if (!entry.actor || !pt.isActorAlive(entry.actor)) continue;

                    if (conf.debug) {
                        auto typeId   = (int)entry.actor->getEntityTypeId();
                        auto entityId = entry.actor->getRuntimeID().rawID;
                        pt.getSelf().getLogger().info(
                            "Ticking typeId={} id={}", typeId, entityId
                        );
                        entry.actor->tick(*entry.region);
                        pt.getSelf().getLogger().info(
                            "Done    typeId={} id={}", typeId, entityId
                        );
                    } else {
                        entry.actor->tick(*entry.region);
                    }
                }
            });
        }

        // 等待本 phase 所有批次完成再进入下一 phase
        pool.waitAll();
    }

    // 全部 phase 执行完毕，清空存活集合
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
