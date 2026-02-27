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

// ================================================================
// SEH 过滤器：关键修复
//
// 0xE06D7363 = MSVC C++ 异常的 SEH 代码
// 必须让它穿透 __except，由外层 C++ try-catch 正确处理
// 否则栈展开被跳过 → 析构函数不跑 → 状态损坏
// ================================================================
static LONG sehFilterNoCpp(unsigned int code) {
    if (code == 0xE06D7363u)          // C++ 异常 → 不拦截
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_EXECUTE_HANDLER;  // 硬件异常 → 拦截
}

// SEH 安全调用（只捕获硬件异常，不捕获 C++ 异常）
static int invokeWithSEH(std::function<void()>& f) {
    __try {
        f();
        return 0;
    } __except (sehFilterNoCpp(GetExceptionCode())) {
        return static_cast<int>(GetExceptionCode());
    }
}

// 完整安全调用（SEH + C++ 异常）
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
            // 使用完整安全调用：C++ 异常走 try-catch 正常展开
            int exCode = invokeFullySafe(task);
            if (exCode != 0) {
                fprintf(stderr,
                    "[FixedThreadPool] Task failed with code 0x%08X\n",
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
