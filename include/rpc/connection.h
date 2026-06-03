#pragma once

#include "event_handler.h"
#include "buffer.h"
#include "protocol.h"

#include <functional>

namespace rpc {

class EventLoop;

// ============================================================
// FrameCallback — 收到完整帧时的回调
// v0.5 增加 Connection* 参数，回调可通过它发送响应
// ============================================================
class Connection;
using FrameCallback = std::function<void(const Frame&, Connection* conn)>;

// ============================================================
// Connection — 客户端连接处理器
//
// 继承 EventHandler，管理一个客户端 socket。
// OnRead：从 socket 读取数据 → Buffer 累积 → 切帧 → 解码 → 回调
// OnWrite：发送缓冲区有数据时，继续写入 socket（ET 模式循环到 EAGAIN）
// OnClose：对端断开或发生错误，从 EventLoop 移除自身
// ============================================================
class Connection : public EventHandler {
public:
    // fd: 客户端 socket fd（已设为非阻塞）
    // loop: 所属的 EventLoop（OnClose 时需要从 loop 移除自己）
    // cb: 收到完整 Frame 后的回调
    Connection(int fd, EventLoop* loop, FrameCallback cb = nullptr);
    ~Connection() override;

    void OnRead() override;
    void OnWrite() override;
    void OnClose() override;

    // 发送数据（非阻塞）
    // 尝试直接 send，写不完的放入发送缓冲区，注册 EPOLLOUT 等待可写
    void Send(const std::vector<uint8_t>& data);

private:
    EventLoop* loop_;               // 所属事件循环，OnClose 时需要调用 loop_->Unregister(fd_)
    Buffer read_buffer_;            // 读缓冲区，处理粘包/拆包
    FrameCallback frame_callback_;  // 收到完整帧后的回调

    // 发送路径
    std::vector<uint8_t> write_buffer_;  // 发送缓冲区
    size_t write_offset_ = 0;            // 缓冲区中已发送的偏移量
};

} // namespace rpc