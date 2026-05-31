#pragma once

namespace rpc {

// ============================================================
// EventHandler — 事件处理器抽象基类
//
// 每种 fd 类型（监听 socket、客户端 socket、定时器等）
// 继承此类并实现对应的回调方法。
//
// EventLoop 通过多态调用 OnRead/OnWrite/OnClose，
// 不需要知道具体 fd 是什么类型。
// ============================================================
class EventHandler {
public:
    EventHandler() = default;
    explicit EventHandler(int fd) : fd_(fd) {}

    virtual ~EventHandler() = default;

    // 可读事件（新连接 / 新数据到达）
    virtual void OnRead() = 0;

    // 可写事件（发送缓冲区有空闲）
    // v0.3 暂时不实现写入，子类留空
    virtual void OnWrite() {}

    // 错误或对端关闭
    virtual void OnClose() {}

    // 获取此 handler 管理的 fd
    int GetFd() const { return fd_; }

protected:
    int fd_ = -1;   // 由子类构造时设置，EventLoop 通过 GetFd() 获取 fd 注册到 epoll
};

} // namespace rpc