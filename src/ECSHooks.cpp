#include "ParallelTick.h"
#include "Config.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/io/Logger.h>
#include <mc/world/level/Level.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/actor/Mob.h>
#include <vector>
#include <chrono>

namespace parallel_tick {

// Hook Level::tick - 虚函数需要使用 $ 前缀
LL_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    auto& inst = ParallelTick::getInstance();
    auto& config = inst.getConfig();

    if (!config.enabled || inst.isParallelPhase()) {
        origin();
        return;
    }

    inst.setParallelPhase(true);

    // 收集所有非玩家生物
    std::vector<Mob*> mobs;
    for (Actor* actor : this->getRuntimeActorList()) {
        if (actor && actor->hasCategory(ActorCategory::Mob) && !actor->isPlayer()) {
            if (inst.isActorSafeToTick(actor)) {
                mobs.push_back(static_cast<Mob*>(actor));
            }
        }
    }

    size_t total = mobs.size();
    size_t batchSize = config.maxEntitiesPerTask;
    size_t batches = (total + batchSize - 1) / batchSize;

    for (size_t i = 0; i < total; i += batchSize) {
        size_t end = std::min(i + batchSize, total);
        auto task = [&inst, mobs, i, end]() {
            for (size_t j = i; j < end; ++j) {
                Mob* mob = mobs[j];
                try {
                    mob->aiStep();
                } catch (...) {
                    inst.markCrashed(mob);
                    inst.getSelf().getLogger().error(
                        "Mob {} crashed during parallel tick",
                        mob->getOrCreateUniqueID().rawID
                    );
                }
            }
        };
        inst.getPool().submit(std::move(task));
    }

    auto timeout = std::chrono::milliseconds(config.actorTickTimeoutMs);
    if (!inst.getPool().waitAllFor(timeout)) {
        inst.getSelf().getLogger().warn(
            "Parallel tick timeout ({} ms), some tasks may be stuck",
            config.actorTickTimeoutMs
        );
    }

    inst.addStats(total, batches);

    // 定期清理黑名单
    static int tickCounter = 0;
    if (++tickCounter >= config.cleanupIntervalTicks) {
        tickCounter = 0;
        inst.cleanupCrashedList();
        if (config.debug) {
            inst.getSelf().getLogger().debug("Cleaned up crashed actor list");
        }
    }

    inst.setParallelPhase(false);
    origin();
}

void registerECSHooks() {
    ll::memory::HookRegistrar<LevelTickHook>().hook();
}

void unregisterECSHooks() {
    ll::memory::HookRegistrar<LevelTickHook>().unhook();
}

} // namespace parallel_tick
