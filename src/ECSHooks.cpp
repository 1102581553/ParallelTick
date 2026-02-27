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

// Hook Level::tick
LL_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::tick,
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
    std::vector<Actor*> entities;
    this->forEachEntity([&](Actor& actor) -> bool {
        if (actor.isMob() && !actor.isPlayer() && inst.isActorSafeToTick(&actor)) {
            entities.push_back(&actor);
        }
        return true;
    });

    size_t total = entities.size();
    size_t batchSize = config.maxEntitiesPerTask;
    size_t batches = (total + batchSize - 1) / batchSize;

    for (size_t i = 0; i < total; i += batchSize) {
        size_t end = std::min(i + batchSize, total);
        auto task = [&inst, entities, i, end]() {
            for (size_t j = i; j < end; ++j) {
                Actor* actor = entities[j];
                try {
                    actor->aiStep();
                } catch (...) {
                    inst.markCrashed(actor);
                    inst.getSelf().getLogger().error(
                        "Actor {} crashed during parallel tick",
                        actor->getOrCreateUniqueID().rawID
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
