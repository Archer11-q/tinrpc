#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

namespace rpc {

// ============================================================
// ThreadPool — 线程池，生产者-消费者模型
//
// 职责：
// - IO 线程（生产者）将 Frame 回调封装为 task，push 到任务队列
// - N 个 worker 线程（消费者）从队列取 task 并执行
// - 将业务逻辑从 IO 线程转移到工作线程，避免阻塞 epoll_wait
//
// 任务粒度：一个完整 Frame 的回调 = 一个 task
// ============================================================
class ThreadPool {
public:
    // 创建线程池，启动 num_workers 个 worker 线程
    // 默认数量 = std::thread::hardware_concurrency()（CPU 逻辑核心数）
    explicit ThreadPool(size_t num_workers = std::thread::hardware_concurrency());

    // 禁止拷贝和移动
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // 提交任务到队列（生产者调用，通常是 IO 线程）
    // 任务被移动到队列，在某个 worker 线程上执行
    // Shutdown 后调用将被忽略
    void Enqueue(std::function<void()> task);

    // 优雅关闭：
    // 1. 等待所有已提交任务执行完毕
    // 2. 通知所有 worker 退出
    // 3. join 所有 worker 线程
    // 调用后不可再 Enqueue
    void Shutdown();

    // 析构：未 Shutdown 则自动调用
    ~ThreadPool();

private:
    // 每个 worker 线程的主循环
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;     // Shutdown 标志：worker 看到后退出循环
};

} // namespace rpc