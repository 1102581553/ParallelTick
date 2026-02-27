#pragma once
#include <ll/api/mod/NativeMod.h>
#include <ll/api/io/Logger.h>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <array>
#include "Config.h"
#include "FixedThreadPool.h"

class Actor;
class BlockSource;

namespace parallel_tick {

struct ActorTickEntry { Actor* actor; BlockSource* region; };

struct GridPos {
    int x, z;
    bool operator==(const GridPos& o) const { return x == o.x && z == o.z; }
};
struct GridPosHash {
    size_t operator()(const GridPos& p) const {
        return std::hash<int>()(p.x) * 2654435761u ^ std::hash<int>()(p.z) * 2246822519u;
    }
};
inline int gridColor(const GridPos& gp) {
    return (((gp.x % 2) + 2) % 2) * 2 + (((gp.z % 2) + 2) % 2);
}

void registerHooks();
void unregisterHooks();
void registerECSHooks();
void unregisterECSHooks();

class GlobalLocks {
public:
    std::mutex entityLifecycleLock;
    static GlobalLocks& get() { static GlobalLocks s; return s; }
};

class ParallelTick {
public:
    static ParallelTick& getInstance();
    ParallelTick() : mSelf(*ll::mod::NativeMod::current()), mPool(nullptr) {}
    ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

    const Config&    getConfig() const { return mConfig; }
    FixedThreadPool& getPool()         { return *mPool; }

    bool isCollecting() const  { return mCollecting.load(std::memory_order_acquire); }
    void setCollecting(bool v) { mCollecting.store(v, std::memory_order_release); }

    void collectActor(Actor* a, BlockSource& r) {
        std::lock_guard<std::mutex> lk(mMtx);
        mQueue.push_back({a, &r}); mLive.insert(a);
    }
    std::vector<ActorTickEntry> takeQueue() {
        std::lock_guard<std::mutex> lk(mMtx);
        return std::move(mQueue);
    }
    void onActorRemoved(Actor* a) {
        std::lock_guard<std::mutex> lk(mMtx);
        mLive.erase(a); mCrashed.erase(a);
    }
    bool isActorSafeToTick(Actor* a) {
        std::lock_guard<std::mutex> lk(mMtx);
        return mLive.count(a) > 0 && mCrashed.count(a) == 0;
    }
    void clearAll() {
        std::lock_guard<std::mutex> lk(mMtx);
        mLive.clear(); mQueue.clear();
    }
    void markCrashed(Actor* a) {
        std::lock_guard<std::mutex> lk(mMtx);
        mCrashed.insert(a); mLive.erase(a);
        mCrashCount.fetch_add(1, std::memory_order_relaxed);
    }
    bool isPermanentlyCrashed(Actor* a) {
        std::lock_guard<std::mutex> lk(mMtx);
        return mCrashed.count(a) > 0;
    }

    bool isParallelPhase() const  { return mParallel.load(std::memory_order_acquire); }
    void setParallelPhase(bool v) { mParallel.store(v, std::memory_order_release); }

    void addStats(size_t total, size_t tasks) {
        mStatTotal.fetch_add(total); mStatTasks.fetch_add(tasks);
    }
    std::atomic<size_t>& crashCount() { return mCrashCount; }

private:
    void startStatsTask();
    void stopStatsTask();

    ll::mod::NativeMod& mSelf;
    Config              mConfig;
    std::unique_ptr<FixedThreadPool> mPool;

    std::mutex                    mMtx;
    std::atomic<bool>             mCollecting{false};
    std::vector<ActorTickEntry>   mQueue;
    std::unordered_set<Actor*>    mLive;
    std::unordered_set<Actor*>    mCrashed;

    std::atomic<bool>             mParallel{false};
    std::atomic<size_t>           mStatTotal{0}, mStatTasks{0}, mCrashCount{0};
    std::atomic<bool>             mStatsRunning{false};
};

} // namespace parallel_tick
