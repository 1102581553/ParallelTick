#include "ParallelTick.h"
#include <ll/api/memory/Hook.h>
#include <mc/entity/systems/GoalSelectorSystem.h>
#include <mc/entity/components/ActorOwnerComponent.h>
#include <mc/deps/ecs/EntityRegistry.h>
#include <mc/world/actor/Actor.h>
#include <windows.h>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <exception>

namespace parallel_tick {

// ── SEH 保护（两层：SEH 不能与 C++ 异常在同一函数体混用）──────────
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

// ── Hook GoalSelectorSystem::tick ────────────────────────────────
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

    // ── 1. 主线程收集所有需要并行的实体 ──────────────────────────
    struct Entry { ActorOwnerComponent* comp; Actor* actor; };
    std::vector<Entry> entries;
    entries.reserve(2048);

    registry.view<ActorOwnerComponent>().each([&](ActorOwnerComponent& aoc) {
        // TypedStorage<unique_ptr<Actor>>：根据实际访问方式二选一：
        //   Actor* actor = aoc.mActor.get();        // 若 TypedStorage 透明转发
        //   Actor* actor = aoc.mActor.get().get();  // 若需要两次 get()
        Actor* actor = aoc.mActor.get();
        if (!actor)                       return;
        if (actor->isPlayer())            return;
        if (actor->isSimulatedPlayer())   return;
        if (!pt.isActorSafeToTick(actor)) return;
        entries.push_back({&aoc, actor});
    });

    if (entries.empty()) return; // 无可并行实体，直接跳过（不调 origin，避免重复执行）

    // ── 2. 均匀分批：目标批数 = 线程数 × 3，保证负载均衡 ─────────
    auto& pool            = pt.getPool();
    const size_t total    = entries.size();
    const size_t nThreads = pool.threadCount();
    const size_t targetBatches = nThreads * 3;
    const size_t batchSize = std::clamp(
        (total + targetBatches - 1) / targetBatches,
        (size_t)1,
        (size_t)cfg.maxEntitiesPerTask
    );

    // ── 3. 全部提交，一次性等待 ───────────────────────────────────
    pt.setParallelPhase(true);

    const auto timeout = std::chrono::milliseconds(
        cfg.actorTickTimeoutMs > 0 ? cfg.actorTickTimeoutMs : 30000
    );

    size_t batchCount = 0;
    for (size_t i = 0; i < total; i += batchSize) {
        const size_t end = std::min(i + batchSize, total);
        // 传下标范围，避免拷贝 entries；entries 在 waitAllFor 返回前不会析构
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
