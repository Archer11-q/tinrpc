#include "rpc/acceptor.h"
#include "rpc/connection.h"
#include "rpc/event_loop.h"

#include <sys/epoll.h>
#include <cerrno>
#include <cstdio>
#include <sys/socket.h>

namespace rpc {

Acceptor::Acceptor(uint16_t port, EventLoop* loop, FrameCallback cb)
    : loop_(loop), cb_(std::move(cb))
{
    // 设置 socket 重用地址，避免重启时 "Address already in use"
    int opt = 1;
    setsockopt(listen_sock_.Fd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    listen_sock_.Bind(port);
    listen_sock_.Listen();
    listen_sock_.SetNonBlocking();

    fd_ = listen_sock_.Fd();
}

void Acceptor::OnRead() {
    while (true) {
        int client_fd = listen_sock_.Accept();
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // 所有新连接已 accept
            }
            // 其他错误，继续尝试（EINTR 等）
            if (errno == EINTR) continue;
            break;
        }

        // 设为非阻塞
        Socket::SetNonBlocking(client_fd);

        // 创建 Connection 并注册到 EventLoop
        auto conn = std::make_unique<Connection>(client_fd, loop_, cb_); // 绑定 客户端fd、事件循环、回调
        loop_->Register(std::move(conn), EPOLLIN | EPOLLET);  // 注册可读事件，边缘触发到Evenloop
    }
}

} // namespace rpc