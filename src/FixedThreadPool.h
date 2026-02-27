#pragma once
#include <windows.h>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <cstdio>

static int invokeWithSEH(std::function<void()>& f) {
    __try {
        f();
        return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

class FixedThreadPool {
public:
    explicit FixedThreadPool(size_t n, size_t stackSize = 8 * 1024 * 1024)
        : mStop(false), mPending(0)
    {
        for (size_t i = 0; i < n; ++i) {
            struct Param { FixedThreadPool* pool; };
            auto* param = new Param{this};
            HANDLE h = CreateThread(
                nullptr, stackSize,
                [](LPVOID p) -> DWORD {
                    auto* param = static_cast<Param*>(p);
                    param->pool->workerLoop();
                    delete param;
                    return 0;
                },
                param, 0, nullptr
            );
            if (h) mWorkers.push_back(h);
            else   delete param;
        }
    }

    ~FixedThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStop = true;
        }
        mCv.notify_all();
        for (auto h : mWorkers) {
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
        }
    }

    void submit(std::function<void()> f) {
        ++mPending;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mTasks.push(std::move(f));
        }
        mCv.notify_one();
    }

    void waitAll() {
        std::unique_lock<std::mutex> lock(mDoneMutex);
        mDoneCv.wait(lock, [this] { return mPending.load() == 0; });
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mCv.wait(lock, [this] { return mStop || !mTasks.empty(); });
                if (mStop && mTasks.empty()) return;
                task = std::move(mTasks.front());
                mTasks.pop();
            }
            int exCode = invokeWithSEH(task);
            if (exCode != 0) {
                fprintf(stderr,
                    "[FixedThreadPool] SEH 0x%08X in task\n",
                    static_cast<unsigned>(exCode));
            }
            if (--mPending == 0) {
                std::lock_guard<std::mutex> lock(mDoneMutex);
                mDoneCv.notify_all();
            }
        }
    }

    std::vector<HANDLE>               mWorkers;
    std::queue<std::function<void()>> mTasks;
    std::mutex                        mMutex;
    std::condition_variable           mCv;
    std::mutex                        mDoneMutex;
    std::condition_variable           mDoneCv;
    std::atomic<int>                  mPending;
    bool                              mStop;
};
