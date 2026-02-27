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
        // 使用更优的哈希组合，减少碰撞
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
      mAutoMaxEntities(256) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

    Config&               getConfig()         { return mConfig; }
    std::recursive_mutex& getLifecycleMutex() { return mLifecycleMutex; }
    FixedThreadPool&      getPool()           { return *mPool; }

    // 自动调整参数访问
    int  getAutoMaxEntities() const  { return mAutoMaxEntities.load(); }
    void setAutoMaxEntities(int val) { mAutoMaxEntities.store(val); }

    // 收集实体（主线程调用）
    void collectActor(Actor* actor, BlockSource& region) {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        mPendingQueue.push_back({actor, &region});
        mLiveActors.insert(actor);
    }

    // 延迟移除：打标记，不立即从 mLiveActors 删除
    void onActorRemoved(Actor* actor) {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        mPendingRemove.insert(actor);
    }

    // waitAll 后调用，统一清理延迟移除的实体
    void flushPendingRemove() {
        std::unique_lock<std::recursive_mutex> lock(mLifecycleMutex);
        for (auto* a : mPendingRemove) {
            mLiveActors.erase(a);
        }
        mPendingRemove.clear();
    }

    // 在主线程建立快照时使用，返回当前存活集合的拷贝
    bool isActorAlive(Actor* actor) {
        // 仅在持锁情况下调用（主线程快照阶段）
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

    bool isCollecting() const { return mCollecting.load(); }
    void setCollecting(bool v) { mCollecting.store(v); }

    void addStats(size_t total, size_t parallel, size_t serial) {
        mTotalStats    += total;
        mParallelStats += parallel;
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
    std::vector<ActorTickEntry>  mPendingQueue;
    std::unordered_set<Actor*>   mLiveActors;
    std::unordered_set<Actor*>   mPendingRemove;   // 延迟移除集合

    // 统计
    std::atomic<size_t> mTotalStats{0};
    std::atomic<size_t> mParallelStats{0};
    std::atomic<size_t> mSerialStats{0};
    std::atomic<bool>   mStatsTaskRunning{false};

    // 自动调整参数
    std::atomic<int> mAutoMaxEntities;
};

} // namespace parallel_tick
