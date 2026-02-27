#include "ParallelTick.h"
#include "Config.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/io/Logger.h>
#include <mc/world/level/Level.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/actor/player/Player.h>
#include <vector>
#include <chrono>

namespace parallel_tick {

// 假设 Hook Level::tick (实际可能不同，请根据 LeviLamina 版本调整)
LL_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::tick,
    void
) {
    auto& inst = ParallelTick::getInstance();
    auto& config = inst.getConfig();

    // 如果全局禁用或在并行阶段，直接调用原函数
    if (!config.enabled || inst.isParallelPhase()) {
        origin();
        return;
    }

    // 开始并行阶段
    inst.setParallelPhase(true);

    // 1. 收集所有需要并行 tick 的实体（例如所有非玩家 Mob）
    std::vector<Actor*> entities;
    this->forEachEntity([&](Actor& actor) -> bool {
        // 只处理生物 (Mob) 且不在黑名单中，排除玩家（玩家通常在主线程处理）
        if (actor.isMob() && !actor.isPlayer() && inst.isActorSafeToTick(&actor)) {
            entities.push_back(&actor);
        }
        return true;
    });

    size_t total = entities.size();
    size_t batchSize = config.maxEntitiesPerTask;
    size_t batches = (total + batchSize - 1) / batchSize;

    // 2. 提交任务到线程池
    for (size_t i = 0; i < total; i += batchSize) {
        size_t end = std::min(i + batchSize, total);
        auto task = [&inst, entities, i, end]() {
            for (size_t j = i; j < end; ++j) {
                Actor* actor = entities[j];
                try {
                    // 调用实体的 aiStep 或其他需要并行的 tick 方法
                    // 注意：确保 actor 在此时仍然有效（不会被销毁）
                    actor->aiStep();
                } catch (...) {
                    // 捕获所有异常，记录并加入黑名单
                    inst.markCrashed(actor);
                    inst.getSelf().getLogger().error(
                        "Actor {} crashed during parallel tick",
                        actor->getUniqueID().rawID
                    );
                }
            }
        };
        inst.getPool().submit(std::move(task));
    }

    // 3. 等待所有任务完成，带超时
    auto timeout = std::chrono::milliseconds(config.actorTickTimeoutMs);
    if (!inst.getPool().waitAllFor(timeout)) {
        inst.getSelf().getLogger().warn(
            "Parallel tick timeout ({} ms), some tasks may be stuck",
            config.actorTickTimeoutMs
        );
        // 超时后，未完成的任务仍会继续，但我们已经跳出等待。
        // 更好的做法是标记需要取消，但线程池未实现取消，暂且记录警告。
    }

    // 4. 更新统计
    inst.addStats(total, batches);

    // 5. 定期清理黑名单（每 cleanupIntervalTicks 次 tick 执行一次）
    //    这里简单用静态计数器，实际可放在 Level::tick 之外更合适
    static int tickCounter = 0;
    if (++tickCounter >= config.cleanupIntervalTicks) {
        tickCounter = 0;
        inst.cleanupCrashedList();
        if (config.debug) {
            inst.getSelf().getLogger().debug("Cleaned up crashed actor list");
        }
    }

    // 结束并行阶段
    inst.setParallelPhase(false);

    // 继续执行原始 Level tick（原函数会处理其他逻辑，如玩家 tick、方块 tick 等）
    origin();
}

void registerECSHooks() {
    ll::memory::HookRegistrar<LevelTickHook>().hook();
}

void unregisterECSHooks() {
    ll::memory::HookRegistrar<LevelTickHook>().unhook();
}

} // namespace parallel_tick
