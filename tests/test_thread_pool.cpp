#include "rpc/thread_pool.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include <set>
#include <mutex>

// ============================================================
// 简易测试框架
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

void RunTest(const char* name, void (*fn)()) {
    printf("  %-48s ... ", name);
    try {
        fn();
        printf("[PASS]\n");
        g_passed++;
    } catch (const std::exception& e) {
        printf("[FAIL] exception: %s\n", e.what());
        g_failed++;
    } catch (...) {
        printf("[FAIL] unknown exception\n");
        g_failed++;
    }
}

// ============================================================
// 测试用例
// ============================================================

// 1. 基本入队执行：提交任务后，任务在 worker 线程中被执行
void TestBasicEnqueue() {
    rpc::ThreadPool pool(2);

    std::atomic<bool> executed{false};
    pool.Enqueue([&executed]() {
        executed = true;
    });

    // 等待任务执行（给 worker 一点时间）
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pool.Shutdown();

    assert(executed.load());
}

// 2. 多个任务在多个 worker 上并行执行
void TestMultipleWorkers() {
    const int kWorkers = 4;
    const int kTasks = kWorkers * 2;
    rpc::ThreadPool pool(kWorkers);

    std::atomic<int> counter{0};
    std::mutex thread_ids_mtx;
    std::set<std::thread::id> thread_ids;   // 记录执行任务的线程 ID

    for (int i = 0; i < kTasks; i++) {
        pool.Enqueue([&counter, &thread_ids, &thread_ids_mtx]() {
            counter++;
            {
                std::lock_guard<std::mutex> lock(thread_ids_mtx);
                thread_ids.insert(std::this_thread::get_id());
            }
            // 模拟一点工作
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        });
    }

    // 等待所有任务完成
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    pool.Shutdown();

    assert(counter.load() == kTasks);
    // 至少用到了 2 个不同的 worker 线程（不严格要求 4 个，因为有调度因素）
    assert(thread_ids.size() >= 2);
    printf("[workers used: %zu]", thread_ids.size());
}

// 3. Shutdown 等待所有已提交任务完成（不丢失任务）
void TestShutdownWaitsForPendingTasks() {
    const int kTasks = 10;
    rpc::ThreadPool pool(2);

    std::atomic<int> counter{0};

    for (int i = 0; i < kTasks; i++) {
        pool.Enqueue([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            counter++;
        });
    }

    // 立即 Shutdown，应该阻塞等待所有任务完成
    pool.Shutdown();

    // Shutdown 返回后，所有任务必须已执行完毕
    assert(counter.load() == kTasks);
}

// 4. 空队列 Shutdown 不卡死
void TestShutdownEmptyPool() {
    rpc::ThreadPool pool(2);
    // 不提交任何任务
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    pool.Shutdown();
    // 不卡死即通过
}

// 5. 任务正确捕获值（非引用悬挂）
void TestTaskCapturesValueCorrectly() {
    rpc::ThreadPool pool(1);

    std::atomic<int> result{0};

    {
        int local_value = 42;
        // 按值捕获 local_value，任务执行时 local_value 可能已销毁
        pool.Enqueue([&result, local_value]() {
            result = local_value;
        });
        // local_value 在这里销毁
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pool.Shutdown();

    assert(result.load() == 42);
}

// 6. Shutdown 后 Enqueue 被忽略，不崩溃
void TestEnqueueAfterShutdown() {
    rpc::ThreadPool pool(1);
    pool.Shutdown();

    std::atomic<bool> executed{false};
    pool.Enqueue([&executed]() {
        executed = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Shutdown 后入队的任务不应执行
    assert(!executed.load());
}

// ============================================================
// 入口
// ============================================================

int main() {
    printf("=== ThreadPool Tests ===\n\n");

    RunTest("TestBasicEnqueue",               TestBasicEnqueue);
    RunTest("TestMultipleWorkers",            TestMultipleWorkers);
    RunTest("TestShutdownWaitsForPendingTasks", TestShutdownWaitsForPendingTasks);
    RunTest("TestShutdownEmptyPool",          TestShutdownEmptyPool);
    RunTest("TestTaskCapturesValueCorrectly", TestTaskCapturesValueCorrectly);
    RunTest("TestEnqueueAfterShutdown",       TestEnqueueAfterShutdown);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}