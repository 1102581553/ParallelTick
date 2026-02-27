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

struct GridPos {
    int x, z;
    bool operator==(const GridPos& o) const { return x == o.x && z == o.z; }
};

struct GridPosHash {
    std::size_t operator()(const GridPos& p) const {
        return std::hash<int>()(p.x) * 2654435761u
             ^ std::hash<int>()(p.z) * 2246822519u;
    }
};

inline int gridColor(const GridPos& gp) {
    int cx = ((gp.x % 2) + 2) % 2;
    int cz = ((gp.z % 2) + 2) % 2;
    return cx * 2 + cz;
}

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

    const Config&    getConfig() const { return mConfig; }
    FixedThreadPool& getPool()         { return *mPool; }

    int  getAutoMaxEntities() const  { return mAutoMaxEntities.load(); }
    void setAutoMaxEntities(int v)   { mAutoMaxEntities.store(v); }

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
    }

    bool isActorSafeToTick(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        return mLiveActors.count(actor) > 0;
    }

    void clearAll() {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        mLiveActors.clear();
        mPendingQueue.clear();
    }

    // ── 并行阶段 ──
    std::mutex& getLevelMutex() { return mLevelMutex; }

    bool isParallelPhase() const {
        return mParallelPhase.load(std::memory_order_acquire);
    }
    void setParallelPhase(bool v) {
        mParallelPhase.store(v, std::memory_order_release);
    }

    // ── 崩溃追踪（增强版）──
    void recordCrash(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        mCrashCount[actor]++;
        if (mCrashCount[actor] >= mConfig.maxCrashCountPerActor) {
            mCrashedActors.insert(actor);
            mLiveActors.erase(actor);
        }
    }

    bool isPermanentlyCrashed(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        return mCrashedActors.count(actor) > 0;
    }

    // 收集需要在主线程 kill 的崩溃实体
    void scheduleKill(Actor* actor) {
        std::lock_guard<std::mutex> lk(mKillMutex);
        mPendingKills.push_back(actor);
    }

    std::vector<Actor*> takePendingKills() {
        std::lock_guard<std::mutex> lk(mKillMutex);
        return std::move(mPendingKills);
    }

    void clearCrashData() {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        // 只清除计数，永久标记保留到实体被移除
    }

    // 实体被移除时清除其崩溃记录
    void clearCrashRecord(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        mCrashedActors.erase(actor);
        mCrashCount.erase(actor);
    }

    // ── 统计 ──
    void addStats(size_t total, size_t phases) {
        mTotalStats.fetch_add(total, std::memory_order_relaxed);
        mPhaseStats.fetch_add(phases, std::memory_order_relaxed);
    }

    std::atomic<size_t>& getCrashStats() { return mCrashStats; }

private:
    void startStatsTask();
    void stopStatsTask();

    ll::mod::NativeMod&              mSelf;
    Config                           mConfig;
    std::unique_ptr<FixedThreadPool> mPool;

    // 收集保护
    std::mutex                  mCollectMutex;
    std::atomic<bool>           mCollecting{false};
    std::vector<ActorTickEntry> mPendingQueue;
    std::unordered_set<Actor*>  mLiveActors;

    // 并行阶段
    std::atomic<bool>           mParallelPhase{false};
    std::mutex                  mLevelMutex;

    // 崩溃追踪
    std::unordered_set<Actor*>           mCrashedActors;
    std::unordered_map<Actor*, int>      mCrashCount;

    // 延迟 kill 队列（主线程消费）
    std::mutex              mKillMutex;
    std::vector<Actor*>     mPendingKills;

    // 统计
    std::atomic<size_t> mTotalStats{0};
    std::atomic<size_t> mPhaseStats{0};
    std::atomic<size_t> mCrashStats{0};
    std::atomic<bool>   mStatsTaskRunning{false};
    std::atomic<int>    mAutoMaxEntities;
};

} // namespace parallel_tick
