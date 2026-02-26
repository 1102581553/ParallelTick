#pragma once
#include <ll/api/mod/NativeMod.h>
#include <ll/api/io/Logger.h>
#include <mutex>
#include <atomic>
#include "Config.h"

namespace parallel_tick {

class ParallelTick {
public:
    static ParallelTick& getInstance();

    ParallelTick() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

    Config& getConfig() { return mConfig; }
    std::mutex& getLifecycleMutex() { return mLifecycleMutex; }
    
    // 统计相关
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
    std::mutex mLifecycleMutex;
    
    // 调试统计变量
    std::atomic<size_t> mPhaseStats[4] = {0, 0, 0, 0};
    std::atomic<size_t> mUnsafeStats = 0;
    bool mDebugTaskRunning = false;
};

} // namespace parallel_tick
