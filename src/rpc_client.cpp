#include "rpc/rpc_client.h"
#include "rpc/protocol.h"
#include "rpc/socket.h"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace rpc {

RpcClient::RpcClient() {
    // 启动 EventLoop 后台线程
    loop_thread_ = std::thread([this]() {
        loop_.Run();
    });
}

RpcClient::~RpcClient() {
    // 先停 EventLoop 线程，避免后面 Unregister 与 Run() 中的 handlers_ 遍历产生竞态
    loop_.Stop();
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
    // EventLoop 已停止，安全清理
    Disconnect();
}

bool RpcClient::Connect(const std::string& ip, uint16_t port) {
    // 创建 socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        perror("[RpcClient] socket() failed");
        return false;
    }

    // 连接服务端
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        perror("[RpcClient] inet_pton() failed");
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (connect(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("[RpcClient] connect() failed");
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    Socket::SetNonBlocking(server_fd_);

    // 创建 Connection，设置响应回调
    auto* pending_ptr = &pending_requests_;
    auto* pending_mutex_ptr = &pending_mutex_;

    auto response_cb = [pending_ptr, pending_mutex_ptr](const Frame& frame, Connection* /*conn*/) {
        // 收到响应帧：根据 request_id 找到对应的 promise
        std::lock_guard<std::mutex> lock(*pending_mutex_ptr);
        auto it = pending_ptr->find(frame.request_id);
        if (it != pending_ptr->end()) {
            if (frame.msg_type == MessageType::Error) {
                // 错误响应：返回空 body（调用方通过 body 大小判断错误）
                it->second.set_value(std::vector<uint8_t>());
            } else {
                it->second.set_value(frame.body);
            }
            pending_ptr->erase(it);
        } else {
            // request_id=0 是服务端推送帧（RoomEvent 等），静默忽略
            if (frame.request_id != 0) {
                printf("[RpcClient] No pending request for id=%u\n", frame.request_id);
            }
        }
    };

    auto conn = std::make_unique<Connection>(server_fd_, &loop_, std::move(response_cb));
    loop_.Register(std::move(conn), EPOLLIN | EPOLLRDHUP | EPOLLET);

    printf("[RpcClient] Connected to %s:%u\n", ip.c_str(), port);
    return true;
}

std::future<std::vector<uint8_t>> RpcClient::Call(const std::string& method_name,
                                                    const std::vector<uint8_t>& body) {
    // 分配 request_id
    uint32_t id = next_request_id_.fetch_add(1);

    // 创建 promise，获取 future
    std::promise<std::vector<uint8_t>> promise;
    auto future = promise.get_future();

    // 存入 pending 表
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_requests_[id] = std::move(promise);
    }

    // 编码帧
    auto frame_bytes = ProtocolFrame::Encode(id, MessageType::Request, method_name, body);

    // 发送（Connection 内部通过 EventLoop 线程发送）
    // 注意：Send 需要在 EventLoop 线程中执行以避免竞争
    // 这里用 loop_.Enqueue 或直接用 Send（如果 Send 是线程安全的）
    // 简单起见，直接 send
    // 但 Connection::Send 内部用 send()，可以在任意线程调用
    // 然而 send() 在非阻塞 socket 上是线程安全的

    // 实际发送：加锁保护，防止多线程并发 send 导致 TCP 字节流交织
    std::lock_guard<std::mutex> send_lock(send_mutex_);
    ssize_t sent = send(server_fd_, frame_bytes.data(), frame_bytes.size(), MSG_NOSIGNAL);
    if (sent < 0) {
        printf("[RpcClient] send() failed for request_id=%u, errno=%d\n", id, errno);
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_requests_.find(id);
        if (it != pending_requests_.end()) {
            it->second.set_value(std::vector<uint8_t>());
            pending_requests_.erase(it);
        }
    }

    return future;
}

void RpcClient::CloseFd() {
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
}

void RpcClient::Disconnect() {
    if (server_fd_ >= 0) {
        loop_.Unregister(server_fd_);
        server_fd_ = -1;
    }
}

} // namespace rpc