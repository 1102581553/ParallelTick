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

// 网格位置哈希
struct GridPos {
    int x, z;
    bool operator==(const GridPos& other) const { return x == other.x && z == other.z; }
};
struct GridPosHash {
    std::size_t operator()(const GridPos& p) const {
        return std::hash<int>()(p.x) ^ (std::hash<int>()(p.z) << 1);
    }
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

    void addStats(int total, int parallel, int serial) {
        mTotalStats += total;
        mParallelStats += parallel;
        mSerialStats += serial;
    }

private:
    void startStatsTask();
    void stopStatsTask();

    ll::mod::NativeMod&             mSelf;
    Config                          mConfig;
    std::recursive_mutex            mLifecycleMutex;
    std::unique_ptr<FixedThreadPool> mPool;

    std::atomic<bool>               mCollecting{false};
    std::vector<ActorTickEntry>     mPendingQueue;
    std::unordered_set<Actor*>      mLiveActors;

    // 统计
    std::atomic<size_t>             mTotalStats{0};
    std::atomic<size_t>             mParallelStats{0};
    std::atomic<size_t>             mSerialStats{0};
    std::atomic<bool>               mStatsTaskRunning{false};
};

} // namespace parallel_tick
