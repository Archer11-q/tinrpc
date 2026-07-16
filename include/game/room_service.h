#pragma once

#include "game/game_room.h"
#include "game/room_manager.h"
#include "game/broadcast.h"
#include "rpc/dispatch.h"
#include "rpc/rpc_client.h"
#include "game.pb.h"

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <functional>

namespace game {

// ============================================================
// RoomService — 房间服务纯虚基类（RPC Service 接口）
//
// 每个方法签名与 rpc::Dispatch::Handler 对齐：
//   接收 Protobuf 序列化后的请求 body → 返回 Protobuf 序列化后的响应 body
//   返回 nullopt 表示参数解析失败
//
// 这种设计允许 RoomService 的方法直接注册到 Dispatch：
//   dispatch.RegisterMethod("CreateRoom", [&svc](auto& b) { return svc.CreateRoom(b); });
// ============================================================
class RoomService {
public:
    virtual ~RoomService() = default;

    // 创建房间
    virtual std::optional<std::vector<uint8_t>> CreateRoom(const std::vector<uint8_t>& body) = 0;

    // 加入房间
    virtual std::optional<std::vector<uint8_t>> JoinRoom(const std::vector<uint8_t>& body) = 0;

    // 离开房间
    virtual std::optional<std::vector<uint8_t>> LeaveRoom(const std::vector<uint8_t>& body) = 0;

    // 发送房间消息（聊天等），消息广播给房间内所有玩家
    virtual std::optional<std::vector<uint8_t>> SendMessage(const std::vector<uint8_t>& body) = 0;

    // 获取房间列表
    virtual std::optional<std::vector<uint8_t>> GetRoomList(const std::vector<uint8_t>& body) = 0;

    // 开始游戏（仅房主可调用）
    virtual std::optional<std::vector<uint8_t>> StartGame(const std::vector<uint8_t>& body) = 0;
};

// ============================================================
// RoomServiceImpl — RoomService 的具体实现
//
// 持有 RoomManager 和 Broadcast 指针（不持有所有权），
// 将 Protobuf 请求解析后委托给 RoomManager / Broadcast 处理。
//
// 线程模型：所有方法必须在 EventLoop IO 线程调用。
// ============================================================
class RoomServiceImpl : public RoomService {
public:
    RoomServiceImpl(RoomManager* room_mgr, Broadcast* broadcast);

    std::optional<std::vector<uint8_t>> CreateRoom(const std::vector<uint8_t>& body) override;
    std::optional<std::vector<uint8_t>> JoinRoom(const std::vector<uint8_t>& body) override;
    std::optional<std::vector<uint8_t>> LeaveRoom(const std::vector<uint8_t>& body) override;
    std::optional<std::vector<uint8_t>> SendMessage(const std::vector<uint8_t>& body) override;
    std::optional<std::vector<uint8_t>> GetRoomList(const std::vector<uint8_t>& body) override;
    std::optional<std::vector<uint8_t>> StartGame(const std::vector<uint8_t>& body) override;

private:
    RoomManager* room_mgr_;    // 不持有所有权
    Broadcast*   broadcast_;   // 不持有所有权
};

// ============================================================
// RoomServiceStub — 客户端代理（Stub 模式）
//
// 封装 RpcClient，将 Protobuf 请求消息序列化后通过 RPC 发送，
// 再将响应反序列化为 Protobuf 消息返回。
//
// 调用方通过此 Stub 像调用本地方法一样调用远程 RoomService。
// ============================================================
class RoomServiceStub {
public:
    explicit RoomServiceStub(rpc::RpcClient* client);

    // 创建房间 → 返回 CreateRoomRes
    CreateRoomRes CreateRoom(const CreateRoomReq& req);

    // 加入房间 → 返回 JoinRoomRes
    JoinRoomRes JoinRoom(const JoinRoomReq& req);

    // 离开房间 → 返回 LeaveRoomRes
    LeaveRoomRes LeaveRoom(const LeaveRoomReq& req);

    // 发送房间消息 → 返回 SendMessageRes
    SendMessageRes SendMessage(const SendMessageReq& req);

    // 获取房间列表 → 返回 GetRoomListRes
    GetRoomListRes GetRoomList();

    // 开始游戏 → 返回 StartGameRes
    StartGameRes StartGame(const StartGameReq& req);

private:
    // 发起 RPC 调用并等待响应，解析为指定 Protobuf 类型
    template<typename ResProto>
    ResProto DoCall(const std::string& method_name,
                    const std::vector<uint8_t>& req_body);

    rpc::RpcClient* client_;  // 不持有所有权
};

// ============================================================
// RegisterRoomService — 将 RoomService 的 6 个方法注册到 Dispatch
//
// 使用方式：
//   RoomServiceImpl svc(&room_mgr, &broadcast);
//   RegisterRoomService(&dispatch, &svc);
// ============================================================
inline void RegisterRoomService(rpc::Dispatch* dispatch, RoomService* service) {
    dispatch->RegisterMethod("CreateRoom",  [service](const std::vector<uint8_t>& body) { return service->CreateRoom(body); });
    dispatch->RegisterMethod("JoinRoom",    [service](const std::vector<uint8_t>& body) { return service->JoinRoom(body); });
    dispatch->RegisterMethod("LeaveRoom",   [service](const std::vector<uint8_t>& body) { return service->LeaveRoom(body); });
    dispatch->RegisterMethod("SendMessage", [service](const std::vector<uint8_t>& body) { return service->SendMessage(body); });
    dispatch->RegisterMethod("GetRoomList", [service](const std::vector<uint8_t>& body) { return service->GetRoomList(body); });
    dispatch->RegisterMethod("StartGame",   [service](const std::vector<uint8_t>& body) { return service->StartGame(body); });
}

// ============================================================
// RoomServiceStub 内联实现（模板 + 简单转发，放在头文件中）
// ============================================================

inline RoomServiceStub::RoomServiceStub(rpc::RpcClient* client)
    : client_(client) {
}

template<typename ResProto>
ResProto RoomServiceStub::DoCall(const std::string& method_name,
                                  const std::vector<uint8_t>& req_body) {
    auto future = client_->Call(method_name, req_body);
    auto rsp_body = future.get();

    ResProto res;
    if (!rsp_body.empty()) {
        res.ParseFromArray(rsp_body.data(), static_cast<int>(rsp_body.size()));
    }
    return res;
}

inline CreateRoomRes RoomServiceStub::CreateRoom(const CreateRoomReq& req) {
    std::string buf;
    req.SerializeToString(&buf);
    return DoCall<CreateRoomRes>("CreateRoom", std::vector<uint8_t>(buf.begin(), buf.end()));
}

inline JoinRoomRes RoomServiceStub::JoinRoom(const JoinRoomReq& req) {
    std::string buf;
    req.SerializeToString(&buf);
    return DoCall<JoinRoomRes>("JoinRoom", std::vector<uint8_t>(buf.begin(), buf.end()));
}

inline LeaveRoomRes RoomServiceStub::LeaveRoom(const LeaveRoomReq& req) {
    std::string buf;
    req.SerializeToString(&buf);
    return DoCall<LeaveRoomRes>("LeaveRoom", std::vector<uint8_t>(buf.begin(), buf.end()));
}

inline SendMessageRes RoomServiceStub::SendMessage(const SendMessageReq& req) {
    std::string buf;
    req.SerializeToString(&buf);
    return DoCall<SendMessageRes>("SendMessage", std::vector<uint8_t>(buf.begin(), buf.end()));
}

inline GetRoomListRes RoomServiceStub::GetRoomList() {
    // GetRoomListReq 无字段，发送空 body
    return DoCall<GetRoomListRes>("GetRoomList", {});
}

inline StartGameRes RoomServiceStub::StartGame(const StartGameReq& req) {
    std::string buf;
    req.SerializeToString(&buf);
    return DoCall<StartGameRes>("StartGame", std::vector<uint8_t>(buf.begin(), buf.end()));
}

} // namespace game
