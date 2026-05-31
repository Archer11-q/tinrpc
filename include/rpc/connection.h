#pragma once

#include "event_handler.h"
#include "buffer.h"
#include "protocol.h"

#include <functional>

namespace rpc {

class EventLoop;

// ============================================================
// FrameCallback — 收到完整帧时的回调
// v0.3 用回调传递给外部做验证；后续版本替换为 Dispatch
// ============================================================
using FrameCallback = std::function<void(const Frame&)>;

// ============================================================
// Connection — 客户端连接处理器
//
// 继承 EventHandler，管理一个客户端 socket。
// OnRead：从 socket 读取数据 → Buffer 累积 → 切帧 → 解码 → 回调
// OnClose：对端断开或发生错误，从 EventLoop 移除自身
// ============================================================
class Connection : public EventHandler {
public:
    // fd: 客户端 socket fd（已设为非阻塞）
    // loop: 所属的 EventLoop（OnClose 时需要从 loop 移除自己）
    // cb: 收到完整 Frame 后的回调（v0.3 用于测试验证，后续改为 Dispatch）
    Connection(int fd, EventLoop* loop, FrameCallback cb = nullptr);

    void OnRead() override;
    void OnClose() override;

private:
    EventLoop* loop_;               // 所属事件循环，OnClose 时需要调用 loop_->Unregister(fd_)
    Buffer read_buffer_;            // 读缓冲区，处理粘包/拆包
    FrameCallback frame_callback_;  // 收到完整帧后的回调，v0.3 用于测试验证，后续改为 Dispatch
};

} // namespace rpc