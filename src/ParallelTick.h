// ParallelTick.h
// 并行 Tick 插件核心单例接口
// ─────────────────────────────────────────────────────────────────
#pragma once

#include <ll/api/plugin/NativePlugin.h>
#include <atomic>
#include <chrono>
#include <cstddef>

// 前向声明
class Actor;
class Mob;

namespace parallel_tick {

// ── 配置 ─────────────────────────────────────────────────────────
struct Config {
    bool   enabled              = true;
    bool   debug                = false;
    int    threadCount          = 0;          // 0 = hardware_concurrency - 1
    size_t maxEntitiesPerTask   = 64;         // 单个线程池任务最多处理的实体数
    int    actorTickTimeoutMs   = 5000;       // aiStep 并行阶段超时（ms）
};

// ── 统计 ─────────────────────────────────────────────────────────
struct Stats {
    std::atomic<size_t> totalMobsParalleled{0};
    std::atomic<size_t> totalBatches{0};
    std::atomic<size_t> crashedActors{0};
};

// ── ThreadPool 接口（实现见 ThreadPool.h / .cpp）─────────────────
class ThreadPool {
public:
    explicit ThreadPool(size_t threads);
    ~ThreadPool();

    void   submit(std::function<void()> task);
    void   waitAll();
    bool   waitAllFor(std::chrono::milliseconds timeout);
    size_t threadCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

// ── 主单例 ────────────────────────────────────────────────────────
class ParallelTick {
public:
    static ParallelTick& getInstance();

    // 插件生命周期
    bool load(ll::plugin::NativePlugin& self);
    bool unload();

    // 访问器
    ll::plugin::NativePlugin& getSelf();
    const Config&             getConfig()  const;
    ThreadPool&               getPool();

    // 线程安全检查
    bool isActorSafeToTick(const Actor* a) const;

    // 崩溃黑名单
    void markCrashed(const Actor* a);
    bool isCrashed(const Actor* a) const;

    // 并行阶段标志（用于重入保护）
    void setParallelPhase(bool v);
    bool isParallelPhase() const;

    // 统计
    void   addStats(size_t mobs, size_t batches);
    Stats& getStats();

private:
    ParallelTick()  = default;
    ~ParallelTick() = default;

    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

// ── Hook 注册 / 注销（在 ECSHooks.cpp 里实现）────────────────────
void registerECSHooks();
void unregisterECSHooks();

} // namespace parallel_tick
