#pragma once
#include <windows.h>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <cstdio>
#include <exception>

static LONG sehFilterNoCpp(unsigned int code) {
    if (code == 0xE06D7363u)
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_EXECUTE_HANDLER;
}

static int invokeWithSEH(std::function<void()>& f) {
    __try {
        f();
        return 0;
    } __except (sehFilterNoCpp(GetExceptionCode())) {
        return static_cast<int>(GetExceptionCode());
    }
}

static int invokeFullySafe(std::function<void()>& f) {
    try {
        return invokeWithSEH(f);
    } catch (const std::exception& e) {
        fprintf(stderr, "[FixedThreadPool] C++ exception: %s\n", e.what());
        return -1;
    } catch (...) {
        fprintf(stderr, "[FixedThreadPool] Unknown C++ exception\n");
        return -2;
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
                    auto* pp = static_cast<Param*>(p);
                    pp->pool->workerLoop();
                    delete pp;
                    return 0;
                },
                param, 0, nullptr
            );
            if (h) mWorkers.push_back(h);
            else   delete param;
        }
    }

    ~FixedThreadPool() {
        mStop.store(true, std::memory_order_release);
        mCv.notify_all();
        for (auto h : mWorkers) {
            WaitForSingleObject(h, INFINITE);
            CloseHandle(h);
        }
    }

    void submit(std::function<void()> f) {
        mPending.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mTasks.push(std::move(f));
        }
        mCv.notify_one();
    }

    void waitAll() {
        std::unique_lock<std::mutex> lock(mDoneMutex);
        mDoneCv.wait(lock, [this] {
            return mPending.load(std::memory_order_acquire) == 0;
        });
    }

    bool waitAllFor(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mDoneMutex);
        return mDoneCv.wait_for(lock, timeout, [this] {
            return mPending.load(std::memory_order_acquire) == 0;
        });
    }

    int pendingCount() const {
        return mPending.load(std::memory_order_acquire);
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mCv.wait(lock, [this] {
                    return mStop.load(std::memory_order_acquire)
                        || !mTasks.empty();
                });
                if (mStop.load(std::memory_order_acquire) && mTasks.empty())
                    return;
                task = std::move(mTasks.front());
                mTasks.pop();
            }
            int exCode = invokeFullySafe(task);
            if (exCode != 0) {
                fprintf(stderr,
                    "[FixedThreadPool] Task failed code 0x%08X\n",
                    static_cast<unsigned>(exCode));
            }
            if (mPending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
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
    std::atomic<bool>                 mStop;
};
