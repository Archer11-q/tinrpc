#pragma once

#include "event_handler.h"
#include "socket.h"
#include "connection.h"

namespace rpc {

class EventLoop;

// ============================================================
// Acceptor — 监听新连接
//
// 继承 EventHandler，管理监听 socket。
// OnRead 被调用时表示有新连接等待 accept。
// accept 后创建 Connection 对象并注册到 EventLoop。
// ============================================================
class Acceptor : public EventHandler {
public:
    // port: 监听端口
    // loop: 所属 EventLoop
    // cb: 收到完整 Frame 后的回调（传递给每个 Connection）
    // on_disconnect: 连接断开时的回调（可选，v0.8 新增）
    Acceptor(uint16_t port, EventLoop* loop, FrameCallback cb = nullptr,
             DisconnectCallback on_disconnect = nullptr);

    // 有新连接到达
    void OnRead() override;

private:
    EventLoop* loop_;            // 所属事件循环，accept 后需要注册新的 Connection 到 loop
    FrameCallback cb_;           // 帧回调，传递给每个 Connection
    DisconnectCallback on_disconnect_; // 断连回调（v0.8 新增）
    Socket listen_sock_;         // 监听 socket，封装了 socket fd 和相关操作
};

} // namespace rpc