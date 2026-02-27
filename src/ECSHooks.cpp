#include "ParallelTick.h"
#include <ll/api/memory/Hook.h>
#include <mc/entity/systems/GoalSelectorSystem.h>
#include <mc/entity/components/ActorOwnerComponent.h>
#include <mc/deps/ecs/gamerefs_entity/EntityRegistry.h>
#include <mc/world/actor/Actor.h>
#include <windows.h>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <exception>

// 压制 levilamina "not yet publicly available" 的 C4996 警告
// 这些符号实际存在于二进制中，只是文档标注未公开
#pragma warning(push)
#pragma warning(disable: 4996)

namespace parallel_tick {

// ── SEH 保护 ──────────────────────────────────────────────────────
static LONG goalSehFilter(unsigned int code) {
    return (code == 0xE06D7363u) ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER;
}
static int tickGoalWithSEH(ActorOwnerComponent& aoc) {
    __try {
        GoalSelectorSystem::_tickGoalSelectorComponent(aoc);
        return 0;
    } __except (goalSehFilter(GetExceptionCode())) {
        return (int)GetExceptionCode();
    }
}
static int tickGoalSafe(ActorOwnerComponent& aoc) {
    try { return tickGoalWithSEH(aoc); }
    catch (const std::exception& e) { fprintf(stderr, "[GoalSel] C++ %s\n", e.what()); return -1; }
    catch (...) { fprintf(stderr, "[GoalSel] Unk\n"); return -2; }
}

// ── Hook ──────────────────────────────────────────────────────────
LL_TYPE_INSTANCE_HOOK(
    HookGoalSelectorTick,
    ll::memory::HookPriority::Normal,
    GoalSelectorSystem,
    &GoalSelectorSystem::$tick,
    void,
    ::EntityRegistry& registry
) {
    auto& pt        = ParallelTick::getInstance();
    const auto& cfg = pt.getConfig();

    if (!cfg.enabled) { origin(registry); return; }

    // ── 1. 主线程收集 ─────────────────────────────────────────────
    // EntityRegistry::mRegistry 是 TypedStorageImpl<entt::basic_registry<EntityId>>
    // .get() 返回 entt::basic_registry<EntityId>&
    auto& enttReg = registry.mRegistry.get();

    struct Entry { ActorOwnerComponent* comp; Actor* actor; };
    std::vector<Entry> entries;
    entries.reserve(2048);

    enttReg.view<ActorOwnerComponent>().each([&](ActorOwnerComponent& aoc) {
        Actor* actor = aoc.mActor.get();
        if (!actor)                       return;
        if (actor->isPlayer())            return;
        if (actor->isSimulatedPlayer())   return;
        if (!pt.isActorSafeToTick(actor)) return;
        entries.push_back({&aoc, actor});
    });

    if (entries.empty()) return;

    // ── 2. 均匀分批 ───────────────────────────────────────────────
    auto& pool            = pt.getPool();
    const size_t total    = entries.size();
    const size_t nThreads = pool.threadCount();
    const size_t batchSize = std::clamp(
        (total + nThreads * 3 - 1) / (nThreads * 3),
        (size_t)1,
        (size_t)cfg.maxEntitiesPerTask
    );

    // ── 3. 提交并等待 ─────────────────────────────────────────────
    pt.setParallelPhase(true);

    const auto timeout = std::chrono::milliseconds(
        cfg.actorTickTimeoutMs > 0 ? cfg.actorTickTimeoutMs : 30000
    );

    size_t batchCount = 0;
    for (size_t i = 0; i < total; i += batchSize) {
        const size_t end = std::min(i + batchSize, total);
        pool.submit([begin = i, end, &entries, &pt]() {
            for (size_t j = begin; j < end; ++j) {
                auto& e = entries[j];
                if (!pt.isActorSafeToTick(e.actor)) continue;
                int ex = tickGoalSafe(*e.comp);
                if (ex != 0) {
                    pt.markCrashed(e.actor);
                    pt.getSelf().getLogger().error(
                        "[GoalSel][Crash] 0x{:08X} actor={:p}",
                        (unsigned)(ex & 0xFFFFFFFF), (void*)e.actor
                    );
                }
            }
        });
        ++batchCount;
    }

    if (!pool.waitAllFor(timeout)) {
        pt.getSelf().getLogger().error(
            "[GoalSel] TIMEOUT {}ms pend={}", timeout.count(), pool.pendingCount()
        );
        pool.waitAllFor(timeout * 2);
    }

    pt.setParallelPhase(false);

    if (cfg.debug) {
        pt.getSelf().getLogger().info(
            "[GoalSel] total={} batches={} batchSize={}",
            total, batchCount, batchSize
        );
    }
    pt.addStats(total, batchCount);
}

void registerECSHooks()   { HookGoalSelectorTick::hook();   }
void unregisterECSHooks() { HookGoalSelectorTick::unhook(); }

} // namespace parallel_tick

#pragma warning(pop)
