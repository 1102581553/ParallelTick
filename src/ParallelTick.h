#pragma once
#include <ll/api/mod/NativeMod.h>
#include <ll/api/io/Logger.h>
#include <mutex>
#include <atomic>
#include <vector>
#include <unordered_set>
#include <unordered_map>
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

    ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

    const Config&    getConfig() const { return mConfig; }
    FixedThreadPool& getPool()         { return *mPool; }

    // ── 收集阶段 ──
    bool isCollecting() const  { return mCollecting.load(std::memory_order_acquire); }
    void setCollecting(bool v) { mCollecting.store(v, std::memory_order_release); }

    void collectActor(Actor* actor, BlockSource& region) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        mPendingQueue.push_back({actor, &region});
        mLiveActors.insert(actor);
    }

    std::vector<ActorTickEntry> takeQueue() {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        return std::move(mPendingQueue);
    }

    // ── 实体生命周期 ──
    void onActorRemoved(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        mLiveActors.erase(actor);
        mCrashedActors.erase(actor);
    }

    bool isActorSafeToTick(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        if (mCrashedActors.count(actor) > 0) return false;
        return mLiveActors.count(actor) > 0;
    }

    void clearAll() {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        mLiveActors.clear();
        mPendingQueue.clear();
    }

    // ── 并行阶段 ──
    // 全局锁：所有 Level 变更操作在并行期间必须串行
    std::mutex& getLevelMutex() { return mLevelMutex; }

    bool isParallelPhase() const {
        return mParallelPhase.load(std::memory_order_acquire);
    }
    void setParallelPhase(bool v) {
        mParallelPhase.store(v, std::memory_order_release);
    }

    // ── 崩溃追踪 ──
    void markCrashed(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        mCrashedActors.insert(actor);
        mLiveActors.erase(actor);
        mCrashStats.fetch_add(1, std::memory_order_relaxed);
    }

    bool isPermanentlyCrashed(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        return mCrashedActors.count(actor) > 0;
    }

    // ── 统计 ──
    void addStats(size_t total, size_t dims) {
        mTotalStats.fetch_add(total, std::memory_order_relaxed);
        mDimStats.fetch_add(dims, std::memory_order_relaxed);
    }

    std::atomic<size_t>& getCrashStats() { return mCrashStats; }

private:
    void startStatsTask();
    void stopStatsTask();

    ll::mod::NativeMod&              mSelf;
    Config                           mConfig;
    std::unique_ptr<FixedThreadPool> mPool;

    std::mutex                  mCollectMutex;
    std::atomic<bool>           mCollecting{false};
    std::vector<ActorTickEntry> mPendingQueue;
    std::unordered_set<Actor*>  mLiveActors;

    std::atomic<bool>           mParallelPhase{false};
    std::mutex                  mLevelMutex;

    std::unordered_set<Actor*>  mCrashedActors;

    std::atomic<size_t> mTotalStats{0};
    std::atomic<size_t> mDimStats{0};
    std::atomic<size_t> mCrashStats{0};
    std::atomic<bool>   mStatsTaskRunning{false};
};

} // namespace parallel_tick
