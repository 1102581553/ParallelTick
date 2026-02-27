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

// 4-色棋盘：color = ((x%2+2)%2)*2 + ((z%2+2)%2)
// 同色网格间距 ≥ 2*gridSizeBase，gridSizeBase=64 时 ≥ 128格
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

    // ── 收集阶段控制 ──
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

    // 在 mCollectMutex 锁内调用消除 TOCTOU
    bool isActorSafeToTick(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        return mLiveActors.count(actor) > 0;
    }

    void clearAll() {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        mLiveActors.clear();
        mPendingQueue.clear();
    }

    // ── 并行阶段 Level 全局变更锁 ──
    // addEntity / removeEntity 在并行期间必须串行化
    std::mutex& getLevelMutex() { return mLevelMutex; }

    bool isParallelPhase() const {
        return mParallelPhase.load(std::memory_order_acquire);
    }
    void setParallelPhase(bool v) {
        mParallelPhase.store(v, std::memory_order_release);
    }

    // ── SEH 崩溃标记 ──
    void markCrashed(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        mCrashedActors.insert(actor);
        mLiveActors.erase(actor);
    }
    bool isCrashed(Actor* actor) {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        return mCrashedActors.count(actor) > 0;
    }
    void clearCrashed() {
        std::lock_guard<std::mutex> lk(mCollectMutex);
        mCrashedActors.clear();
    }

    // ── 统计 ──
    void addStats(size_t total, size_t phases) {
        mTotalStats.fetch_add(total, std::memory_order_relaxed);
        mPhaseStats.fetch_add(phases, std::memory_order_relaxed);
    }

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
    std::unordered_set<Actor*>  mCrashedActors;

    // 统计
    std::atomic<size_t> mTotalStats{0};
    std::atomic<size_t> mPhaseStats{0};
    std::atomic<bool>   mStatsTaskRunning{false};
    std::atomic<int>    mAutoMaxEntities;
};

} // namespace parallel_tick
