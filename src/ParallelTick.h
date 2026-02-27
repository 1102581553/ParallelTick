#pragma once
#include <ll/api/mod/NativeMod.h>
#include <ll/api/io/Logger.h>
#include <mutex>          // std::recursive_mutex
#include <atomic>
#include <vector>
#include <unordered_set>
#include <memory>
#include "Config.h"
#include "FixedThreadPool.h"

class Actor;
class BlockSource;

namespace parallel_tick {

struct ActorTickEntry {
    Actor*       actor;
    BlockSource* region;
};

void registerHooks();
void unregisterHooks();

class ParallelTick {
public:
    static ParallelTick& getInstance();

    ParallelTick()
    : mSelf(*ll::mod::NativeMod::current()),
      mPool(nullptr) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

    Config&                 getConfig()         { return mConfig; }
    std::recursive_mutex&   getLifecycleMutex() { return mLifecycleMutex; }
    FixedThreadPool&        getPool()           { return *mPool; }

    // 收集实体：写锁
    void collectActor(Actor* actor, BlockSource& region) {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        mPendingQueue.push_back({actor, &region});
        mLiveActors.insert(actor);
    }

    void onActorRemoved(Actor* actor) {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        mLiveActors.erase(actor);
    }

    // 检查存活：独占锁
    bool isActorAlive(Actor* actor) {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        return mLiveActors.count(actor) > 0;
    }

    void clearLive() {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        mLiveActors.clear();
    }

    std::vector<ActorTickEntry> takeQueue() {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
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
    void startStatsTask();
    void stopStatsTask();

    ll::mod::NativeMod&             mSelf;
    Config                          mConfig;
    std::recursive_mutex            mLifecycleMutex;   // 递归互斥量
    std::unique_ptr<FixedThreadPool> mPool;

    std::atomic<bool>               mCollecting{false};
    std::vector<ActorTickEntry>     mPendingQueue;
    std::unordered_set<Actor*>      mLiveActors;

    std::atomic<size_t>             mPhaseStats[4] = {0, 0, 0, 0};
    std::atomic<size_t>             mUnsafeStats   = 0;
    std::atomic<bool>               mStatsTaskRunning{false};
};

} // namespace parallel_tick
