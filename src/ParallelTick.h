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
    bool operator==(const GridPos& other) const {
        return x == other.x && z == other.z;
    }
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

    void collectActor(Actor* actor, BlockSource& region) {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        mPendingQueue.push_back({actor, &region});
        mLiveActors.insert(actor);
    }

    void onActorRemoved(Actor* actor) {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        if (mTickingNow.load()) {
            mPendingRemove.insert(actor);
        } else {
            mLiveActors.erase(actor);
        }
    }

    void flushPendingRemove() {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        for (auto* a : mPendingRemove) mLiveActors.erase(a);
        mPendingRemove.clear();
    }

    bool isActorAlive(Actor* actor) {
        return mLiveActors.count(actor) > 0;
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

    // 标记实体已被并行 tick（工作线程调用，需加锁）
    void markTicked(Actor* actor) {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        mAlreadyTicked.insert(actor);
    }

    // 检查实体是否已被并行 tick（主线程二次 tick 拦截时调用）
    bool wasTicked(Actor* actor) {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        return mAlreadyTicked.count(actor) > 0;
    }

    // waitAll 后清理（主线程调用）
    void clearTicked() {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        mAlreadyTicked.clear();
    }

    bool isCollecting() const  { return mCollecting.load(); }
    void setCollecting(bool v) { mCollecting.store(v); }

    bool isTickingNow() const  { return mTickingNow.load(); }
    void setTickingNow(bool v) { mTickingNow.store(v); }

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

    std::atomic<bool>           mCollecting{false};
    std::atomic<bool>           mTickingNow{false};

    std::vector<ActorTickEntry> mPendingQueue;
    std::unordered_set<Actor*>  mLiveActors;
    std::unordered_set<Actor*>  mPendingRemove;
    std::unordered_set<Actor*>  mAlreadyTicked;  // ← 新增

    std::atomic<size_t> mTotalStats{0};
    std::atomic<size_t> mParallelStats{0};
    std::atomic<size_t> mSerialStats{0};
    std::atomic<bool>   mStatsTaskRunning{false};

    std::atomic<int> mAutoMaxEntities;
};

} // namespace parallel_tick
