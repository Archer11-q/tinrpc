#pragma once

#include "event_loop.h"
#include "connection.h"

#include <string>
#include <vector>
#include <cstdint>
#include <future>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <memory>

namespace rpc {

// ============================================================
/// @brief RpcClient — RPC 客户端
///
/// 职责：
/// - 建立到服务端的 TCP 连接
/// - 发送 RPC 请求（分配 request_id、序列化、帧封装、send）
/// - 管理 pending_requests_ 表（request_id → promise）
/// - 接收响应帧，匹配 request_id，兑现 promise
///
/// 内部持有 EventLoop（后台线程），所有网络 IO 由 EventLoop 驱动
// ============================================================
class RpcClient {
public:
    RpcClient();
    ~RpcClient();

    // 禁止拷贝和移动
    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    /// @brief 连接到服务端
    /// @param ip 服务端 IP 地址
    /// @param port 服务端端口
    /// @return 成功返回 true
    bool Connect(const std::string& ip, uint16_t port);

    /// @brief 发起 RPC 调用
    /// @param method_name 方法名（用于服务端 Dispatch 分发）
    /// @param body 序列化后的参数（由调用方用 Serializer 准备）
    /// @return future，调用方可通过 future.get() 阻塞等待响应 body
    std::future<std::vector<uint8_t>> Call(const std::string& method_name,
                                           const std::vector<uint8_t>& body);

    /// @brief 断开连接并停止 EventLoop
    void Disconnect();

    /// @brief 仅关闭 fd，不操作 EventLoop（析构时才安全清理）
    void CloseFd();

private:
    EventLoop loop_;
    std::thread loop_thread_;
    int server_fd_ = -1; // -1 表示未连接

    std::atomic<uint32_t> next_request_id_{1};

    std::mutex pending_mutex_;
    std::mutex send_mutex_; // 保护 send() 调用，防止多线程并发写同一个 fd
    std::unordered_map<uint32_t, std::promise<std::vector<uint8_t>>> pending_requests_;
};

} // namespace rpc