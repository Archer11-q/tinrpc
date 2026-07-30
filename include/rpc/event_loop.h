#pragma once

#include <unordered_map>
#include <memory>
#include <vector>
#include <cstdint>

namespace rpc {

class EventHandler;

// ============================================================
// EventLoop — Reactor 核心，epoll 事件循环
//
// 职责：
// - 管理 epoll 实例
// - 注册/注销 EventHandler（fd → handler 映射）
// - 主循环：epoll_wait → 查表找到 handler → 分发事件
//
// 单线程运行。一个 EventLoop 管理数百个连接。
// ============================================================
class EventLoop {
public:
    EventLoop(); // 构造时创建 epoll 实例
    ~EventLoop(); // 析构时关闭 epoll 实例

    // 禁止拷贝和移动（epoll fd 不可移动）
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // 启动事件循环（阻塞，直到 Stop() 被调用）
    void Run();

    // 停止事件循环（可在其他线程调用）
    void Stop();

    // 注册一个 handler 及其管理的 fd 到 epoll
    // events: epoll 事件掩码（EPOLLIN | EPOLLET 等）
    void Register(std::unique_ptr<EventHandler> handler, uint32_t events);

    // 从 epoll 移除一个 fd 并销毁其 handler
    void Unregister(int fd);

    // 修改已注册 fd 的 epoll 事件掩码（EPOLL_CTL_MOD）
    // 用于 Connection 在仅监听可读 和 同时监听可读可写 之间切换
    void UpdateEvents(int fd, uint32_t events);

private:
    int epfd_ = -1; // epoll 实例 fd
    int wakeup_fd_ = -1; // eventfd，用于 Stop() 唤醒 epoll_wait
    bool running_ = false; // 事件循环状态
    std::unordered_map<int, std::unique_ptr<EventHandler>> handlers_;
};

} // namespace rpc