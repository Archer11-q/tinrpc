// ============================================================
// bench_server — 双模式 benchmark 服务端
//
// 用法：
//   ./bench_server --mode rpc  --port 8080   (TinyRPC 模式)
//   ./bench_server --mode http --port 8080   (HTTP+JSON 模式)
//
// 启动后持续运行，Ctrl+C 优雅关闭。
// ============================================================

#include "rpc/acceptor.h"
#include "rpc/connection.h"
#include "rpc/dispatch.h"
#include "rpc/event_loop.h"
#include "rpc/protocol.h"
#include "rpc/serializer.h"
#include "rpc/socket.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <optional>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <unistd.h>

// ============================================================
// 全局指针（信号处理使用）
// ============================================================
static rpc::EventLoop* g_loop = nullptr;

static void SignalHandler(int) {
    if (g_loop) {
        g_loop->Stop();
        printf("\n[bench_server] 收到退出信号，正在关闭...\n");
    }
}

// ============================================================
// 共享业务逻辑 — 与 RPC 服务端完全相同的实现
// ============================================================
static int Add(int a, int b) { return a + b; }
static int Sub(int a, int b) { return a - b; }

// ============================================================
// 最小化 JSON 工具（仅支持 int 单层对象）
// ============================================================
namespace json {

// 从 {"key1":val1,"key2":val2} 中提取整数值
// 朴素解析，不处理转义、Unicode、嵌套、数组
static std::optional<int> ExtractInt(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return std::nullopt;
    pos += search.size();
    while (pos < json.size() && json[pos] == ' ') pos++;
    size_t end = pos;
    if (end < json.size() && json[end] == '-') end++;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') end++;
    if (end == pos) return std::nullopt;
    return std::stoi(json.substr(pos, end - pos));
}

// 构造 {"result":val}
static std::string FormatResult(int value) {
    return "{\"result\":" + std::to_string(value) + "}";
}

// 构造 {"error":"..."}
static std::string FormatError(const std::string& msg) {
    return "{\"error\":\"" + msg + "\"}";
}

} // namespace json

// ============================================================
// HTTP 模式：最小化 HTTP/1.1 解析
// ============================================================

// 从请求头提取 Content-Length 值（兼容大小写）
static int ParseContentLength(const std::string& header) {
    // 查找 "Content-Length:" 或 "content-length:"
    auto pos = header.find("Content-Length:");
    if (pos == std::string::npos) pos = header.find("content-length:");
    if (pos == std::string::npos) return -1;

    pos += 15;  // strlen("Content-Length:")
    while (pos < header.size() && header[pos] == ' ') pos++;

    size_t end = pos;
    while (end < header.size() && header[end] >= '0' && header[end] <= '9') end++;
    if (end == pos) return -1;

    return std::stoi(header.substr(pos, end - pos));
}

// 从请求行 "POST /rpc/MethodName HTTP/1.1" 中提取路径
static std::string ParsePath(const std::string& header) {
    size_t first = header.find(' ');
    if (first == std::string::npos) return "";
    size_t second = header.find(' ', first + 1);
    if (second == std::string::npos) return "";
    return header.substr(first + 1, second - first - 1);
}

// ============================================================
// HTTP 模式：连接处理器（支持 keep-alive，持续处理请求直至对端关闭）
// ============================================================
class HttpConnection : public rpc::EventHandler {
public:
    HttpConnection(int client_fd, rpc::EventLoop* loop)
        : loop_(loop)
    {
        fd_ = client_fd;  // 设置基类 fd_
    }

    void OnRead() override {
        // 循环读取直到 EAGAIN
        char buf[4096];
        while (true) {
            ssize_t n = recv(fd_, buf, sizeof(buf), 0);
            if (n > 0) {
                read_buf_.append(buf, static_cast<size_t>(n));
            } else if (n == 0) {
                // 对端正常关闭 — 退出
                OnClose();
                return;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                // 其他错误 — 退出
                OnClose();
                return;
            }
        }

        // 循环处理缓冲区中的所有完整请求（HTTP pipelining）
        while (ProcessOneRequest()) {
            // ProcessOneRequest 在成功后已从 read_buf_ 头部消耗掉该请求
        }
        // 请求不完整 → 返回，等待更多数据触发下一次 OnRead
    }

    void OnClose() override {
        // 先保存并关闭 fd，再 Unregister（会销毁本对象）
        int fd = fd_;
        fd_ = -1;
        close(fd);
        loop_->Unregister(fd);
    }

private:
    // 尝试从 read_buf_ 头部解析一个完整 HTTP 请求并处理
    // 成功返回 true 并从缓冲区移除该请求数据；不完整返回 false
    bool ProcessOneRequest() {
        // 查找请求头结束位置 \r\n\r\n
        auto header_end = read_buf_.find("\r\n\r\n");
        if (header_end == std::string::npos) return false;

        // 提取 Content-Length
        int content_length = ParseContentLength(read_buf_);
        if (content_length < 0) {
            SendError(400, "Missing Content-Length");
            // 无法恢复，关闭连接
            OnClose();
            return false;
        }

        // 检查 body 是否完整接收
        size_t total = header_end + 4 + static_cast<size_t>(content_length);
        if (read_buf_.size() < total) return false;

        // 提取请求的头+体（header + body）
        std::string request_data = read_buf_.substr(0, total);

        // 从缓冲区移除已处理的请求
        read_buf_.erase(0, total);

        // 解析 body（header_end 是相对于 request_data 的偏移）
        std::string body = request_data.substr(header_end + 4, static_cast<size_t>(content_length));

        // 提取方法名（/rpc/Add → Add）
        std::string path = ParsePath(request_data);
        auto slash = path.rfind('/');
        std::string method = (slash != std::string::npos) ? path.substr(slash + 1) : path;

        // 提取参数
        auto a = json::ExtractInt(body, "a");
        auto b = json::ExtractInt(body, "b");
        if (!a || !b) {
            SendError(400, "Invalid JSON: missing a or b");
            OnClose();
            return false;
        }

        // 执行业务逻辑
        int result = 0;
        if (method == "Add") {
            result = Add(*a, *b);
        } else if (method == "Sub") {
            result = Sub(*a, *b);
        } else {
            SendError(404, "Unknown method: " + method);
            OnClose();
            return false;
        }

        // 返回成功响应（keep-alive：不关闭连接）
        SendResponse(200, json::FormatResult(result));
        return true;
    }

    void SendResponse(int code, const std::string& body) {
        std::string status_line;
        if (code == 200) status_line = "200 OK";
        else if (code == 400) status_line = "400 Bad Request";
        else status_line = std::to_string(code) + " Error";

        char buf[4096];
        int len = snprintf(buf, sizeof(buf),
            "HTTP/1.1 %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "%s",
            status_line.c_str(), body.size(), body.c_str());
        send(fd_, buf, static_cast<size_t>(len), MSG_NOSIGNAL);
    }

    void SendError(int code, const std::string& msg) {
        SendResponse(code, json::FormatError(msg));
    }

    rpc::EventLoop* loop_;
    std::string read_buf_;
};

// ============================================================
// HTTP 模式：监听器
// ============================================================
class HttpAcceptor : public rpc::EventHandler {
public:
    HttpAcceptor(uint16_t port, rpc::EventLoop* loop)
        : loop_(loop)
    {
        int opt = 1;
        setsockopt(listen_sock_.Fd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        listen_sock_.Bind(port);
        listen_sock_.Listen();
        listen_sock_.SetNonBlocking();
        fd_ = listen_sock_.Fd();  // 设置基类 fd_
    }

    void OnRead() override {
        while (true) {
            int client_fd = listen_sock_.Accept();
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                break;
            }
            rpc::Socket::SetNonBlocking(client_fd);
            auto conn = std::make_unique<HttpConnection>(client_fd, loop_);
            loop_->Register(std::move(conn), EPOLLIN | EPOLLET);
        }
    }

private:
    rpc::Socket listen_sock_;
    rpc::EventLoop* loop_;
};

// ============================================================
// RPC 模式服务端
// ============================================================
static void RunRpcServer(uint16_t port) {
    rpc::EventLoop loop;
    g_loop = &loop;

    // 注册业务方法
    rpc::Dispatch dispatch;
    dispatch.RegisterMethod("Add", [](const std::vector<uint8_t>& body) -> std::optional<std::vector<uint8_t>> {
        rpc::Serializer reader(body);
        auto a = reader.ReadInt32();
        auto b = reader.ReadInt32();
        if (!a || !b) return std::nullopt;
        rpc::Serializer writer;
        writer.WriteInt32(Add(*a, *b));
        return writer.GetBuffer();
    });

    dispatch.RegisterMethod("Sub", [](const std::vector<uint8_t>& body) -> std::optional<std::vector<uint8_t>> {
        rpc::Serializer reader(body);
        auto a = reader.ReadInt32();
        auto b = reader.ReadInt32();
        if (!a || !b) return std::nullopt;
        rpc::Serializer writer;
        writer.WriteInt32(Sub(*a, *b));
        return writer.GetBuffer();
    });

    // 帧回调：Dispatch 分发 → 编码响应 → 发送
    auto server_cb = [&dispatch](const rpc::Frame& frame, rpc::Connection* conn) {
        auto rsp_body = dispatch.Call(frame.method_name, frame.body);
        if (rsp_body) {
            auto rsp_bytes = rpc::ProtocolFrame::Encode(
                frame.request_id, rpc::MessageType::Response, frame.method_name, *rsp_body);
            conn->Send(rsp_bytes);
        } else {
            auto err_bytes = rpc::ProtocolFrame::Encode(
                frame.request_id, rpc::MessageType::Error, frame.method_name, {});
            conn->Send(err_bytes);
        }
    };

    auto acceptor = std::make_unique<rpc::Acceptor>(port, &loop, server_cb);
    loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

    printf("[bench_server] RPC 模式，监听端口 %u\n", port);
    printf("[bench_server] 已注册方法: Add, Sub\n");
    fflush(stdout);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    loop.Run();
    printf("[bench_server] RPC 服务端已关闭\n");
}

// ============================================================
// HTTP 模式服务端
// ============================================================
static void RunHttpServer(uint16_t port) {
    rpc::EventLoop loop;
    g_loop = &loop;

    auto acceptor = std::make_unique<HttpAcceptor>(port, &loop);
    loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

    printf("[bench_server] HTTP+JSON 模式，监听端口 %u\n", port);
    printf("[bench_server] 端点: POST /rpc/Add, POST /rpc/Sub\n");
    fflush(stdout);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    loop.Run();
    printf("[bench_server] HTTP+JSON 服务端已关闭\n");
}

// ============================================================
// 入口
// ============================================================
int main(int argc, char* argv[]) {
    const char* mode = "rpc";
    uint16_t port = 8080;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("用法: bench_server --mode rpc|http --port PORT\n");
            printf("  --mode    rpc = TinyRPC 协议, http = HTTP+JSON\n");
            printf("  --port    监听端口 (默认 8080)\n");
            return 0;
        }
    }

    printf("=== TinyRPC Benchmark Server ===\n");

    if (strcmp(mode, "rpc") == 0) {
        RunRpcServer(port);
    } else if (strcmp(mode, "http") == 0) {
        RunHttpServer(port);
    } else {
        fprintf(stderr, "错误: 未知模式 '%s'（应为 rpc 或 http）\n", mode);
        return 1;
    }

    return 0;
}