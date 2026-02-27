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
    bool operator==(const GridPos& other) const { return x == other.x && z == other.z; }
};

struct GridPosHash {
    std::size_t operator()(const GridPos& p) const {
        size_t hx = std::hash<int>()(p.x) * 2654435761u;
        size_t hz = std::hash<int>()(p.z) * 2246822519u;
        return hx ^ (hz << 1);
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
      mAutoMaxEntities(256),
      mTickingNow(false) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

    Config&               getConfig()         { return mConfig; }
    std::recursive_mutex& getLifecycleMutex() { return mLifecycleMutex; }
    FixedThreadPool&      getPool()           { return *mPool; }

    int  getAutoMaxEntities() const  { return mAutoMaxEntities.load(); }
    void setAutoMaxEntities(int val) { mAutoMaxEntities.store(val); }

    // 收集实体（主线程调用）
    void collectActor(Actor* actor, BlockSource& region) {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        mPendingQueue.push_back({actor, &region});
        mLiveActors.insert(actor);
    }

    // removeEntity Hook 调用
    // 若当前正在并行 tick，则挂起移除请求，等 waitAll 后主线程统一处理
    // 若没有在并行 tick，直接标记移除即可
    void onActorRemoved(Actor* actor) {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        if (mTickingNow.load()) {
            // 并行 tick 期间：挂起，不动 mLiveActors
            mPendingRemove.insert(actor);
        } else {
            // 非 tick 期间：直接移除
            mLiveActors.erase(actor);
        }
    }

    // waitAll 后在主线程调用，清理挂起的移除
    void flushPendingRemove() {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        for (auto* a : mPendingRemove) {
            mLiveActors.erase(a);
        }
        mPendingRemove.clear();
    }

    // 仅在主线程持锁时调用
    bool isActorAlive(Actor* actor) {
        return mLiveActors.count(actor) > 0;
    }

    // 从 mLiveActors 移除指定实体（快照过滤后使用）
    void removeFromLive(Actor* actor) {
        mLiveActors.erase(actor);
    }

    void clearLive() {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        mLiveActors.clear();
        mPendingRemove.clear();
    }

    std::vector<ActorTickEntry> takeQueue() {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        return std::move(mPendingQueue);
    }

    bool isCollecting() const { return mCollecting.load(); }
    void setCollecting(bool v) { mCollecting.store(v); }

    // 标记是否正在并行 tick
    void setTickingNow(bool v) { mTickingNow.store(v); }
    bool isTickingNow() const  { return mTickingNow.load(); }

    void addStats(size_t total, size_t tasks, size_t serial) {
        mTotalStats    += total;
        mParallelStats += tasks;
        mSerialStats   += serial;
    }

private:
    void startStatsTask();
    void stopStatsTask();

    ll::mod::NativeMod&              mSelf;
    Config                           mConfig;
    std::recursive_mutex             mLifecycleMutex;
    std::unique_ptr<FixedThreadPool> mPool;

    std::atomic<bool>            mCollecting{false};
    std::atomic<bool>            mTickingNow{false};

    std::vector<ActorTickEntry>  mPendingQueue;
    std::unordered_set<Actor*>   mLiveActors;
    std::unordered_set<Actor*>   mPendingRemove;

    std::atomic<size_t> mTotalStats{0};
    std::atomic<size_t> mParallelStats{0};
    std::atomic<size_t> mSerialStats{0};
    std::atomic<bool>   mStatsTaskRunning{false};

    std::atomic<int> mAutoMaxEntities;
};

} // namespace parallel_tick
