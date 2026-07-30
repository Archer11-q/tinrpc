#pragma once

namespace rpc {

// ============================================================
/// @brief EventHandler — 事件处理器抽象基类
///
/// 每种 fd 类型（监听 socket、客户端 socket、定时器等）
/// 继承此类并实现对应的回调方法。
///
/// EventLoop 通过多态调用 OnRead/OnWrite/OnClose，
/// 不需要知道具体 fd 是什么类型。
// ============================================================
class EventHandler {
public:
    EventHandler() = default;
    /// @brief 用已有 fd 构造
    /// @param fd 要管理的文件描述符
    explicit EventHandler(int fd) : fd_(fd) {
    }

    virtual ~EventHandler() = default;

    /// @brief 可读事件回调（新连接 / 新数据到达）
    virtual void OnRead() = 0;

    /// @brief 可写事件回调（发送缓冲区有空闲）
    /// @details v0.3 暂时不实现写入，子类留空
    virtual void OnWrite() {
    }

    /// @brief 错误或对端关闭回调
    virtual void OnClose() {
    }

    // 获取此 handler 管理的 fd
    int GetFd() const {
        return fd_;
    }

protected:
    int fd_ = -1; // 由子类构造时设置，EventLoop 通过 GetFd() 获取 fd 注册到 epoll
};

} // namespace rpc