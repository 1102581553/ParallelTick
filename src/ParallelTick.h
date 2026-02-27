#pragma once

#include <ll/api/mod/NativeMod.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <unordered_map>

// 前向声明
class Actor;
class ActorUniqueID;

namespace parallel_tick {

struct Config;
struct Stats;

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

    // 黑名单定期清理
    void cleanupCrashedList();

private:
    ParallelTick()  = default;
    ~ParallelTick() = default;

    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

// Hook 注册/注销
void registerECSHooks();
void unregisterECSHooks();

} // namespace parallel_tick
