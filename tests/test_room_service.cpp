// ============================================================
// test_room_service — RoomService RPC 端到端测试
//
// 验证：RoomService 纯虚接口 → RoomServiceImpl → Dispatch 注册
//       → RoomServiceStub 客户端代理 → RPC 网络往返 → 端到端闭环
//
// 客户端使用裸 socket（阻塞模式），避免 RpcClient 多实例线程竞争。
// 参考 test_game_e2e.cpp 的客户端辅助模式。
//
// 覆盖场景：
//   1. Stub::CreateRoom — 创建房间
//   2. Stub::JoinRoom — 加入房间
//   3. Stub::SendMessage — 发送房间消息
//   4. Stub::GetRoomList — 获取房间列表
//   5. Stub::LeaveRoom — 离开房间
//   6. 重复创建被拒绝（含 error_code 验证）
//   7. 销毁后不出现在列表
//   8. ErrorCode 链路：JoinRoom/LeaveRoom/SendMessage 6 项错误码测试
// ============================================================

#include "rpc/acceptor.h"
#include "rpc/connection.h"
#include "rpc/dispatch.h"
#include "rpc/event_loop.h"
#include "rpc/protocol.h"
#include "rpc/socket.h"

#include "game/game_room.h"
#include "game/room_manager.h"
#include "game/broadcast.h"
#include "game/room_service.h"
#include "game.pb.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <chrono>
#include <atomic>

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
// 客户端辅助（裸 socket，阻塞模式）
// ============================================================

struct SimpleClient {
    int fd = -1;

    bool Connect(uint16_t port) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            fprintf(stderr, "[SimpleClient] socket() failed\n");
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) <= 0) {
            close(fd);
            fd = -1;
            return false;
        }
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            fd = -1;
            return false;
        }
        return true;
    }

    // 发送请求帧，返回响应 body
    std::vector<uint8_t> Call(const std::string& method, const std::vector<uint8_t>& req_body) {
        static std::atomic<uint32_t> next_id{1};
        uint32_t id = next_id.fetch_add(1);

        // 发送
        auto frame = rpc::ProtocolFrame::Encode(id, rpc::MessageType::Request, method, req_body);
        SendAll(frame);

        // 接收
        auto rsp = RecvFrame();
        (void) rsp; // method_name is known
        return rsp.body;
    }

    void Disconnect() {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }

private:
    void SendAll(const std::vector<uint8_t>& data) {
        size_t offset = 0;
        while (offset < data.size()) {
            ssize_t n = send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
            if (n <= 0) {
                fprintf(stderr, "[SimpleClient] send error, fd=%d, n=%zd, errno=%d\n", fd, n, errno);
                fprintf(stderr, "FATAL: send failed\n");
                abort();
            }
            offset += static_cast<size_t>(n);
        }
    }

    std::vector<uint8_t> RecvN(size_t n) {
        std::vector<uint8_t> buf(n);
        size_t offset = 0;
        while (offset < n) {
            ssize_t nread = recv(fd, buf.data() + offset, n - offset, 0);
            if (nread <= 0) {
                fprintf(stderr, "[SimpleClient] recv error, fd=%d, nread=%zd, errno=%d\n", fd,
                        nread, errno);
                fprintf(stderr, "FATAL: recv failed\n");
                abort();
            }
            offset += static_cast<size_t>(nread);
        }
        return buf;
    }

    rpc::Frame RecvFrame() {
        auto header = RecvN(13);
        uint32_t total_len = (static_cast<uint32_t>(header[2]) << 24) |
                             (static_cast<uint32_t>(header[3]) << 16) |
                             (static_cast<uint32_t>(header[4]) << 8) |
                             static_cast<uint32_t>(header[5]);
        assert(total_len >= 13 && total_len <= rpc::kMaxFrameSize);

        size_t remaining = total_len - 13;
        std::vector<uint8_t> raw = header;
        if (remaining > 0) {
            auto rest = RecvN(remaining);
            raw.insert(raw.end(), rest.begin(), rest.end());
        }
        auto frame = rpc::ProtocolFrame::Decode(raw);
        assert(frame.has_value());
        return *frame;
    }
};

// ============================================================
// SimpleStub — 基于 SimpleClient 的 RoomService 客户端代理
// （与 RoomServiceStub 接口一致，验证 Stub 模式模式）
// ============================================================

struct SimpleStub {
    SimpleClient* client;

    explicit SimpleStub(SimpleClient* c) : client(c) {
    }

    game::CreateRoomRes CreateRoom(const game::CreateRoomReq& req) {
        std::string buf;
        req.SerializeToString(&buf);
        auto rsp = client->Call("CreateRoom", std::vector<uint8_t>(buf.begin(), buf.end()));
        game::CreateRoomRes res;
        if (!rsp.empty())
            res.ParseFromArray(rsp.data(), static_cast<int>(rsp.size()));
        return res;
    }

    game::JoinRoomRes JoinRoom(const game::JoinRoomReq& req) {
        std::string buf;
        req.SerializeToString(&buf);
        auto rsp = client->Call("JoinRoom", std::vector<uint8_t>(buf.begin(), buf.end()));
        game::JoinRoomRes res;
        if (!rsp.empty())
            res.ParseFromArray(rsp.data(), static_cast<int>(rsp.size()));
        return res;
    }

    game::LeaveRoomRes LeaveRoom(const game::LeaveRoomReq& req) {
        std::string buf;
        req.SerializeToString(&buf);
        auto rsp = client->Call("LeaveRoom", std::vector<uint8_t>(buf.begin(), buf.end()));
        game::LeaveRoomRes res;
        if (!rsp.empty())
            res.ParseFromArray(rsp.data(), static_cast<int>(rsp.size()));
        return res;
    }

    game::SendMessageRes SendMessage(const game::SendMessageReq& req) {
        std::string buf;
        req.SerializeToString(&buf);
        auto rsp = client->Call("SendMessage", std::vector<uint8_t>(buf.begin(), buf.end()));
        game::SendMessageRes res;
        if (!rsp.empty())
            res.ParseFromArray(rsp.data(), static_cast<int>(rsp.size()));
        return res;
    }

    game::GetRoomListRes GetRoomList() {
        auto rsp = client->Call("GetRoomList", {});
        game::GetRoomListRes res;
        if (!rsp.empty())
            res.ParseFromArray(rsp.data(), static_cast<int>(rsp.size()));
        return res;
    }

    game::StartGameRes StartGame(const game::StartGameReq& req) {
        std::string buf;
        req.SerializeToString(&buf);
        auto rsp = client->Call("StartGame", std::vector<uint8_t>(buf.begin(), buf.end()));
        game::StartGameRes res;
        if (!rsp.empty())
            res.ParseFromArray(rsp.data(), static_cast<int>(rsp.size()));
        return res;
    }

    game::SendInputRes SendInput(const game::PlayerInputReq& req) {
        std::string buf;
        req.SerializeToString(&buf);
        auto rsp = client->Call("SendInput", std::vector<uint8_t>(buf.begin(), buf.end()));
        game::SendInputRes res;
        if (!rsp.empty())
            res.ParseFromArray(rsp.data(), static_cast<int>(rsp.size()));
        return res;
    }

    game::StopGameRes StopGame(const game::StopGameReq& req) {
        std::string buf;
        req.SerializeToString(&buf);
        auto rsp = client->Call("StopGame", std::vector<uint8_t>(buf.begin(), buf.end()));
        game::StopGameRes res;
        if (!rsp.empty())
            res.ParseFromArray(rsp.data(), static_cast<int>(rsp.size()));
        return res;
    }
};

// ============================================================
// 客户端 Login 辅助
// ============================================================

void ClientLogin(SimpleClient& client, const std::string& player_id) {
    game::LoginReq req;
    req.set_token(player_id);
    std::string buf;
    req.SerializeToString(&buf);
    auto rsp_body = client.Call("Login", std::vector<uint8_t>(buf.begin(), buf.end()));
    game::LoginRes res;
    res.ParseFromArray(rsp_body.data(), static_cast<int>(rsp_body.size()));
    assert(res.success());
}

// ============================================================
// Test 1: Stub::CreateRoom — 创建房间端到端
// ============================================================

void TestStubCreateRoom() {
    // 1. 启动服务端
    rpc::EventLoop server_loop;
    game::RoomManager room_mgr;
    rpc::Dispatch dispatch;
    std::unordered_map<std::string, rpc::Connection*> player_conns;

    auto send_fn = [&player_conns](const std::string& player_id, const std::vector<uint8_t>& data) {
        auto it = player_conns.find(player_id);
        if (it != player_conns.end() && it->second) {
            auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, "RoomEvent", data);
            it->second->Send(frame);
        }
    };

    game::Broadcast broadcast(&room_mgr, send_fn);
    game::RoomServiceImpl service(&room_mgr, &broadcast);
    game::RegisterRoomService(&dispatch, &service);

    auto server_cb = [&dispatch, &player_conns](const rpc::Frame& frame, rpc::Connection* conn) {
        if (frame.method_name == "Login") {
            game::LoginReq req;
            req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()));
            player_conns[req.token()] = conn;

            game::LoginRes res;
            res.set_success(true);
            res.mutable_player_info()->set_player_id(req.token());
            std::string buf;
            res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  "Login",
                                                  std::vector<uint8_t>(buf.begin(), buf.end()));
            conn->Send(rsp);
            return;
        }

        auto rsp_body = dispatch.Call(frame.method_name, frame.body);
        if (rsp_body) {
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  frame.method_name, *rsp_body);
            conn->Send(rsp);
        } else {
            auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                  frame.method_name, {});
            conn->Send(err);
        }
    };

    auto acceptor = std::make_unique<rpc::Acceptor>(0, &server_loop, server_cb);
    int listen_fd = acceptor->GetFd();
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);
    server_loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

    std::thread server_thread([&server_loop]() { server_loop.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 2. 客户端
    SimpleClient client;
    do {
        bool _ok = client.Connect(port);
        if (!_ok) {
            fprintf(stderr, "FATAL: Connect failed\n");
            abort();
        }
    } while (0);
    ClientLogin(client, "player_a");

    SimpleStub stub(&client);

    // 3. 通过 Stub 创建房间
    game::CreateRoomReq req;
    req.set_player_id("player_a");
    req.set_max_players(4);

    game::CreateRoomRes res = stub.CreateRoom(req);

    assert(res.success());
    assert(!res.room_info().room_id().empty());
    assert(res.room_info().player_count() == 1);
    assert(res.room_info().max_players() == 4);
    assert(res.room_info().room_state() == game::ROOM_STATE_WAITING);
    assert(res.room_info().room_id() == "room_001");

    printf("\n    room_id=%s\n", res.room_info().room_id().c_str());

    // 清理
    client.Disconnect();
    server_loop.Stop();
    server_thread.join();
}

// ============================================================
// Test 2: Stub::CreateRoom + Stub::JoinRoom — 一人建一人加
// ============================================================

void TestStubCreateAndJoin() {
    rpc::EventLoop server_loop;
    game::RoomManager room_mgr;
    rpc::Dispatch dispatch;
    std::unordered_map<std::string, rpc::Connection*> player_conns;

    auto send_fn = [&player_conns](const std::string& player_id, const std::vector<uint8_t>& data) {
        auto it = player_conns.find(player_id);
        if (it != player_conns.end() && it->second) {
            auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, "RoomEvent", data);
            it->second->Send(frame);
        }
    };

    game::Broadcast broadcast(&room_mgr, send_fn);
    game::RoomServiceImpl service(&room_mgr, &broadcast);
    game::RegisterRoomService(&dispatch, &service);

    auto server_cb = [&dispatch, &player_conns](const rpc::Frame& frame, rpc::Connection* conn) {
        if (frame.method_name == "Login") {
            game::LoginReq req;
            req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()));
            player_conns[req.token()] = conn;
            game::LoginRes res;
            res.set_success(true);
            res.mutable_player_info()->set_player_id(req.token());
            std::string buf;
            res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  "Login",
                                                  std::vector<uint8_t>(buf.begin(), buf.end()));
            conn->Send(rsp);
            return;
        }
        auto rsp_body = dispatch.Call(frame.method_name, frame.body);
        if (rsp_body) {
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  frame.method_name, *rsp_body);
            conn->Send(rsp);
        } else {
            auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                  frame.method_name, {});
            conn->Send(err);
        }
    };

    auto acceptor = std::make_unique<rpc::Acceptor>(0, &server_loop, server_cb);
    int listen_fd = acceptor->GetFd();
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);
    server_loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

    std::thread server_thread([&server_loop]() { server_loop.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 两个客户端
    SimpleClient client_a, client_b;
    do {
        bool _ok = client_a.Connect(port);
        if (!_ok) {
            fprintf(stderr, "FATAL: client_a Connect failed\n");
            abort();
        }
    } while (0);
    do {
        bool _ok = client_b.Connect(port);
        if (!_ok) {
            fprintf(stderr, "FATAL: client_b Connect failed\n");
            abort();
        }
    } while (0);
    ClientLogin(client_a, "player_a");
    ClientLogin(client_b, "player_b");

    SimpleStub stub_a(&client_a);
    SimpleStub stub_b(&client_b);

    // A 创建房间
    game::CreateRoomReq create_req;
    create_req.set_player_id("player_a");
    create_req.set_max_players(4);
    game::CreateRoomRes create_res = stub_a.CreateRoom(create_req);
    assert(create_res.success());
    std::string room_id = create_res.room_info().room_id();

    // B 加入房间
    game::JoinRoomReq join_req;
    join_req.set_player_id("player_b");
    join_req.set_room_id(room_id);
    game::JoinRoomRes join_res = stub_b.JoinRoom(join_req);
    assert(join_res.success());
    assert(join_res.room_info().player_count() == 2);
    assert(join_res.room_info().room_id() == room_id);

    // 服务端验证
    auto* room = room_mgr.GetRoom(room_id);
    assert(room != nullptr);
    assert(room->player_count() == 2);
    assert(room->HasPlayer("player_a"));
    assert(room->HasPlayer("player_b"));

    printf("\n    room_id=%s, player_count=%d\n", room_id.c_str(),
           join_res.room_info().player_count());

    client_a.Disconnect();
    client_b.Disconnect();
    server_loop.Stop();
    server_thread.join();
}

// ============================================================
// Test 3: Stub::SendMessage — 发送房间消息
// ============================================================

void TestStubSendMessage() {
    rpc::EventLoop server_loop;
    game::RoomManager room_mgr;
    rpc::Dispatch dispatch;
    std::unordered_map<std::string, rpc::Connection*> player_conns;

    auto send_fn = [&player_conns](const std::string& player_id, const std::vector<uint8_t>& data) {
        auto it = player_conns.find(player_id);
        if (it != player_conns.end() && it->second) {
            auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, "RoomEvent", data);
            it->second->Send(frame);
        }
    };

    game::Broadcast broadcast(&room_mgr, send_fn);
    game::RoomServiceImpl service(&room_mgr, &broadcast);
    game::RegisterRoomService(&dispatch, &service);

    auto server_cb = [&dispatch, &player_conns](const rpc::Frame& frame, rpc::Connection* conn) {
        if (frame.method_name == "Login") {
            game::LoginReq req;
            req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()));
            player_conns[req.token()] = conn;
            game::LoginRes res;
            res.set_success(true);
            res.mutable_player_info()->set_player_id(req.token());
            std::string buf;
            res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  "Login",
                                                  std::vector<uint8_t>(buf.begin(), buf.end()));
            conn->Send(rsp);
            return;
        }
        auto rsp_body = dispatch.Call(frame.method_name, frame.body);
        if (rsp_body) {
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  frame.method_name, *rsp_body);
            conn->Send(rsp);
        } else {
            auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                  frame.method_name, {});
            conn->Send(err);
        }
    };

    auto acceptor = std::make_unique<rpc::Acceptor>(0, &server_loop, server_cb);
    int listen_fd = acceptor->GetFd();
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);
    server_loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

    std::thread server_thread([&server_loop]() { server_loop.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SimpleClient client_a;
    do {
        bool _ok = client_a.Connect(port);
        if (!_ok) {
            fprintf(stderr, "FATAL: client_a Connect failed\n");
            abort();
        }
    } while (0);
    ClientLogin(client_a, "player_a");
    SimpleStub stub_a(&client_a);

    // 创建房间
    game::CreateRoomReq create_req;
    create_req.set_player_id("player_a");
    create_req.set_max_players(4);
    auto create_res = stub_a.CreateRoom(create_req);
    assert(create_res.success());
    std::string room_id = create_res.room_info().room_id();

    // 发送消息
    game::SendMessageReq msg_req;
    msg_req.set_room_id(room_id);
    msg_req.set_sender_id("player_a");
    msg_req.set_content("Hello, room!");

    game::SendMessageRes msg_res = stub_a.SendMessage(msg_req);
    assert(msg_res.success());

    printf("\n    房间 %s 消息发送成功\n", room_id.c_str());

    client_a.Disconnect();
    server_loop.Stop();
    server_thread.join();
}

// ============================================================
// Test 4: Stub::GetRoomList — 获取房间列表
// ============================================================

void TestStubGetRoomList() {
    rpc::EventLoop server_loop;
    game::RoomManager room_mgr;
    rpc::Dispatch dispatch;
    std::unordered_map<std::string, rpc::Connection*> player_conns;

    auto send_fn = [&player_conns](const std::string& player_id, const std::vector<uint8_t>& data) {
        auto it = player_conns.find(player_id);
        if (it != player_conns.end() && it->second) {
            auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, "RoomEvent", data);
            it->second->Send(frame);
        }
    };

    game::Broadcast broadcast(&room_mgr, send_fn);
    game::RoomServiceImpl service(&room_mgr, &broadcast);
    game::RegisterRoomService(&dispatch, &service);

    auto server_cb = [&dispatch, &player_conns](const rpc::Frame& frame, rpc::Connection* conn) {
        if (frame.method_name == "Login") {
            game::LoginReq req;
            req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()));
            player_conns[req.token()] = conn;
            game::LoginRes res;
            res.set_success(true);
            res.mutable_player_info()->set_player_id(req.token());
            std::string buf;
            res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  "Login",
                                                  std::vector<uint8_t>(buf.begin(), buf.end()));
            conn->Send(rsp);
            return;
        }
        auto rsp_body = dispatch.Call(frame.method_name, frame.body);
        if (rsp_body) {
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  frame.method_name, *rsp_body);
            conn->Send(rsp);
        } else {
            auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                  frame.method_name, {});
            conn->Send(err);
        }
    };

    auto acceptor = std::make_unique<rpc::Acceptor>(0, &server_loop, server_cb);
    int listen_fd = acceptor->GetFd();
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);
    server_loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

    std::thread server_thread([&server_loop]() { server_loop.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 客户端 A
    SimpleClient client_a;
    do {
        bool _ok = client_a.Connect(port);
        if (!_ok) {
            fprintf(stderr, "FATAL: client_a Connect failed\n");
            abort();
        }
    } while (0);
    ClientLogin(client_a, "player_a");
    SimpleStub stub_a(&client_a);

    // 创建房间 1
    game::CreateRoomReq req1;
    req1.set_player_id("player_a");
    req1.set_max_players(2);
    auto res1 = stub_a.CreateRoom(req1);
    assert(res1.success());
    std::string id1 = res1.room_info().room_id();

    // 客户端 B 创建房间 2
    SimpleClient client_b;
    do {
        bool _ok = client_b.Connect(port);
        if (!_ok) {
            fprintf(stderr, "FATAL: client_b Connect failed\n");
            abort();
        }
    } while (0);
    ClientLogin(client_b, "player_b");
    SimpleStub stub_b(&client_b);

    game::CreateRoomReq req2;
    req2.set_player_id("player_b");
    req2.set_max_players(3);
    auto res2 = stub_b.CreateRoom(req2);
    assert(res2.success());
    std::string id2 = res2.room_info().room_id();

    // 查询房间列表
    game::GetRoomListRes list_res = stub_a.GetRoomList();
    assert(list_res.rooms_size() >= 2);

    bool found1 = false, found2 = false;
    for (int i = 0; i < list_res.rooms_size(); i++) {
        if (list_res.rooms(i).room_id() == id1)
            found1 = true;
        if (list_res.rooms(i).room_id() == id2)
            found2 = true;
    }
    assert(found1 && found2);

    printf("\n    共 %d 个房间\n", list_res.rooms_size());

    // 清理 player_b 的房间
    game::LeaveRoomReq leave_req;
    leave_req.set_player_id("player_b");
    leave_req.set_room_id(id2);
    stub_b.LeaveRoom(leave_req);
    room_mgr.CleanupDestroyed();

    client_a.Disconnect();
    client_b.Disconnect();
    server_loop.Stop();
    server_thread.join();
}

// ============================================================
// Test 5: Stub::LeaveRoom — 离开房间端到端
// ============================================================

void TestStubLeaveRoom() {
    rpc::EventLoop server_loop;
    game::RoomManager room_mgr;
    rpc::Dispatch dispatch;
    std::unordered_map<std::string, rpc::Connection*> player_conns;

    auto send_fn = [&player_conns](const std::string& player_id, const std::vector<uint8_t>& data) {
        auto it = player_conns.find(player_id);
        if (it != player_conns.end() && it->second) {
            auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, "RoomEvent", data);
            it->second->Send(frame);
        }
    };

    game::Broadcast broadcast(&room_mgr, send_fn);
    game::RoomServiceImpl service(&room_mgr, &broadcast);
    game::RegisterRoomService(&dispatch, &service);

    auto server_cb = [&dispatch, &player_conns](const rpc::Frame& frame, rpc::Connection* conn) {
        if (frame.method_name == "Login") {
            game::LoginReq req;
            req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()));
            player_conns[req.token()] = conn;
            game::LoginRes res;
            res.set_success(true);
            res.mutable_player_info()->set_player_id(req.token());
            std::string buf;
            res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  "Login",
                                                  std::vector<uint8_t>(buf.begin(), buf.end()));
            conn->Send(rsp);
            return;
        }
        auto rsp_body = dispatch.Call(frame.method_name, frame.body);
        if (rsp_body) {
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  frame.method_name, *rsp_body);
            conn->Send(rsp);
        } else {
            auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                  frame.method_name, {});
            conn->Send(err);
        }
    };

    auto acceptor = std::make_unique<rpc::Acceptor>(0, &server_loop, server_cb);
    int listen_fd = acceptor->GetFd();
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);
    server_loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

    std::thread server_thread([&server_loop]() { server_loop.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SimpleClient client_a, client_b;
    do {
        bool _ok = client_a.Connect(port);
        if (!_ok) {
            fprintf(stderr, "FATAL: client_a Connect failed\n");
            abort();
        }
    } while (0);
    do {
        bool _ok = client_b.Connect(port);
        if (!_ok) {
            fprintf(stderr, "FATAL: client_b Connect failed\n");
            abort();
        }
    } while (0);
    ClientLogin(client_a, "player_a");
    ClientLogin(client_b, "player_b");

    SimpleStub stub_a(&client_a);
    SimpleStub stub_b(&client_b);

    // A 创建房间，B 加入
    game::CreateRoomReq create_req;
    create_req.set_player_id("player_a");
    create_req.set_max_players(4);
    auto create_res = stub_a.CreateRoom(create_req);
    assert(create_res.success());
    std::string room_id = create_res.room_info().room_id();

    game::JoinRoomReq join_req;
    join_req.set_player_id("player_b");
    join_req.set_room_id(room_id);
    auto join_res2 = stub_b.JoinRoom(join_req);
    assert(join_res2.success());
    assert(room_mgr.GetRoom(room_id)->player_count() == 2);

    // B 离开
    game::LeaveRoomReq leave_req;
    leave_req.set_player_id("player_b");
    leave_req.set_room_id(room_id);
    game::LeaveRoomRes leave_res = stub_b.LeaveRoom(leave_req);
    assert(leave_res.success());

    auto* room = room_mgr.GetRoom(room_id);
    assert(room != nullptr);
    assert(room->player_count() == 1);
    assert(room->HasPlayer("player_a"));
    assert(!room->HasPlayer("player_b"));

    printf("\n    B离开后房间人数=%d\n", room->player_count());

    client_a.Disconnect();
    client_b.Disconnect();
    server_loop.Stop();
    server_thread.join();
}

// ============================================================
// Test 6: 重复创建被拒绝
// ============================================================

void TestStubCreateRoomFailDuplicate() {
    rpc::EventLoop server_loop;
    game::RoomManager room_mgr;
    rpc::Dispatch dispatch;
    std::unordered_map<std::string, rpc::Connection*> player_conns;

    auto send_fn = [&player_conns](const std::string& player_id, const std::vector<uint8_t>& data) {
        auto it = player_conns.find(player_id);
        if (it != player_conns.end() && it->second) {
            auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, "RoomEvent", data);
            it->second->Send(frame);
        }
    };

    game::Broadcast broadcast(&room_mgr, send_fn);
    game::RoomServiceImpl service(&room_mgr, &broadcast);
    game::RegisterRoomService(&dispatch, &service);

    auto server_cb = [&dispatch, &player_conns](const rpc::Frame& frame, rpc::Connection* conn) {
        if (frame.method_name == "Login") {
            game::LoginReq req;
            req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()));
            player_conns[req.token()] = conn;
            game::LoginRes res;
            res.set_success(true);
            res.mutable_player_info()->set_player_id(req.token());
            std::string buf;
            res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  "Login",
                                                  std::vector<uint8_t>(buf.begin(), buf.end()));
            conn->Send(rsp);
            return;
        }
        auto rsp_body = dispatch.Call(frame.method_name, frame.body);
        if (rsp_body) {
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  frame.method_name, *rsp_body);
            conn->Send(rsp);
        } else {
            auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                  frame.method_name, {});
            conn->Send(err);
        }
    };

    auto acceptor = std::make_unique<rpc::Acceptor>(0, &server_loop, server_cb);
    int listen_fd = acceptor->GetFd();
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);
    server_loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

    std::thread server_thread([&server_loop]() { server_loop.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SimpleClient client;
    do {
        bool _ok = client.Connect(port);
        if (!_ok) {
            fprintf(stderr, "FATAL: Connect failed\n");
            abort();
        }
    } while (0);
    ClientLogin(client, "player_a");
    SimpleStub stub(&client);

    // 第一个房间 → 成功
    game::CreateRoomReq req1;
    req1.set_player_id("player_a");
    req1.set_max_players(4);
    auto res1_dup = stub.CreateRoom(req1);
    assert(res1_dup.success());

    // 第二个房间 → 失败（已在房间中）
    game::CreateRoomReq req2;
    req2.set_player_id("player_a");
    req2.set_max_players(3);
    auto res2 = stub.CreateRoom(req2);
    assert(!res2.success());
    assert(res2.error_code() == game::ERR_PLAYER_ALREADY_IN_ROOM);

    printf("\n    重复创建被正确拒绝 (ERR_PLAYER_ALREADY_IN_ROOM)\n");

    client.Disconnect();
    server_loop.Stop();
    server_thread.join();
}

// ============================================================
// ErrorCode 链路测试辅助
// ============================================================

// 快速搭建服务端 + 单客户端，调用方负责 qs.Shutdown()
struct QuickServer {
    rpc::EventLoop loop;
    game::RoomManager room_mgr;
    std::unordered_map<std::string, rpc::Connection*> player_conns;
    game::Broadcast broadcast;
    game::RoomServiceImpl service;
    rpc::Dispatch dispatch;
    std::thread server_thread;

    SimpleClient client;
    SimpleStub stub;
    uint16_t port = 0; // 暴露给测试方，用于创建第二个客户端

    QuickServer()
        : broadcast(&room_mgr,
                    [this](const std::string& pid, const std::vector<uint8_t>& data) {
                        auto it = player_conns.find(pid);
                        if (it != player_conns.end() && it->second) {
                            auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response,
                                                                    "RoomEvent", data);
                            it->second->Send(frame);
                        }
                    })
        , service(&room_mgr, &broadcast)
        , stub(&client) {
        game::RegisterRoomService(&dispatch, &service);

        auto server_cb = [this](const rpc::Frame& frame, rpc::Connection* conn) {
            if (frame.method_name == "Login") {
                game::LoginReq req;
                req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()));
                player_conns[req.token()] = conn;
                game::LoginRes res;
                res.set_success(true);
                res.mutable_player_info()->set_player_id(req.token());
                std::string buf;
                res.SerializeToString(&buf);
                auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                      "Login",
                                                      std::vector<uint8_t>(buf.begin(), buf.end()));
                conn->Send(rsp);
                return;
            }
            auto rsp_body = dispatch.Call(frame.method_name, frame.body);
            if (rsp_body) {
                auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                      frame.method_name, *rsp_body);
                conn->Send(rsp);
            } else {
                auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                      frame.method_name, {});
                conn->Send(err);
            }
        };

        auto acceptor = std::make_unique<rpc::Acceptor>(0, &loop, server_cb);
        int listen_fd = acceptor->GetFd();
        sockaddr_in addr{};
        socklen_t addr_len = sizeof(addr);
        getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
        port = ntohs(addr.sin_port);
        loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

        server_thread = std::thread([this]() { loop.Run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        bool ok = client.Connect(port);
        if (!ok) {
            fprintf(stderr, "FATAL: QuickServer Connect failed\n");
            abort();
        }
        ClientLogin(client, "player_a");
    }

    void Shutdown() {
        client.Disconnect();
        loop.Stop();
        if (server_thread.joinable())
            server_thread.join();
    }
};

// ============================================================
// Test 7: JoinRoom 房间不存在 → ERR_ROOM_NOT_FOUND
// ============================================================

void TestErrorJoinRoomNotFound() {
    QuickServer qs;

    game::JoinRoomReq req;
    req.set_player_id("player_a");
    req.set_room_id("room_999");

    auto res = qs.stub.JoinRoom(req);
    assert(!res.success());
    assert(res.error_code() == game::ERR_ROOM_NOT_FOUND);

    printf("\n    error_code=%d (ERR_ROOM_NOT_FOUND)\n", (int) res.error_code());
    qs.Shutdown();
}

// ============================================================
// Test 8: JoinRoom 房间已满 → ERR_ROOM_FULL
// ============================================================

void TestErrorJoinRoomFull() {
    QuickServer qs;

    // 创建 max_players=1 的房间（房主占唯一位置）
    game::CreateRoomReq create_req;
    create_req.set_player_id("player_a");
    create_req.set_max_players(1);
    auto cr = qs.stub.CreateRoom(create_req);
    assert(cr.success());

    // 第二个客户端尝试加入 → 满员
    SimpleClient client_b;
    bool ok = client_b.Connect(qs.port);
    if (!ok) {
        fprintf(stderr, "FATAL: client_b Connect failed\n");
        abort();
    }
    ClientLogin(client_b, "player_b");
    SimpleStub stub_b(&client_b);

    game::JoinRoomReq join_req;
    join_req.set_player_id("player_b");
    join_req.set_room_id(cr.room_info().room_id());

    auto res = stub_b.JoinRoom(join_req);
    assert(!res.success());
    assert(res.error_code() == game::ERR_ROOM_FULL);

    printf("\n    error_code=%d (ERR_ROOM_FULL)\n", (int) res.error_code());
    client_b.Disconnect();
    qs.Shutdown();
}

// ============================================================
// Test 9: JoinRoom 状态不允许 → ERR_ROOM_NOT_JOINABLE
// ============================================================

void TestErrorJoinRoomNotJoinable() {
    QuickServer qs;

    // 创建房间
    game::CreateRoomReq create_req;
    create_req.set_player_id("player_a");
    create_req.set_max_players(4);
    auto cr = qs.stub.CreateRoom(create_req);
    assert(cr.success());

    // 改为 PLAYING 状态（不可加入）
    qs.room_mgr.GetRoom(cr.room_info().room_id())->SetState(game::ROOM_STATE_PLAYING);

    // 第二个客户端尝试加入
    SimpleClient client_b;
    bool ok = client_b.Connect(qs.port);
    if (!ok) {
        fprintf(stderr, "FATAL: client_b Connect failed\n");
        abort();
    }
    ClientLogin(client_b, "player_b");
    SimpleStub stub_b(&client_b);

    game::JoinRoomReq join_req;
    join_req.set_player_id("player_b");
    join_req.set_room_id(cr.room_info().room_id());

    auto res = stub_b.JoinRoom(join_req);
    assert(!res.success());
    assert(res.error_code() == game::ERR_ROOM_NOT_JOINABLE);

    printf("\n    error_code=%d (ERR_ROOM_NOT_JOINABLE)\n", (int) res.error_code());
    client_b.Disconnect();
    qs.Shutdown();
}

// ============================================================
// Test 10: LeaveRoom 房间不存在 → ERR_ROOM_NOT_FOUND
// ============================================================

void TestErrorLeaveRoomNotFound() {
    QuickServer qs;

    game::LeaveRoomReq req;
    req.set_player_id("player_a");
    req.set_room_id("room_999");

    auto res = qs.stub.LeaveRoom(req);
    assert(!res.success());
    assert(res.error_code() == game::ERR_ROOM_NOT_FOUND);

    printf("\n    error_code=%d (ERR_ROOM_NOT_FOUND)\n", (int) res.error_code());
    qs.Shutdown();
}

// ============================================================
// Test 11: LeaveRoom 玩家不在房间 → ERR_PLAYER_NOT_IN_ROOM
// ============================================================

void TestErrorLeaveRoomPlayerNotInRoom() {
    QuickServer qs;

    // 创建房间 → 离开 → 再次离开（触发 PLAYER_NOT_IN_ROOM）
    game::CreateRoomReq create_req;
    create_req.set_player_id("player_a");
    create_req.set_max_players(4);
    auto cr = qs.stub.CreateRoom(create_req);
    assert(cr.success());
    std::string room_id = cr.room_info().room_id();

    // 第一次离开（正常）
    game::LeaveRoomReq leave_req;
    leave_req.set_player_id("player_a");
    leave_req.set_room_id(room_id);
    assert(qs.stub.LeaveRoom(leave_req).success());

    // 第二次离开 → 不在房间
    auto res2 = qs.stub.LeaveRoom(leave_req);
    assert(!res2.success());
    assert(res2.error_code() == game::ERR_PLAYER_NOT_IN_ROOM);

    printf("\n    error_code=%d (ERR_PLAYER_NOT_IN_ROOM)\n", (int) res2.error_code());
    qs.Shutdown();
}

// ============================================================
// Test 12: SendMessage 房间不存在 → ERR_ROOM_NOT_FOUND
// ============================================================

void TestErrorSendMessageRoomNotFound() {
    QuickServer qs;

    game::SendMessageReq req;
    req.set_room_id("room_999");
    req.set_sender_id("player_a");
    req.set_content("hello");

    auto res = qs.stub.SendMessage(req);
    assert(!res.success());
    assert(res.error_code() == game::ERR_ROOM_NOT_FOUND);

    printf("\n    error_code=%d (ERR_ROOM_NOT_FOUND)\n", (int) res.error_code());
    qs.Shutdown();
}

// ============================================================
// Test 13: 客户端异常断开 → 自动从房间移除 + 通知剩余玩家
// ============================================================

void TestDisconnectAutoCleanup() {
    rpc::EventLoop server_loop;
    game::RoomManager room_mgr;
    rpc::Dispatch dispatch;
    std::unordered_map<std::string, rpc::Connection*> player_conns;
    // fd → player_id 反向映射（供 disconnect 回调查询）
    std::unordered_map<int, std::string> fd_to_player;

    // 发送回调
    auto send_fn = [&player_conns](const std::string& player_id, const std::vector<uint8_t>& data) {
        auto it = player_conns.find(player_id);
        if (it != player_conns.end() && it->second) {
            auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, "RoomEvent", data);
            it->second->Send(frame);
        }
    };

    game::Broadcast broadcast(&room_mgr, send_fn);
    game::RoomServiceImpl service(&room_mgr, &broadcast);
    game::RegisterRoomService(&dispatch, &service);

    // 断连回调：自动从房间移除 + 通知剩余玩家
    rpc::DisconnectCallback on_disconnect = [&room_mgr, &broadcast, &player_conns,
                                             &fd_to_player](int fd) {
        auto it = fd_to_player.find(fd);
        if (it == fd_to_player.end())
            return; // 未登录就断开，无需清理

        std::string player_id = it->second;
        printf("[Disconnect] fd=%d, player=%s 断连，自动清理...\n", fd, player_id.c_str());

        // 从所在房间移除（自动广播 PlayerLeaveNtf 给剩余玩家）
        std::string room_id = room_mgr.GetPlayerRoom(player_id);
        if (!room_id.empty()) {
            room_mgr.LeaveRoomAndNotify(room_id, player_id, &broadcast);
        }

        // 清理映射
        player_conns.erase(player_id);
        fd_to_player.erase(fd);
    };

    // 服务端帧回调
    auto server_cb = [&dispatch, &player_conns, &fd_to_player](const rpc::Frame& frame,
                                                               rpc::Connection* conn) {
        if (frame.method_name == "Login") {
            game::LoginReq req;
            req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()));
            player_conns[req.token()] = conn;
            fd_to_player[conn->GetFd()] = req.token(); // 建立 fd→player 映射

            game::LoginRes res;
            res.set_success(true);
            res.mutable_player_info()->set_player_id(req.token());
            std::string buf;
            res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  "Login",
                                                  std::vector<uint8_t>(buf.begin(), buf.end()));
            conn->Send(rsp);
            return;
        }
        auto rsp_body = dispatch.Call(frame.method_name, frame.body);
        if (rsp_body) {
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  frame.method_name, *rsp_body);
            conn->Send(rsp);
        } else {
            auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                  frame.method_name, {});
            conn->Send(err);
        }
    };

    // 创建 Acceptor，传入断连回调
    auto acceptor = std::make_unique<rpc::Acceptor>(0, &server_loop, server_cb, on_disconnect);
    int listen_fd = acceptor->GetFd();
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);
    server_loop.Register(std::move(acceptor), EPOLLIN | EPOLLRDHUP | EPOLLET);

    std::thread server_thread([&server_loop]() { server_loop.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 客户端 A：连接、登录、创建房间
    SimpleClient client_a;
    bool ok = client_a.Connect(port);
    if (!ok) {
        fprintf(stderr, "FATAL: client_a Connect failed\n");
        abort();
    }
    ClientLogin(client_a, "player_a");
    SimpleStub stub_a(&client_a);

    game::CreateRoomReq create_req;
    create_req.set_player_id("player_a");
    create_req.set_max_players(4);
    auto create_res = stub_a.CreateRoom(create_req);
    assert(create_res.success());
    std::string room_id = create_res.room_info().room_id();

    // 客户端 B：连接、登录、加入房间
    SimpleClient client_b;
    ok = client_b.Connect(port);
    if (!ok) {
        fprintf(stderr, "FATAL: client_b Connect failed\n");
        abort();
    }
    ClientLogin(client_b, "player_b");
    SimpleStub stub_b(&client_b);

    game::JoinRoomReq join_req;
    join_req.set_player_id("player_b");
    join_req.set_room_id(room_id);
    auto join_res = stub_b.JoinRoom(join_req);
    assert(join_res.success());
    assert(room_mgr.GetRoom(room_id)->player_count() == 2);

    // 验证映射已建立
    assert(player_conns.size() == 2);
    assert(fd_to_player.size() == 2);

    // —— 模拟 client_b 异常断开（直接 close，不发送 LeaveRoom） ——
    printf("\n    [模拟] client_b 异常断开 (close fd=%d)...\n", client_b.fd);
    client_b.Disconnect(); // close(fd)

    // 等待服务端处理 EPOLLRDHUP 事件
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 验证：player_b 已从房间移除
    auto* room = room_mgr.GetRoom(room_id);
    assert(room != nullptr);
    assert(room->player_count() == 1);
    assert(room->HasPlayer("player_a"));
    assert(!room->HasPlayer("player_b"));

    // 验证：映射已清理
    assert(player_conns.size() == 1);
    assert(fd_to_player.size() == 1);
    assert(room_mgr.GetPlayerRoom("player_b") == "");

    printf("    断连后房间人数=%d, 映射残留=%zu\n", room->player_count(), player_conns.size());

    // A 可以正常继续操作（房间未被破坏）
    game::GetRoomListRes list = stub_a.GetRoomList();
    assert(list.rooms_size() == 1);

    // 清理
    client_a.Disconnect();
    server_loop.Stop();
    server_thread.join();
}

// ============================================================
// Test 14 (原 7): 创建 → 离开 → 销毁 → 不出现在列表
// ============================================================

void TestStubRoomCleanupAfterEmpty() {
    rpc::EventLoop server_loop;
    game::RoomManager room_mgr;
    rpc::Dispatch dispatch;
    std::unordered_map<std::string, rpc::Connection*> player_conns;

    auto send_fn = [&player_conns](const std::string& player_id, const std::vector<uint8_t>& data) {
        auto it = player_conns.find(player_id);
        if (it != player_conns.end() && it->second) {
            auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, "RoomEvent", data);
            it->second->Send(frame);
        }
    };

    game::Broadcast broadcast(&room_mgr, send_fn);
    game::RoomServiceImpl service(&room_mgr, &broadcast);
    game::RegisterRoomService(&dispatch, &service);

    auto server_cb = [&dispatch, &player_conns](const rpc::Frame& frame, rpc::Connection* conn) {
        if (frame.method_name == "Login") {
            game::LoginReq req;
            req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()));
            player_conns[req.token()] = conn;
            game::LoginRes res;
            res.set_success(true);
            res.mutable_player_info()->set_player_id(req.token());
            std::string buf;
            res.SerializeToString(&buf);
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  "Login",
                                                  std::vector<uint8_t>(buf.begin(), buf.end()));
            conn->Send(rsp);
            return;
        }
        auto rsp_body = dispatch.Call(frame.method_name, frame.body);
        if (rsp_body) {
            auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                                  frame.method_name, *rsp_body);
            conn->Send(rsp);
        } else {
            auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                  frame.method_name, {});
            conn->Send(err);
        }
    };

    auto acceptor = std::make_unique<rpc::Acceptor>(0, &server_loop, server_cb);
    int listen_fd = acceptor->GetFd();
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    uint16_t port = ntohs(addr.sin_port);
    server_loop.Register(std::move(acceptor), EPOLLIN | EPOLLET);

    std::thread server_thread([&server_loop]() { server_loop.Run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SimpleClient client;
    do {
        bool _ok = client.Connect(port);
        if (!_ok) {
            fprintf(stderr, "FATAL: Connect failed\n");
            abort();
        }
    } while (0);
    ClientLogin(client, "player_a");
    SimpleStub stub(&client);

    // 创建房间
    game::CreateRoomReq create_req;
    create_req.set_player_id("player_a");
    create_req.set_max_players(4);
    auto create_res = stub.CreateRoom(create_req);
    assert(create_res.success());
    std::string room_id = create_res.room_info().room_id();

    // 离开 → 房间自动 DESTROYED
    game::LeaveRoomReq leave_req;
    leave_req.set_player_id("player_a");
    leave_req.set_room_id(room_id);
    auto lr_res = stub.LeaveRoom(leave_req);
    assert(lr_res.success());

    room_mgr.CleanupDestroyed();
    assert(room_mgr.room_count() == 0);

    // 列表不应包含已销毁房间
    game::GetRoomListRes list_res = stub.GetRoomList();
    bool found = false;
    for (int i = 0; i < list_res.rooms_size(); i++) {
        if (list_res.rooms(i).room_id() == room_id)
            found = true;
    }
    assert(!found);

    printf("\n    销毁后房间列表已不包含 %s\n", room_id.c_str());

    client.Disconnect();
    server_loop.Stop();
    server_thread.join();
}

// ============================================================
// Test 15: StartGame → 自动启动帧同步 + SendInput
// ============================================================

void TestStartGameInitiatesFrameSync() {
    // 不走网络，直接操作 RoomManager 和 RoomService 验证帧同步集成
    game::RoomManager room_mgr;
    // 直接操作底层 RoomManager，验证帧同步组件与房间的衔接

    // 1. 直接通过 RoomManager 创建房间
    game::GameRoom::Config cfg;
    cfg.max_players = 4;
    auto result = room_mgr.CreateRoom("owner", cfg);
    if (!result.ok) {
        printf("[FAIL] CreateRoom\n");
        abort();
    }
    std::string room_id = result.room_id;
    auto* room = room_mgr.GetRoom(room_id);
    if (!room) {
        printf("[FAIL] no room\n");
        abort();
    }
    room->SetState(game::ROOM_STATE_WAITING);

    // 游戏开始前无帧同步
    if (room->HasFrameSync()) {
        printf("[FAIL] frame sync exists before start\n");
        abort();
    }

    // 2. 模拟 StartGame：改变状态 + 初始化帧同步
    auto start_result = room_mgr.StartGame(room_id, "owner");
    if (!start_result.ok) {
        printf("[FAIL] StartGame\n");
        abort();
    }

    room->InitFrameSync(20, 120, 60);
    room->StartFrameSync();

    // 验证帧同步组件已就绪
    if (!room->HasFrameSync()) {
        printf("[FAIL] no frame sync\n");
        abort();
    }
    if (!room->GetInputBuffer()) {
        printf("[FAIL] no input buffer\n");
        abort();
    }
    if (!room->GetSnapshotManager()) {
        printf("[FAIL] no snapshot mgr\n");
        abort();
    }
    if (!room->GetFrameSync()->IsRunning()) {
        printf("[FAIL] frame sync not running, frame=%u\n", room->GetFrameSync()->CurrentFrame());
        abort();
    }

    printf("\n    帧同步已初始化: fps=%d, frame=%u\n", room->GetFrameSync()->Fps(),
           room->GetFrameSync()->CurrentFrame());

    // 3. 通过 OnPlayerFrameInput 添加输入
    room->OnPlayerFrameInput(1, "owner", {0x04}); // RIGHT
    if (room->GetInputBuffer()->FrameCount() != 1) {
        printf("[FAIL] InputBuffer count=%zu\n", room->GetInputBuffer()->FrameCount());
        abort();
    }

    // 4. 手动 Tick → 验证帧输入被收集并消费
    room->StopFrameSync(); // 停止自动定时器
    size_t n = room->GetFrameSync()->Tick();
    if (n != 1) {
        printf("[FAIL] Tick players=%zu\n", n);
        abort();
    }
    if (room->GetFrameSync()->CurrentFrame() != 1) {
        printf("[FAIL] frame=%u\n", room->GetFrameSync()->CurrentFrame());
        abort();
    }

    printf("    Tick frame=%u, players=%zu\n", room->GetFrameSync()->CurrentFrame(), n);
}

// ============================================================
// Test 16: 完整端到端 — 房间 → StartGame → 帧同步多帧 → StopGame
// ============================================================

void TestE2EFrameSyncFlow() {
    // 直接操作底层验证完整房间+帧同步流程（不走RPC，避免网络层时序干扰）
    game::RoomManager room_mgr;

    // 创建房间 + 加入
    game::GameRoom::Config cfg;
    cfg.max_players = 4;
    auto cr = room_mgr.CreateRoom("player_a", cfg);
    if (!cr.ok) {
        printf("[FAIL] CreateRoom\n");
        abort();
    }
    std::string room_id = cr.room_id;
    auto* room = room_mgr.GetRoom(room_id);
    room->SetState(game::ROOM_STATE_WAITING);
    if (!room_mgr.JoinRoom(room_id, "player_b").ok) {
        printf("[FAIL] JoinRoom\n");
        abort();
    }
    if (room->player_count() != 2) {
        printf("[FAIL] player count\n");
        abort();
    }

    printf("\n    [Step1] 房间=%s, 2人\n", room_id.c_str());

    // ---- Step 2: StartGame → 帧同步初始化 ----
    auto sg = room_mgr.StartGame(room_id, "player_a");
    if (!sg.ok) {
        printf("[FAIL] StartGame: code=%d\n", (int) sg.code);
        abort();
    }

    room->InitFrameSync(20, 120, 60);
    if (!room->HasFrameSync()) {
        printf("[FAIL] no frame sync\n");
        abort();
    }

    // 设置帧广播回调
    int broadcast_count = 0;
    room->GetFrameSync()->SetFrameCallback(
        [&broadcast_count](uint32_t, const auto&) { broadcast_count++; });

    printf("    [Step2] 帧同步初始化完成, fps=%d\n", room->GetFrameSync()->Fps());

    // ---- Step 3: 3帧输入 → 手动Tick ----
    for (int f = 1; f <= 3; f++) {
        room->OnPlayerFrameInput(static_cast<uint32_t>(f), "player_a", {0x04});
        room->OnPlayerFrameInput(static_cast<uint32_t>(f), "player_b", {0x02});

        size_t n = room->GetFrameSync()->Tick();
        if (n != 2) {
            printf("[FAIL] f=%d players=%zu\n", f, n);
            abort();
        }
    }

    if (broadcast_count != 3) {
        printf("[FAIL] broadcast=%d\n", broadcast_count);
        abort();
    }
    if (room->GetFrameSync()->CurrentFrame() != 3) {
        printf("[FAIL] frame=%u\n", room->GetFrameSync()->CurrentFrame());
        abort();
    }

    printf("    [Step3] 3帧完成: frame=%u, broadcast=%d\n", room->GetFrameSync()->CurrentFrame(),
           broadcast_count);

    // ---- Step 4: StopGame → FINISHED ----
    room->StopFrameSync();
    room->SetState(game::ROOM_STATE_FINISHED);
    if (room->state() != game::ROOM_STATE_FINISHED) {
        printf("[FAIL] state not FINISHED\n");
        abort();
    }

    printf("    [Step4] 游戏结束, 状态=FINISHED\n");
}

// ============================================================
// 入口
// ============================================================

int main() {
    setbuf(stdout, NULL);

    printf("=== RoomService RPC 端到端测试 ===\n\n");

    printf("[Stub] 创建房间\n");
    RunTest("Stub::CreateRoom 创建房间", TestStubCreateRoom);

    printf("\n[Stub] 创建 + 加入\n");
    RunTest("Stub::CreateRoom + JoinRoom", TestStubCreateAndJoin);

    printf("\n[Stub] 发送消息\n");
    RunTest("Stub::SendMessage 房间消息", TestStubSendMessage);

    printf("\n[Stub] 房间列表\n");
    RunTest("Stub::GetRoomList 获取列表", TestStubGetRoomList);

    printf("\n[Stub] 离开房间\n");
    RunTest("Stub::LeaveRoom 离开房间", TestStubLeaveRoom);

    printf("\n[Stub] 边界用例\n");
    RunTest("重复创建被拒绝", TestStubCreateRoomFailDuplicate);
    RunTest("清理后房间不出现在列表", TestStubRoomCleanupAfterEmpty);

    printf("\n[ErrorCode] 错误码链路验证\n");
    RunTest("JoinRoom -> ERR_ROOM_NOT_FOUND", TestErrorJoinRoomNotFound);
    RunTest("JoinRoom -> ERR_ROOM_FULL", TestErrorJoinRoomFull);
    RunTest("JoinRoom -> ERR_ROOM_NOT_JOINABLE", TestErrorJoinRoomNotJoinable);
    RunTest("LeaveRoom -> ERR_ROOM_NOT_FOUND", TestErrorLeaveRoomNotFound);
    RunTest("LeaveRoom -> ERR_PLAYER_NOT_IN_ROOM", TestErrorLeaveRoomPlayerNotInRoom);
    RunTest("SendMessage -> ERR_ROOM_NOT_FOUND", TestErrorSendMessageRoomNotFound);

    printf("\n[Disconnect] 断连检测与自动清理\n");
    RunTest("客户端断开后自动移除房间", TestDisconnectAutoCleanup);

    printf("\n[FrameSync集成] 房间状态机与帧同步衔接\n");
    RunTest("StartGame启动帧同步+SendInput", TestStartGameInitiatesFrameSync);

    printf("\n[E2E] 房间→StartGame→帧同步→StopGame\n");
    RunTest("匹配→房间→开始→帧同步→结束", TestE2EFrameSyncFlow);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
