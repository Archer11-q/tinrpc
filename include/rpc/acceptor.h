#pragma once

#include "event_handler.h"
#include "socket.h"
#include "connection.h"

namespace rpc {

class EventLoop;

// ============================================================
/// @brief Acceptor — 监听新连接
///
/// 继承 EventHandler，管理监听 socket。
/// OnRead 被调用时表示有新连接等待 accept。
/// accept 后创建 Connection 对象并注册到 EventLoop。
// ============================================================
class Acceptor : public EventHandler {
public:
    /// @brief 构造并开始监听
    /// @param port 监听端口
    /// @param loop 所属 EventLoop
    /// @param cb 收到完整 Frame 后的回调（传递给每个 Connection）
    /// @param on_disconnect 连接断开时的回调（可选，v0.8 新增）
    Acceptor(uint16_t port, EventLoop* loop, FrameCallback cb = nullptr,
             DisconnectCallback on_disconnect = nullptr);

    /// @brief 有新连接到达时的回调
    void OnRead() override;

private:
    EventLoop* loop_; // 所属事件循环，accept 后需要注册新的 Connection 到 loop
    FrameCallback cb_; // 帧回调，传递给每个 Connection
    DisconnectCallback on_disconnect_; // 断连回调（v0.8 新增）
    Socket listen_sock_; // 监听 socket，封装了 socket fd 和相关操作
};

} // namespace rpc