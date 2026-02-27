#pragma once
#include <windows.h>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <cstdio>

// SEH 包装：必须是独立的非内联函数，内部不能有任何 C++ 析构对象
// 返回 0 表示正常，非 0 为 SEH 异常码
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
                nullptr,
                stackSize,
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

            // SEH 捕获硬件异常（0xC0000005 等），工作线程不崩溃
            int exCode = invokeWithSEH(task);
            if (exCode != 0) {
                fprintf(stderr,
                    "[FixedThreadPool] SEH exception 0x%08X in task\n",
                    static_cast<unsigned>(exCode));
            }

            // 无论成功或异常都递减，防止 waitAll 永久阻塞
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
