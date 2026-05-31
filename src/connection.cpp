#include "rpc/connection.h"
#include "rpc/event_loop.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

namespace rpc {

Connection::Connection(int fd, EventLoop* loop, FrameCallback cb)
    : loop_(loop), frame_callback_(std::move(cb))
{
    fd_ = fd;
}

void Connection::OnRead() {
    uint8_t tmp[4096];

    while (true) {
        ssize_t n = recv(fd_, tmp, sizeof(tmp), 0);
        if (n > 0) {
            // 追加到 Buffer（粘包/拆包由 Buffer 内部处理）
            read_buffer_.Append(tmp, static_cast<size_t>(n));
        } else if (n == 0) {
            // 对端正常关闭
            OnClose();
            return;
        } else {
            // n == -1
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // 本轮数据读完，退出循环等待下次 EPOLLIN
            }
            if (errno == EINTR) continue;  // 被信号打断，重试
            // 其他错误
            OnClose();
            return;
        }
    }

    // 尝试从 Buffer 切出完整帧
    while (auto frame_bytes = read_buffer_.TryPopFrame()) {
        auto frame = ProtocolFrame::Decode(*frame_bytes);
        if (frame) {
            if (frame_callback_) {
                frame_callback_(*frame);    // 调用回调传递完整帧，v0.3 用于测试验证，后续改为 Dispatch
            } else {
                // 默认行为：打印帧信息
                printf("[Connection] Received frame: request_id=%u, method=%s, body_size=%zu\n",
                       frame->request_id, frame->method_name.c_str(), frame->body.size());
            }
        }
        // 帧解码失败 → 静默丢弃，继续处理下一个
    }
}

void Connection::OnClose() {
    loop_->Unregister(fd_);
    // fd 由 Socket RAII 管理——但 Connection 不持有 Socket 对象，
    // 所以需要显式 close
    // 注意：只有在这里 close。Acceptor 不通过此路径关闭。
    close(fd_);
}

} // namespace rpc