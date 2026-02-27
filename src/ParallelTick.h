#pragma once
#include <ll/api/mod/NativeMod.h>
#include <ll/api/io/Logger.h>
#include <mutex>
#include <atomic>
#include <vector>
#include <unordered_set>
#include <memory>
#include <unordered_map>
#include "Config.h"
#include "FixedThreadPool.h"

class Actor;
class BlockSource;

namespace parallel_tick {

struct ActorTickEntry {
    Actor*       actor;
    BlockSource* region;
};

struct GridPos {
    int x, z;
    bool operator==(const GridPos& o) const { return x==o.x && z==o.z; }
};

struct GridPosHash {
    std::size_t operator()(const GridPos& p) const {
        return std::hash<int>()(p.x)*2654435761u
             ^ std::hash<int>()(p.z)*2246822519u;
    }
};

void registerHooks();
void unregisterHooks();

class ParallelTick {
public:
    static ParallelTick& getInstance();

    ParallelTick()
        : mSelf(*ll::mod::NativeMod::current()),
          mPool(nullptr),
          mAutoMaxEntities(256) {}

    ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

    Config&               getConfig()         { return mConfig; }
    std::recursive_mutex& getLifecycleMutex() { return mLifecycleMutex; }
    FixedThreadPool&      getPool()           { return *mPool; }

    int  getAutoMaxEntities() const  { return mAutoMaxEntities.load(); }
    void setAutoMaxEntities(int v)   { mAutoMaxEntities.store(v); }

    // 收集阶段：Actor::tick Hook 调用
    void collectActor(Actor* actor, BlockSource& region) {
        std::unique_lock<std::recursive_mutex> lk(mLifecycleMutex);
        mPendingQueue.push_back({actor, &region});
        mLiveActors.insert(actor);
    }

    // removeEntity Hook 调用
    void onActorRemoved(Actor* actor) {
        std::unique_lock<std::recursive_mutex> lk(mLifecycleMutex);
        mLiveActors.erase(actor);
        mPendingRemove.insert(actor);
    }

    bool isActorAlive(Actor* actor) {
        // 调用前必须已持锁
        return mLiveActors.count(actor) > 0;
    }

    void clearAll() {
        std::unique_lock<std::recursive_mutex> lk(mLifecycleMutex);
        mLiveActors.clear();
        mPendingRemove.clear();
        mPendingQueue.clear();
    }

    std::vector<ActorTickEntry> takeQueue() {
        std::unique_lock<std::recursive_mutex> lk(mLifecycleMutex);
        return std::move(mPendingQueue);
    }

    bool isCollecting() const  { return mCollecting.load(); }
    void setCollecting(bool v) { mCollecting.store(v); }

    void addStats(size_t total, size_t tasks) {
        mTotalStats    += total;
        mParallelStats += tasks;
    }

private:
    void startStatsTask();
    void stopStatsTask();

    ll::mod::NativeMod&              mSelf;
    Config                           mConfig;
    std::recursive_mutex             mLifecycleMutex;
    std::unique_ptr<FixedThreadPool> mPool;

    std::atomic<bool>           mCollecting{false};
    std::vector<ActorTickEntry> mPendingQueue;
    std::unordered_set<Actor*>  mLiveActors;
    std::unordered_set<Actor*>  mPendingRemove;

    std::atomic<size_t> mTotalStats{0};
    std::atomic<size_t> mParallelStats{0};
    std::atomic<bool>   mStatsTaskRunning{false};
    std::atomic<int>    mAutoMaxEntities;
};

} // namespace parallel_tick
