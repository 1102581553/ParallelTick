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
        // 实体被移除时清除崩溃记录（指针即将失效）
        mCrashedActors.erase(actor);
        mCrashCount.erase(actor);
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
        // 注意：mCrashedActors 和 mCrashCount 不清除
        // 它们在 onActorRemoved 时按实体清除
    }

    // ── 并行阶段 ──
    std::mutex& getLevelMutex() { return mLevelMutex; }

    bool isParallelPhase() const {
        return mParallelPhase.load(std::memory_order_acquire);
    }
    void setParallelPhase(bool v) {
        mParallelPhase.store(v, std::memory_order_release);
    }

    // ── 崩溃追踪 ──
    // 记录一次崩溃，达到阈值后永久跳过
    // 返回 true = 已达到永久禁用阈值
    bool recordCrash(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        int count = ++mCrashCount[actor];
        mCrashStats.fetch_add(1, std::memory_order_relaxed);
        if (count >= mConfig.maxCrashCountPerActor) {
            mCrashedActors.insert(actor);
            mLiveActors.erase(actor);
            return true;
        }
        return false;
    }

    bool isPermanentlyCrashed(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        return mCrashedActors.count(actor) > 0;
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

    std::mutex                  mCollectMutex;
    std::atomic<bool>           mCollecting{false};
    std::vector<ActorTickEntry> mPendingQueue;
    std::unordered_set<Actor*>  mLiveActors;

    std::atomic<bool>           mParallelPhase{false};
    std::mutex                  mLevelMutex;

    // 崩溃追踪（持久化到实体被移除）
    std::unordered_set<Actor*>      mCrashedActors;
    std::unordered_map<Actor*, int> mCrashCount;

    std::atomic<size_t> mTotalStats{0};
    std::atomic<size_t> mPhaseStats{0};
    std::atomic<size_t> mCrashStats{0};
    std::atomic<bool>   mStatsTaskRunning{false};
    std::atomic<int>    mAutoMaxEntities;
};

} // namespace parallel_tick
