#include "rpc/thread_pool.h"

#include <cstdio>

namespace rpc {

ThreadPool::ThreadPool(size_t num_workers) {
    // 启动 num_workers 个 worker 线程
    for (size_t i = 0; i < num_workers; i++) {
        workers_.emplace_back(&ThreadPool::WorkerLoop, this); // 成员函数作为线程入口，传递 this 指针
    }
    printf("[ThreadPool] Created with %zu worker threads\n", num_workers);
}

ThreadPool::~ThreadPool() {
    if (!stop_) {
        Shutdown();
    }
}

void ThreadPool::Enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_)
            return; // 已关闭，拒绝新任务
        tasks_.push(std::move(task));
    }
    cv_.notify_one(); // 唤醒一个等待的 worker
}

void ThreadPool::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_)
            return; // 已经关闭过了
        stop_ = true;
    }
    cv_.notify_all(); // 唤醒所有睡眠的 worker

    // join 所有 worker 线程
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    printf("[ThreadPool] Shutdown complete, all workers joined\n");
}

void ThreadPool::WorkerLoop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            // 等待条件：队列非空 或 收到关闭信号
            cv_.wait(lock, [this] { return !tasks_.empty() || stop_; });

            // 关闭且队列为空 → 退出
            if (stop_ && tasks_.empty()) {
                return;
            }

            // 取出队首任务
            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // 在锁外执行任务，避免长时间持锁阻塞其他 worker
        try {
            task(); // 执行任务
        } catch (const std::exception& e) {
            // 任务异常不影响 worker 继续运行
            printf("[ThreadPool] Task threw exception: %s\n", e.what());
        } catch (...) {
            printf("[ThreadPool] Task threw unknown exception\n");
        }
    }
}

} // namespace rpc