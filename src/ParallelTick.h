#pragma once
#include <ll/api/mod/NativeMod.h>
#include <ll/api/io/Logger.h>
#include <shared_mutex>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_set>
#include "Config.h"
#include "FixedThreadPool.h"

class Actor;
class BlockSource;

namespace parallel_tick {

struct ActorTickEntry {
    Actor*       actor;
    BlockSource* region;  // 仅用于收集阶段，并行任务中不再使用
};

void registerHooks();
void unregisterHooks();

class ParallelTick {
public:
    static ParallelTick& getInstance();

    ParallelTick()
    : mSelf(*ll::mod::NativeMod::current()),
      mPool(std::max(1u, std::thread::hardware_concurrency() - 1)) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

    Config&            getConfig()         { return mConfig; }
    std::shared_mutex& getLifecycleMutex() { return mLifecycleMutex; }
    FixedThreadPool&   getPool()           { return mPool; }

    // 收集实体：写锁保护下将实体加入队列和存活集合
    void collectActor(Actor* actor, BlockSource& region) {
        std::unique_lock lock(mLifecycleMutex);
        mPendingQueue.push_back({actor, &region});
        mLiveActors.insert(actor);
    }

    // 实体移除时从存活集合中删除（写锁保护）
    void onActorRemoved(Actor* actor) {
        std::unique_lock lock(mLifecycleMutex);
        mLiveActors.erase(actor);
    }

    // 不加锁版本，供已持有写锁的调用者使用
    void unsafeOnActorRemoved(Actor* actor) {
        mLiveActors.erase(actor);
    }

    // 检查实体是否存活（读锁保护）
    bool isActorAlive(Actor* actor) {
        std::shared_lock lock(mLifecycleMutex);
        return mLiveActors.count(actor) > 0;
    }

    // 清空存活集合（写锁保护）
    void clearLive() {
        std::unique_lock lock(mLifecycleMutex);
        mLiveActors.clear();
    }

    // 取出队列并清空（写锁保护）
    std::vector<ActorTickEntry> takeQueue() {
        std::unique_lock lock(mLifecycleMutex);
        return std::move(mPendingQueue);
    }

    bool isCollecting() const { return mCollecting.load(); }
    void setCollecting(bool v) { mCollecting.store(v); }

    void addStats(int p0, int p1, int p2, int p3, int u) {
        mPhaseStats[0] += p0; mPhaseStats[1] += p1;
        mPhaseStats[2] += p2; mPhaseStats[3] += p3;
        mUnsafeStats   += u;
    }

private:
    void startDebugTask();
    void stopDebugTask();

    ll::mod::NativeMod&             mSelf;
    Config                          mConfig;
    // 读写锁，保护 mPendingQueue 和 mLiveActors
    std::shared_mutex               mLifecycleMutex;
    FixedThreadPool                 mPool;

    std::atomic<bool>               mCollecting{false};
    // 以下容器由 mLifecycleMutex 保护
    std::vector<ActorTickEntry>     mPendingQueue;
    std::unordered_set<Actor*>      mLiveActors;

    std::atomic<size_t>             mPhaseStats[4] = {0, 0, 0, 0};
    std::atomic<size_t>             mUnsafeStats   = 0;
    std::atomic<bool>               mDebugTaskRunning{false};
};

} // namespace parallel_tick
