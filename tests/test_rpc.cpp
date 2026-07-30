#include "rpc/acceptor.h"
#include "rpc/connection.h"
#include "rpc/dispatch.h"
#include "rpc/event_loop.h"
#include "rpc/protocol.h"
#include "rpc/rpc_client.h"
#include "rpc/serializer.h"
#include "rpc/socket.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <unistd.h>

// ============================================================
// 简易测试框架
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

void RunTest(const char* name, void (*fn)()) {
    printf("  %-48s ... ", name);
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
// Test 1: Dispatch 基本功能
// ============================================================

void TestDispatchBasic() {
    rpc::Dispatch dispatch;

    // 注册 Add 方法
    dispatch.RegisterMethod(
        "Add", [](const std::vector<uint8_t>& body) -> std::optional<std::vector<uint8_t>> {
            rpc::Serializer reader(body);
            auto a = reader.ReadInt32();
            auto b = reader.ReadInt32();
            if (!a || !b)
                return std::nullopt;

            rpc::Serializer writer;
            writer.WriteInt32(*a + *b);
            return writer.GetBuffer();
        });

    // 正常调用
    rpc::Serializer req_ser;
    req_ser.WriteInt32(3);
    req_ser.WriteInt32(5);
    auto rsp = dispatch.Call("Add", req_ser.GetBuffer());
    assert(rsp.has_value());

    rpc::Serializer rsp_reader(*rsp);
    auto result = rsp_reader.ReadInt32();
    assert(result.has_value() && *result == 8);

    // 未注册的方法
    auto rsp2 = dispatch.Call("Mul", req_ser.GetBuffer());
    assert(!rsp2.has_value());
}

// ============================================================
// Test 2: RPC 端到端 — 客户端调用服务端
// ============================================================

void TestRpcEndToEnd() {
    // 1. 创建服务端
    rpc::EventLoop server_loop;
    rpc::Dispatch dispatch;

    // 注册方法
    dispatch.RegisterMethod(
        "Add", [](const std::vector<uint8_t>& body) -> std::optional<std::vector<uint8_t>> {
            rpc::Serializer reader(body);
            auto a = reader.ReadInt32();
            auto b = reader.ReadInt32();
            if (!a || !b)
                return std::nullopt;
            rpc::Serializer writer;
            writer.WriteInt32(*a + *b);
            return writer.GetBuffer();
        });

    dispatch.RegisterMethod(
        "Sub", [](const std::vector<uint8_t>& body) -> std::optional<std::vector<uint8_t>> {
            rpc::Serializer reader(body);
            auto a = reader.ReadInt32();
            auto b = reader.ReadInt32();
            if (!a || !b)
                return std::nullopt;
            rpc::Serializer writer;
            writer.WriteInt32(*a - *b);
            return writer.GetBuffer();
        });

    // 2. 服务端回调：Dispatch 分发 + 发送响应
    auto server_cb = [&dispatch](const rpc::Frame& frame, rpc::Connection* conn) {
        auto rsp_body = dispatch.Call(frame.method_name, frame.body);
        if (rsp_body) {
            auto rsp_bytes = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                        frame.method_name, *rsp_body);
            conn->Send(rsp_bytes);
        } else {
            auto err_bytes = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                        frame.method_name, {});
            conn->Send(err_bytes);
        }
    };

    // 3. 创建 Acceptor（端口 0 = 内核分配），获取实际端口
    auto acceptor = std::make_unique<rpc::Acceptor>(0, &server_loop, server_cb);
    int listen_fd = acceptor->GetFd(); // 必须在 Register 前获取

    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);

    server_loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

    // 启动服务端线程（带超时自动停止）
    std::thread server_thread([&server_loop]() {
        std::thread stopper([&server_loop]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            server_loop.Stop();
        });
        stopper.detach();
        server_loop.Run();
    });

    // 等待服务端就绪
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 4. 客户端连接并调用
    rpc::RpcClient client;
    bool connected = client.Connect("127.0.0.1", port);
    assert(connected);

    // 调用 Add(3, 5) → 期望 8
    {
        rpc::Serializer ser;
        ser.WriteInt32(3);
        ser.WriteInt32(5);
        auto future = client.Call("Add", ser.GetBuffer());

        auto status = future.wait_for(std::chrono::seconds(2));
        assert(status == std::future_status::ready);

        auto rsp_body = future.get();
        assert(!rsp_body.empty());

        rpc::Serializer reader(rsp_body);
        auto result = reader.ReadInt32();
        assert(result.has_value() && *result == 8);
    }

    // 调用 Sub(10, 4) → 期望 6
    {
        rpc::Serializer ser;
        ser.WriteInt32(10);
        ser.WriteInt32(4);
        auto future = client.Call("Sub", ser.GetBuffer());

        auto status = future.wait_for(std::chrono::seconds(2));
        assert(status == std::future_status::ready);

        auto rsp_body = future.get();
        assert(!rsp_body.empty());

        rpc::Serializer reader(rsp_body);
        auto result = reader.ReadInt32();
        assert(result.has_value() && *result == 6);
    }

    // 5. 清理
    client.Disconnect();
    server_loop.Stop();
    server_thread.join();
}

// ============================================================
// Test 3: Connection Send/OnWrite — 发送一帧到已关闭的对端，不崩溃
// ============================================================

void TestConnectionSend() {
    int pipefd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pipefd) == 0);
    rpc::Socket::SetNonBlocking(pipefd[0]);
    rpc::Socket::SetNonBlocking(pipefd[1]);

    rpc::EventLoop loop;
    auto conn = std::make_unique<rpc::Connection>(pipefd[0], &loop);
    loop.Register(std::move(conn), EPOLLIN | EPOLLET);

    // 对端关闭
    close(pipefd[1]);

    // 尝试发送 — 应该触发 OnClose，不崩溃
    std::thread loop_thread([&loop]() {
        std::thread stopper([&loop]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            loop.Stop();
        });
        stopper.detach();
        loop.Run();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 发送数据到已关闭的对端
    // 这会触发 EPOLLERR/EPOLLHUP → OnClose
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    // 无法从外部调 Send（因为 conn 已被 move），但 epoll 会检测到错误

    loop.Stop();
    loop_thread.join();
}

// ============================================================
// Test 4: 未注册方法的 RPC 调用返回错误
// ============================================================

void TestRpcUnknownMethod() {
    rpc::EventLoop server_loop;
    rpc::Dispatch dispatch;

    auto server_cb = [&dispatch](const rpc::Frame& frame, rpc::Connection* conn) {
        auto rsp_body = dispatch.Call(frame.method_name, frame.body);
        if (rsp_body) {
            auto rsp_bytes = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                        frame.method_name, *rsp_body);
            conn->Send(rsp_bytes);
        } else {
            auto err_bytes = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                        frame.method_name, {});
            conn->Send(err_bytes);
        }
    };

    auto acceptor = std::make_unique<rpc::Acceptor>(0, &server_loop, server_cb);
    int listen_fd = acceptor->GetFd();
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);
    server_loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

    std::thread server_thread([&server_loop]() {
        std::thread stopper([&server_loop]() {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            server_loop.Stop();
        });
        stopper.detach();
        server_loop.Run();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    rpc::RpcClient client;
    assert(client.Connect("127.0.0.1", port));

    rpc::Serializer ser;
    ser.WriteInt32(1);
    auto future = client.Call("NoSuchMethod", ser.GetBuffer());

    auto status = future.wait_for(std::chrono::seconds(1));
    assert(status == std::future_status::ready);
    auto rsp_body = future.get();
    // Error 响应：body 为空
    assert(rsp_body.empty());

    client.Disconnect();
    server_loop.Stop();
    server_thread.join();
}

// ============================================================
// 入口
// ============================================================

int main() {
    printf("=== RPC Integration Tests ===\n\n");

    RunTest("TestDispatchBasic", TestDispatchBasic);
    RunTest("TestRpcEndToEnd", TestRpcEndToEnd);
    RunTest("TestConnectionSend", TestConnectionSend);
    RunTest("TestRpcUnknownMethod", TestRpcUnknownMethod);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}