#pragma once
#include <ll/api/mod/NativeMod.h>
#include <ll/api/io/Logger.h>
#include <shared_mutex>
#include <atomic>
#include <vector>
#include "Config.h"

class Actor;

namespace parallel_tick {

struct ParallelGroups {
    std::vector<Actor*> phase[4];
    std::vector<Actor*> unsafe;
};

class ParallelTick {
public:
    static ParallelTick& getInstance();

    ParallelTick() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

    Config& getConfig() { return mConfig; }
    std::shared_mutex& getLifecycleMutex() { return mLifecycleMutex; }

    void addStats(int p0, int p1, int p2, int p3, int u) {
        mPhaseStats[0] += p0; mPhaseStats[1] += p1;
        mPhaseStats[2] += p2; mPhaseStats[3] += p3;
        mUnsafeStats += u;
    }

private:
    void startDebugTask();
    void stopDebugTask();

    ll::mod::NativeMod& mSelf;
    Config mConfig;
    std::shared_mutex mLifecycleMutex;

    std::atomic<size_t> mPhaseStats[4] = {0, 0, 0, 0};
    std::atomic<size_t> mUnsafeStats = 0;
    std::atomic<bool> mDebugTaskRunning = false;
};

} // namespace parallel_tick
