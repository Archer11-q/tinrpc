#include "rpc/socket.h"
#include "rpc/event_loop.h"
#include "rpc/event_handler.h"
#include "rpc/acceptor.h"
#include "rpc/connection.h"
#include "rpc/protocol.h"
#include "rpc/serializer.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// ============================================================
// 简易测试框架
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

void RunTest(const char* name, void (*fn)()) {
    printf("  %-40s ... ", name);
    try {
        fn();
        printf("[PASS]\n");
        g_passed++;
    } catch (const std::exception& e) {
        printf("[FAIL] exception: %s\n", e.what());
        g_failed++;
    } catch (...) {
        printf("[FAIL] unknown exception\n");
        g_failed++;
    }
}

// ============================================================
// MockHandler — 用于单元测试的 EventHandler
// ============================================================
class MockHandler : public rpc::EventHandler {
public:
    explicit MockHandler(int fd) : EventHandler(fd) {}
    int read_count = 0;
    void OnRead() override {
        read_count++;
        // 消费数据 → 清空缓冲区 → 让ET模式下次能正常触发，不会失效
        char buf[64];
        while (read(fd_, buf, sizeof(buf)) > 0) {}
    }
};

// ============================================================
// TestAcceptor — 测试用 accept handler
// ============================================================
class TestAcceptor : public rpc::EventHandler {
public:
    TestAcceptor(int listen_fd, rpc::EventLoop* loop, rpc::FrameCallback cb)
        : EventHandler(listen_fd), loop_(loop), cb_(std::move(cb)) {}

    void OnRead() override {
        while (true) {
            int client = accept(fd_, nullptr, nullptr);
            if (client < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                break;
            }
            rpc::Socket::SetNonBlocking(client);
            auto conn = std::make_unique<rpc::Connection>(client, loop_, cb_);
            loop_->Register(std::move(conn), EPOLLIN | EPOLLET);
        }
    }

private:
    rpc::EventLoop* loop_;
    rpc::FrameCallback cb_;
};

// ============================================================
// Socket 单元测试
// ============================================================

void TestSocketCreate() {
    rpc::Socket sock;
    assert(sock.Fd() >= 0);
}

void TestSocketBindListen() {
    rpc::Socket sock;
    sock.Bind(0);
    sock.Listen();
}

void TestSocketSetNonBlocking() {
    rpc::Socket sock;
    sock.SetNonBlocking();
}

void TestSocketMove() {
    rpc::Socket s1;
    int fd = s1.Fd();
    rpc::Socket s2(std::move(s1));
    assert(s1.Fd() == -1);
    assert(s2.Fd() == fd);
}

// ============================================================
// EventLoop 单元测试
// ============================================================

void TestEventLoopRegisterUnregister() {
    rpc::EventLoop loop;

    int pipefd[2];
    assert(pipe(pipefd) == 0);
    rpc::Socket::SetNonBlocking(pipefd[0]); // 读端非阻塞，配合 ET 模式测试
    rpc::Socket::SetNonBlocking(pipefd[1]); // 写端非阻塞，避免写满管道导致死锁

    auto handler = std::make_unique<MockHandler>(pipefd[0]);
    loop.Register(std::move(handler), EPOLLIN | EPOLLET);

    // 写数据触发可读事件
    const char* msg = "test";
    write(pipefd[1], msg, strlen(msg));

    // 后台运行 100ms 后停止
    std::thread loop_thread([&loop]() { loop.Run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    loop.Stop();
    loop_thread.join();

    close(pipefd[0]);
    close(pipefd[1]);
}

void TestEventLoopStop() {
    rpc::EventLoop loop;
    std::thread loop_thread([&loop]() { loop.Run(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    loop.Stop();
    loop_thread.join();
    // 不崩溃即通过
}

// ============================================================
// 集成测试：完整数据路径
// 服务端: EventLoop + TestAcceptor（epoll 驱动 accept） + Connection
// 客户端: 发送一帧合法 RPC 数据
// 验证: 数据 → socket → Buffer → ProtocolFrame → Frame 回调
// ============================================================

void TestFullDataPath() {
    rpc::EventLoop loop;

    // 1. 创建监听 socket
    rpc::Socket listen_sock;
    int opt = 1;
    setsockopt(listen_sock.Fd(), SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    listen_sock.Bind(0);
    listen_sock.Listen();
    listen_sock.SetNonBlocking();

    // 获取端口号
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    getsockname(listen_sock.Fd(), reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);

    // 2. 同步变量
    std::mutex mtx;
    std::condition_variable cv;
    rpc::Frame received_frame;
    bool frame_received = false;

    // 3. 启动服务端：注册 TestAcceptor（epoll 驱动 accept），启动事件循环
    std::thread server_thread([&]() {
        auto acceptor = std::make_unique<TestAcceptor>(
            listen_sock.Fd(), &loop,
            [&mtx, &received_frame, &frame_received, &cv](const rpc::Frame& f) {
                std::lock_guard<std::mutex> lk(mtx);
                received_frame = f;
                frame_received = true;
                cv.notify_one();
            }
        );
        loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

        // 1 秒后自动停止
        std::thread stopper([&loop]() {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            loop.Stop();
        });
        stopper.detach();
        loop.Run();
    });

    // 4. 短暂等待确保服务端进入 epoll_wait
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 5. 客户端连接并发送数据
    int client_sock = socket(AF_INET, SOCK_STREAM, 0);
    assert(client_sock >= 0);

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    assert(connect(client_sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == 0);

    // 构造帧
    rpc::Serializer ser;
    ser.WriteInt32(42);
    ser.WriteString("hello from test");
    auto frame_bytes = rpc::ProtocolFrame::Encode(
        1, rpc::MessageType::Request, "TestMethod", ser.GetBuffer());

    ssize_t sent = send(client_sock, frame_bytes.data(), frame_bytes.size(), MSG_NOSIGNAL);
    assert(sent == static_cast<ssize_t>(frame_bytes.size()));

    // 6. 等待服务端解码完成
    {
        std::unique_lock<std::mutex> lk(mtx);
        bool ok = cv.wait_for(lk, std::chrono::seconds(2), [&] { return frame_received; });
        assert(ok);
    }

    // 7. 验证帧内容
    assert(received_frame.request_id == 1);
    assert(received_frame.msg_type == rpc::MessageType::Request);
    assert(received_frame.method_name == "TestMethod");

    rpc::Serializer body_reader(received_frame.body);
    auto p1 = body_reader.ReadInt32();
    auto p2 = body_reader.ReadString();
    assert(p1.has_value() && *p1 == 42);
    assert(p2.has_value() && *p2 == "hello from test");

    // 8. 清理
    close(client_sock);
    loop.Stop();
    server_thread.join();
}

// ============================================================
// 入口
// ============================================================

int main() {
    printf("=== Network IO Tests ===\n\n");

    printf("--- Socket ---\n");
    RunTest("TestSocketCreate",         TestSocketCreate);
    RunTest("TestSocketBindListen",     TestSocketBindListen);
    RunTest("TestSocketSetNonBlocking", TestSocketSetNonBlocking);
    RunTest("TestSocketMove",           TestSocketMove);

    printf("\n--- EventLoop ---\n");
    RunTest("TestEventLoopRegisterUnregister", TestEventLoopRegisterUnregister);
    RunTest("TestEventLoopStop",               TestEventLoopStop);

    printf("\n--- Integration ---\n");
    RunTest("TestFullDataPath",         TestFullDataPath);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}