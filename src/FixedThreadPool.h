#pragma once
#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>

class FixedThreadPool {
public:
    explicit FixedThreadPool(size_t n) : mStop(false), mPending(0) {
        for (size_t i = 0; i < n; ++i) {
            mWorkers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock(mMutex);
                        mCv.wait(lock, [this] { return mStop || !mTasks.empty(); });
                        if (mStop && mTasks.empty()) return;
                        task = std::move(mTasks.front());
                        mTasks.pop();
                    }
                    task();
                    if (--mPending == 0) mDoneCv.notify_all();
                }
            });
        }
    }

    ~FixedThreadPool() {
        { std::lock_guard lock(mMutex); mStop = true; }
        mCv.notify_all();
        for (auto& t : mWorkers) t.join();
    }

    void submit(std::function<void()> f) {
        ++mPending;
        { std::lock_guard lock(mMutex); mTasks.push(std::move(f)); }
        mCv.notify_one();
    }

    void waitAll() {
        std::unique_lock lock(mDoneMutex);
        mDoneCv.wait(lock, [this] { return mPending == 0; });
    }

private:
    std::vector<std::thread>          mWorkers;
    std::queue<std::function<void()>> mTasks;
    std::mutex                        mMutex;
    std::condition_variable           mCv;
    std::mutex                        mDoneMutex;
    std::condition_variable           mDoneCv;
    std::atomic<int>                  mPending;
    bool                              mStop;
};
