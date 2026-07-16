#include "rpc/connection.h"
#include "rpc/event_loop.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>

namespace rpc {

Connection::Connection(int fd, EventLoop* loop, FrameCallback cb,
                       DisconnectCallback on_disconnect)
    : loop_(loop), frame_callback_(std::move(cb))
    , on_disconnect_(std::move(on_disconnect))
{
    fd_ = fd;
}

Connection::~Connection() {
    // RAII 清理：如果 fd 仍然有效则关闭
    // OnClose 中已经 close 过则 fd_ == -1，此处安全
    if (fd_ >= 0) {
        close(fd_);
    }
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
                frame_callback_(*frame, this);  // v0.5: 传递 Connection*，回调可通过它发送响应
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
    // 先通知外部（服务端可借此清理 player_conns 映射 + 移除房间）
    if (on_disconnect_) {
        on_disconnect_(fd_);
    }
    loop_->Unregister(fd_);
    // 显式 close，析构函数检测 fd_ >= 0 不会再 close
    close(fd_);
    fd_ = -1;
}

void Connection::Send(const std::vector<uint8_t>& data) {
    // 如果发送缓冲区为空，尝试直接发送
    if (write_buffer_.empty()) {
        ssize_t n = send(fd_, data.data(), data.size(), MSG_NOSIGNAL);
        if (n == static_cast<ssize_t>(data.size())) {
            return;  // 全部发送完毕
        }
        if (n > 0) {
            // 部分发送：追加未发送部分
            write_buffer_.insert(write_buffer_.end(),
                                 data.begin() + n, data.end());
        } else {
            // n == -1 且 EAGAIN：内核缓冲区满，追加全部数据
            write_buffer_.insert(write_buffer_.end(),
                                 data.begin(), data.end());
        }
    } else {
        // 缓冲区已有待发送数据：追加新数据到末尾，保序
        write_buffer_.insert(write_buffer_.end(), data.begin(), data.end());
    }

    // 注册 EPOLLOUT，等待可写时 OnWrite 继续发送
    loop_->UpdateEvents(fd_, EPOLLIN | EPOLLRDHUP | EPOLLOUT | EPOLLET);
}

void Connection::OnWrite() {
    while (!write_buffer_.empty()) {
        size_t remaining = write_buffer_.size() - write_offset_;
        ssize_t n = send(fd_, write_buffer_.data() + write_offset_,
                         remaining, MSG_NOSIGNAL);
        if (n > 0) {
            write_offset_ += static_cast<size_t>(n);
            if (write_offset_ >= write_buffer_.size()) {
                // 全部发送完毕，清空缓冲区，取消 EPOLLOUT
                write_buffer_.clear();
                write_offset_ = 0;
                loop_->UpdateEvents(fd_, EPOLLIN | EPOLLRDHUP | EPOLLET);
                return;
            }
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;  // 内核缓冲区满，等下次 EPOLLOUT
        } else {
            // 发送错误
            OnClose();
            return;
        }
    }
}

} // namespace rpc