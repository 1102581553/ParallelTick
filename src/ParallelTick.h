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
    BlockSource* region;
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

    void collectActor(Actor* actor, BlockSource& region) {
        std::lock_guard lock(mQueueMutex);
        mPendingQueue.push_back({actor, &region});
        mLiveActors.insert(actor);
    }

    // 实体被删除时，从存活集合移除
    void onActorRemoved(Actor* actor) {
        std::lock_guard lock(mQueueMutex);
        mLiveActors.erase(actor);
    }

    bool isActorAlive(Actor* actor) {
        std::lock_guard lock(mQueueMutex);
        return mLiveActors.count(actor) > 0;
    }

    std::vector<ActorTickEntry> takeQueue() {
        std::lock_guard lock(mQueueMutex);
        mLiveActors.clear();
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
    std::shared_mutex               mLifecycleMutex;
    FixedThreadPool                 mPool;

    std::atomic<bool>               mCollecting{false};
    std::mutex                      mQueueMutex;
    std::vector<ActorTickEntry>     mPendingQueue;
    std::unordered_set<Actor*>      mLiveActors;  // 收集期间存活的 actor

    std::atomic<size_t>             mPhaseStats[4] = {0, 0, 0, 0};
    std::atomic<size_t>             mUnsafeStats   = 0;
    std::atomic<bool>               mDebugTaskRunning{false};
};

} // namespace parallel_tick
