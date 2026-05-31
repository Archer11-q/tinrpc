#pragma once

#include <cstdint>
#include <string>
#include <sys/socket.h>

namespace rpc {

// ============================================================
// Socket — RAII socket 封装
//
// 职责：
// - 管理 socket fd 的生命周期（创建、关闭）
// - 提供 bind、listen、accept 等 POSIX socket 操作的 C++ 封装
// - 设置非阻塞模式
//
// 只支持移动，禁止拷贝（fd 是独占资源）
// ============================================================
class Socket {
public:
    // 创建一个 TCP socket（AF_INET, SOCK_STREAM）
    Socket();

    // 接管一个已有的 fd（通常来自 accept 返回值）
    explicit Socket(int fd);

    ~Socket();

    // 移动语义
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    // 禁止拷贝
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // 绑定到指定端口
    void Bind(uint16_t port);

    // 开始监听，backlog 默认 SOMAXCONN（系统最大值）
    void Listen(int backlog = SOMAXCONN);

    // 接受一个客户端连接，返回客户端 fd
    // 非阻塞模式下可能返回 -1 + errno=EAGAIN（无新连接）
    int Accept();

    // 设置此 socket 为非阻塞模式
    void SetNonBlocking();

    // 静态版本：设置任意 fd 为非阻塞（用于 accept 返回的 fd）
    static void SetNonBlocking(int fd);

    // 获取内部 fd
    int Fd() const { return fd_; }

private:
    int fd_ = -1;
};

} // namespace rpc