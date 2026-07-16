// ============================================================
// test_game_e2e — 游戏房间端到端流程测试
//
// 覆盖：启动server → 2个模拟客户端 → 创建房间 → 加入
//       → 广播消息 → 离开 → 验证房间自动销毁
//
// 使用 RPC 网络层（EventLoop + Acceptor + Connection）
// 在本地回环上完成客户端↔服务端消息交互。
//
// 客户端使用阻塞 socket，按严格的请求→响应顺序执行。
// 广播通知机制（JoinRoomAndNotify 等）已在 test_room_events.cpp 中验证，
// 本测试专注端到端网络链路。
// ============================================================

#include "rpc/acceptor.h"
#include "rpc/connection.h"
#include "rpc/event_loop.h"
#include "rpc/protocol.h"
#include "rpc/socket.h"

#include "game/game_room.h"
#include "game/room_manager.h"
#include "game/broadcast.h"
#include "game.pb.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <chrono>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static int g_passed = 0;
static int g_failed = 0;

void RunTest(const char* name, void (*fn)()) {
    printf("  %-55s ... ", name);
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
// 客户端辅助
// ============================================================

static int ConnectClient(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    assert(inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
    int ret = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    assert(ret == 0);
    return fd;
}

static void SendAll(int fd, const std::vector<uint8_t>& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t n = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
        assert(n > 0 && "send failed");
        offset += static_cast<size_t>(n);
    }
}

static std::vector<uint8_t> RecvN(int fd, size_t n) {
    std::vector<uint8_t> buf(n);
    size_t offset = 0;
    while (offset < n) {
        ssize_t nread = recv(fd, buf.data() + offset, n - offset, 0);
        assert(nread > 0 && "recv failed or connection closed");
        offset += static_cast<size_t>(nread);
    }
    return buf;
}

static void ClientSendReq(int fd, uint32_t req_id,
                           const std::string& method,
                           const std::vector<uint8_t>& body) {
    auto raw = rpc::ProtocolFrame::Encode(req_id, rpc::MessageType::Request, method, body);
    SendAll(fd, raw);
}

static rpc::Frame ClientRecvFrame(int fd) {
    auto header = RecvN(fd, 13);
    uint32_t total_len =
        (static_cast<uint32_t>(header[2]) << 24) |
        (static_cast<uint32_t>(header[3]) << 16) |
        (static_cast<uint32_t>(header[4]) << 8)  |
        static_cast<uint32_t>(header[5]);
    assert(total_len >= 13 && total_len <= rpc::kMaxFrameSize);

    size_t remaining = total_len - 13;
    std::vector<uint8_t> raw = header;
    if (remaining > 0) {
        auto rest = RecvN(fd, remaining);
        raw.insert(raw.end(), rest.begin(), rest.end());
    }
    auto frame = rpc::ProtocolFrame::Decode(raw);
    assert(frame.has_value() && "frame decode failed");
    return *frame;
}

// ============================================================
// 测试1：基础 Echo 往返
// ============================================================

void TestBasicRoundTrip() {
    rpc::EventLoop loop;
    auto cb = [](const rpc::Frame& f, rpc::Connection* c) {
        if (f.method_name == "Echo") {
            auto rsp = rpc::ProtocolFrame::Encode(
                f.request_id, rpc::MessageType::Response, "Echo", f.body);
            c->Send(rsp);
        }
    };

    auto acc = std::make_unique<rpc::Acceptor>(0, &loop, cb);
    int lfd = acc->GetFd();
    sockaddr_in addr{}; socklen_t alen = sizeof(addr);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen);
    uint16_t port = ntohs(addr.sin_port);
    loop.Register(std::move(acc), EPOLLIN | EPOLLET);

    std::thread st([&loop]() {
        std::thread stopper([&loop]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            loop.Stop();
        });
        stopper.detach();
        loop.Run();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int fd = ConnectClient(port);
    std::vector<uint8_t> body = {0xAA, 0xBB, 0xCC};
    ClientSendReq(fd, 1, "Echo", body);
    auto f = ClientRecvFrame(fd);
    assert(f.request_id == 1);
    assert(f.msg_type == rpc::MessageType::Response);
    assert(f.body == body);

    close(fd);
    loop.Stop();
    st.join();
}

// ============================================================
// 测试2：多请求往返 — 单客户端多轮交互
// ============================================================

void TestMultiRoundTrip() {
    rpc::EventLoop loop;
    int counter = 0;

    auto cb = [&counter](const rpc::Frame& f, rpc::Connection* c) {
        if (f.method_name == "Ping") {
            counter++;
            std::vector<uint8_t> rsp_body = {static_cast<uint8_t>(counter)};
            auto rsp = rpc::ProtocolFrame::Encode(
                f.request_id, rpc::MessageType::Response, "Ping", rsp_body);
            c->Send(rsp);
        }
    };

    auto acc = std::make_unique<rpc::Acceptor>(0, &loop, cb);
    int lfd = acc->GetFd();
    sockaddr_in addr{}; socklen_t alen = sizeof(addr);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen);
    uint16_t port = ntohs(addr.sin_port);
    loop.Register(std::move(acc), EPOLLIN | EPOLLET);

    std::thread st([&loop]() {
        std::thread stopper([&loop]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            loop.Stop();
        });
        stopper.detach();
        loop.Run();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int fd = ConnectClient(port);

    // 3 轮 Ping-Pong
    for (int i = 0; i < 3; i++) {
        ClientSendReq(fd, static_cast<uint32_t>(i + 1), "Ping", {});
        auto f = ClientRecvFrame(fd);
        assert(f.msg_type == rpc::MessageType::Response);
        assert(f.body.size() == 1);
        assert(f.body[0] == static_cast<uint8_t>(i + 1));
    }

    close(fd);
    loop.Stop();
    st.join();
}

// ============================================================
// 测试3：双客户端独立交互
// ============================================================

void TestTwoClients() {
    rpc::EventLoop loop;
    std::unordered_map<std::string, rpc::Connection*> players;

    auto cb = [&players](const rpc::Frame& f, rpc::Connection* c) {
        if (f.method_name == "Login") {
            game::LoginReq req;
            req.ParseFromArray(f.body.data(), static_cast<int>(f.body.size()));
            players[req.token()] = c;

            game::LoginRes res;
            res.set_success(true);
            res.mutable_player_info()->set_player_id(req.token());
            std::string buf; res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(
                f.request_id, rpc::MessageType::Response, "Login",
                std::vector<uint8_t>(buf.begin(), buf.end()));
            c->Send(rsp);
        }
    };

    auto acc = std::make_unique<rpc::Acceptor>(0, &loop, cb);
    int lfd = acc->GetFd();
    sockaddr_in addr{}; socklen_t alen = sizeof(addr);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen);
    uint16_t port = ntohs(addr.sin_port);
    loop.Register(std::move(acc), EPOLLIN | EPOLLET);

    std::thread st([&loop]() {
        std::thread stopper([&loop]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            loop.Stop();
        });
        stopper.detach();
        loop.Run();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int fdA = ConnectClient(port);
    int fdB = ConnectClient(port);

    // A 登录
    {
        game::LoginReq req;
        req.set_token("player_a");
        std::string buf; req.SerializeToString(&buf);
        ClientSendReq(fdA, 1, "Login", std::vector<uint8_t>(buf.begin(), buf.end()));
        auto f = ClientRecvFrame(fdA);
        assert(f.method_name == "Login");
        game::LoginRes res;
        res.ParseFromArray(f.body.data(), static_cast<int>(f.body.size()));
        assert(res.success());
    }

    // B 登录
    {
        game::LoginReq req;
        req.set_token("player_b");
        std::string buf; req.SerializeToString(&buf);
        ClientSendReq(fdB, 2, "Login", std::vector<uint8_t>(buf.begin(), buf.end()));
        auto f = ClientRecvFrame(fdB);
        assert(f.method_name == "Login");
        game::LoginRes res;
        res.ParseFromArray(f.body.data(), static_cast<int>(f.body.size()));
        assert(res.success());
    }

    assert(players.size() == 2);
    assert(players.count("player_a") == 1);
    assert(players.count("player_b") == 1);

    close(fdA);
    close(fdB);
    loop.Stop();
    st.join();
}

// ============================================================
// 测试4：完整房间生命周期（使用基础操作）
// 服务器直接用 player_conns 实现广播，避免 Broadcast 中间层
// ============================================================

void TestE2ERoomLifecycle() {

    rpc::EventLoop loop;
    game::RoomManager room_mgr;
    std::unordered_map<std::string, rpc::Connection*> conns;

    auto cb = [&room_mgr, &conns](const rpc::Frame& f, rpc::Connection* c) {
        // --- Login ---
        if (f.method_name == "Login") {
            game::LoginReq req;
            req.ParseFromArray(f.body.data(), static_cast<int>(f.body.size()));
            conns[req.token()] = c;

            game::LoginRes res;
            res.set_success(true);
            res.mutable_player_info()->set_player_id(req.token());
            std::string buf; res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(
                f.request_id, rpc::MessageType::Response, "Login",
                std::vector<uint8_t>(buf.begin(), buf.end()));
            c->Send(rsp);
        }
        // --- CreateRoom ---
        else if (f.method_name == "CreateRoom") {
            game::CreateRoomReq req;
            req.ParseFromArray(f.body.data(), static_cast<int>(f.body.size()));
            game::GameRoom::Config cfg;
            cfg.max_players = req.max_players() > 0 ? req.max_players() : 4;
            auto result = room_mgr.CreateRoom(req.player_id(), cfg);

            game::CreateRoomRes res;
            res.set_success(result.ok);
            if (result.ok) {
                room_mgr.GetRoom(result.room_id)->SetState(game::ROOM_STATE_WAITING);
                *res.mutable_room_info() = room_mgr.GetRoom(result.room_id)->ToProto();
            }
            std::string buf; res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(
                f.request_id, rpc::MessageType::Response, "CreateRoom",
                std::vector<uint8_t>(buf.begin(), buf.end()));
            c->Send(rsp);
        }
        // --- JoinRoom ---
        else if (f.method_name == "JoinRoom") {
            game::JoinRoomReq req;
            req.ParseFromArray(f.body.data(), static_cast<int>(f.body.size()));

            // 基础操作（不广播，避免在回调中向其他 conn 发送的复杂性）
            auto result = room_mgr.JoinRoom(req.room_id(), req.player_id());

            // 加入成功后，手动向房间内其他人推送通知
            if (result.ok) {
                auto* room = room_mgr.GetRoom(req.room_id());
                game::PlayerJoinNtf ntf;
                ntf.set_room_id(req.room_id());
                ntf.set_player_id(req.player_id());
                ntf.set_player_count(room->player_count());
                std::string nbuf; ntf.SerializeToString(&nbuf);
                auto nframe = rpc::ProtocolFrame::Encode(
                    0, rpc::MessageType::Response, "RoomEvent",
                    std::vector<uint8_t>(nbuf.begin(), nbuf.end()));
                for (const auto& pid : room->players()) {
                    if (pid == req.player_id()) continue;  // 排除加入者自己
                    auto it = conns.find(pid);
                    if (it != conns.end() && it->second)
                        it->second->Send(nframe);
                }
            }

            game::JoinRoomRes res;
            res.set_success(result.ok);
            if (result.ok) {
                auto* room = room_mgr.GetRoom(req.room_id());
                if (room) *res.mutable_room_info() = room->ToProto();
            }
            std::string buf; res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(
                f.request_id, rpc::MessageType::Response, "JoinRoom",
                std::vector<uint8_t>(buf.begin(), buf.end()));
            c->Send(rsp);
        }
        // --- BroadcastMsg ---
        else if (f.method_name == "BroadcastMsg") {
            game::RoomBroadcastMsg msg;
            msg.ParseFromArray(f.body.data(), static_cast<int>(f.body.size()));
            auto* room = room_mgr.GetRoom(msg.room_id());
            if (room) {
                std::string mbuf; msg.SerializeToString(&mbuf);
                auto mframe = rpc::ProtocolFrame::Encode(
                    0, rpc::MessageType::Response, "RoomEvent",
                    std::vector<uint8_t>(mbuf.begin(), mbuf.end()));
                for (const auto& pid : room->players()) {
                    auto it = conns.find(pid);
                    if (it != conns.end() && it->second)
                        it->second->Send(mframe);
                }
            }
            // ACK
            auto ack = rpc::ProtocolFrame::Encode(
                f.request_id, rpc::MessageType::Response, "BroadcastMsg",
                std::vector<uint8_t>{0x01});
            c->Send(ack);
        }
        // --- LeaveRoom ---
        else if (f.method_name == "LeaveRoom") {
            game::LeaveRoomReq req;
            req.ParseFromArray(f.body.data(), static_cast<int>(f.body.size()));

            auto* room = room_mgr.GetRoom(req.room_id());
            auto result = room_mgr.LeaveRoom(req.room_id(), req.player_id());

            // 离开成功后，向剩余玩家推送通知
            if (result.ok && room && room->state() != game::ROOM_STATE_DESTROYED) {
                game::PlayerLeaveNtf ntf;
                ntf.set_room_id(req.room_id());
                ntf.set_player_id(req.player_id());
                ntf.set_player_count(room->player_count());
                std::string nbuf; ntf.SerializeToString(&nbuf);
                auto nframe = rpc::ProtocolFrame::Encode(
                    0, rpc::MessageType::Response, "RoomEvent",
                    std::vector<uint8_t>(nbuf.begin(), nbuf.end()));
                for (const auto& pid : room->players()) {
                    auto it = conns.find(pid);
                    if (it != conns.end() && it->second)
                        it->second->Send(nframe);
                }
            }

            game::LeaveRoomRes res;
            res.set_success(result.ok);
            if (!result.ok) res.set_error_msg("leave failed");
            std::string buf; res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(
                f.request_id, rpc::MessageType::Response, "LeaveRoom",
                std::vector<uint8_t>(buf.begin(), buf.end()));
            c->Send(rsp);
        }
    };

    // 启动服务端
    auto acc = std::make_unique<rpc::Acceptor>(0, &loop, cb);
    int lfd = acc->GetFd();
    sockaddr_in addr{}; socklen_t alen = sizeof(addr);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &alen);
    uint16_t port = ntohs(addr.sin_port);
    loop.Register(std::move(acc), EPOLLIN | EPOLLET);

    std::thread st([&loop]() {
        std::thread stopper([&loop]() {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            loop.Stop();
        });
        stopper.detach();
        loop.Run();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 客户端连接
    int fdA = ConnectClient(port);
    int fdB = ConnectClient(port);

    // Step 1: A 登录
    {
        game::LoginReq req; req.set_token("player_a");
        std::string buf; req.SerializeToString(&buf);
        ClientSendReq(fdA, 1, "Login", std::vector<uint8_t>(buf.begin(), buf.end()));
        auto f = ClientRecvFrame(fdA);
        assert(f.method_name == "Login");
    }

    // Step 2: B 登录
    {
        game::LoginReq req; req.set_token("player_b");
        std::string buf; req.SerializeToString(&buf);
        ClientSendReq(fdB, 2, "Login", std::vector<uint8_t>(buf.begin(), buf.end()));
        auto f = ClientRecvFrame(fdB);
        assert(f.method_name == "Login");
    }

    // Step 3: A 创建房间
    std::string room_id;
    {
        game::CreateRoomReq req;
        req.set_player_id("player_a"); req.set_max_players(4);
        std::string buf; req.SerializeToString(&buf);
        ClientSendReq(fdA, 3, "CreateRoom", std::vector<uint8_t>(buf.begin(), buf.end()));
        auto f = ClientRecvFrame(fdA);
        assert(f.method_name == "CreateRoom");
        game::CreateRoomRes res;
        res.ParseFromArray(f.body.data(), static_cast<int>(f.body.size()));
        assert(res.success());
        room_id = res.room_info().room_id();
        assert(!room_id.empty());
    }

    // Step 4: B 加入房间
    {
        game::JoinRoomReq req;
        req.set_player_id("player_b"); req.set_room_id(room_id);
        std::string buf; req.SerializeToString(&buf);
        ClientSendReq(fdB, 4, "JoinRoom", std::vector<uint8_t>(buf.begin(), buf.end()));

        // B 收到 JoinRoomRes
        auto f = ClientRecvFrame(fdB);
        assert(f.method_name == "JoinRoom");
        game::JoinRoomRes res;
        res.ParseFromArray(f.body.data(), static_cast<int>(f.body.size()));
        assert(res.success());
        assert(res.room_info().player_count() == 2);

        // A 收到 PlayerJoinNtf
        auto ntf = ClientRecvFrame(fdA);
        assert(ntf.method_name == "RoomEvent");

        assert(room_mgr.GetRoom(room_id)->player_count() == 2);
    }

    // Step 5: A 发送广播 → B 收到
    {
        game::RoomBroadcastMsg msg;
        msg.set_room_id(room_id); msg.set_sender_id("player_a");
        msg.set_content("hello!"); msg.set_timestamp(1000);
        std::string buf; msg.SerializeToString(&buf);
        ClientSendReq(fdA, 5, "BroadcastMsg", std::vector<uint8_t>(buf.begin(), buf.end()));

        // 服务器先发 RoomEvent（给所有玩家含发送者），后发 ACK
        // 所以 A 先收到广播，再收到 ACK
        auto bcA = ClientRecvFrame(fdA);
        assert(bcA.method_name == "RoomEvent");

        // A 收到 ACK
        auto ack = ClientRecvFrame(fdA);
        assert(ack.method_name == "BroadcastMsg");

        // B 收到广播
        auto bcB = ClientRecvFrame(fdB);
        assert(bcB.method_name == "RoomEvent");
    }

    // Step 6: B 离开
    {
        game::LeaveRoomReq req;
        req.set_player_id("player_b"); req.set_room_id(room_id);
        std::string buf; req.SerializeToString(&buf);
        ClientSendReq(fdB, 6, "LeaveRoom", std::vector<uint8_t>(buf.begin(), buf.end()));

        auto f = ClientRecvFrame(fdB);
        assert(f.method_name == "LeaveRoom");

        // A 收到 PlayerLeaveNtf
        auto ntf = ClientRecvFrame(fdA);
        assert(ntf.method_name == "RoomEvent");

        assert(room_mgr.GetRoom(room_id)->player_count() == 1);
    }

    // Step 7: A 离开 → 房间自动销毁
    {
        game::LeaveRoomReq req;
        req.set_player_id("player_a"); req.set_room_id(room_id);
        std::string buf; req.SerializeToString(&buf);
        ClientSendReq(fdA, 7, "LeaveRoom", std::vector<uint8_t>(buf.begin(), buf.end()));

        auto f = ClientRecvFrame(fdA);
        assert(f.method_name == "LeaveRoom");

        auto* room = room_mgr.GetRoom(room_id);
        assert(room && room->state() == game::ROOM_STATE_DESTROYED);
    }

    // Step 8: 清理
    assert(room_mgr.CleanupDestroyed() == 1);
    assert(room_mgr.room_count() == 0);

    close(fdA);
    close(fdB);
    loop.Stop();
    st.join();
}

// ============================================================
// 入口
// ============================================================

int main() {
    setbuf(stdout, NULL);
    printf("=== 游戏房间端到端流程测试 ===\n\n");

    printf("[基础] Echo 往返\n");
    RunTest("Echo 请求→响应 往返", TestBasicRoundTrip);

    printf("[基础] 多轮 Ping-Pong\n");
    RunTest("单客户端 3 轮往返", TestMultiRoundTrip);

    printf("[基础] 双客户端各自登录\n");
    RunTest("两个客户端分别登录", TestTwoClients);

    printf("\n[E2E] 完整房间生命周期\n");
    RunTest("创建→加入→广播→离开→销毁", TestE2ERoomLifecycle);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
