#include "rpc/socket.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <stdexcept>

namespace rpc {

Socket::Socket() {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        throw std::runtime_error("socket() failed");
    }
}

Socket::Socket(int fd) : fd_(fd) {
}

Socket::~Socket() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0)
            close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void Socket::Bind(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET; // IPv4
    addr.sin_addr.s_addr = INADDR_ANY; // 绑定到所有接口
    addr.sin_port = htons(port);

    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw std::runtime_error("bind() failed");
    }
}

void Socket::Listen(int backlog) {
    if (listen(fd_, backlog) < 0) {
        throw std::runtime_error("listen() failed");
    }
}

int Socket::Accept() {
    int client_fd = accept(fd_, nullptr, nullptr);
    return client_fd; // 非阻塞下可能返回 -1（errno=EAGAIN）
}

void Socket::SetNonBlocking() {
    SetNonBlocking(fd_);
}

void Socket::SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0); // 获取当前文件状态标志
    if (flags < 0) {
        throw std::runtime_error("fcntl(F_GETFL) failed");
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error("fcntl(F_SETFL O_NONBLOCK) failed");
    }
}

} // namespace rpc