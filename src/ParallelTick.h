#pragma once

#include <ll/api/mod/NativeMod.h>
#include <ll/api/coro/CoroTask.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <unordered_map>

// 前向声明
class Actor;
class ActorUniqueID;

namespace parallel_tick {

// 配置结构（已在 Config.h 定义）
struct Config;

// 统计结构
struct Stats {
    std::atomic<size_t> totalMobsParalleled{0};
    std::atomic<size_t> totalBatches{0};
    std::atomic<size_t> crashedActors{0};
};

// 线程池接口
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

// 主单例
class ParallelTick {
public:
    static ParallelTick& getInstance();

    // 插件生命周期（使用 ll::mod::NativeMod）
    bool load(ll::mod::NativeMod& self);
    bool unload();

    // 访问器
    ll::mod::NativeMod& getSelf();
    const Config&       getConfig() const;
    ThreadPool&         getPool();

    // 实体安全检查（基于黑名单）
    bool isActorSafeToTick(Actor* a) const;

    // 崩溃黑名单操作（基于 ActorUniqueID）
    void markCrashed(Actor* a);
    bool isCrashed(Actor* a) const;

    // 并行阶段标志（用于重入保护）
    void setParallelPhase(bool v);
    bool isParallelPhase() const;

    // 统计
    void   addStats(size_t mobs, size_t batches);
    Stats& getStats();
    void   printStats(); // 输出当前统计信息

    // 黑名单定期清理
    void cleanupCrashedList();

private:
    ParallelTick()  = default;
    ~ParallelTick() = default;

    void startStatsTask();  // 启动统计输出任务
    void stopStatsTask();   // 停止统计输出任务

    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

// Hook 注册/注销
void registerECSHooks();
void unregisterECSHooks();

} // namespace parallel_tick
