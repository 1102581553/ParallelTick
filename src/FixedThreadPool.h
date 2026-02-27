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
                }
            });
        }
    }

    ~FixedThreadPool() {
        { std::lock_guard lock(mMutex); mStop = true; }
        mCv.notify_all();
        for (auto& t : mWorkers) t.join();
    }

    // 修改：确保异常安全，任务无论是否抛出异常都递减 mPending 并通知
    void submit(std::function<void()> f) {
        ++mPending;
        {
            std::lock_guard lock(mMutex);
            mTasks.push([this, f = std::move(f)] {
                // RAII 风格保证递减
                auto decrement = [this] {
                    if (--mPending == 0) {
                        std::lock_guard lock(mDoneMutex);
                        mDoneCv.notify_all();
                    }
                };
                try {
                    f();
                    decrement();
                } catch (...) {
                    decrement();
                    throw;  // 可根据需要改为只记录日志而不重新抛出
                }
            });
        }
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
