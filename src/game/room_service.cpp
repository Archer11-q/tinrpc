#include "game/room_service.h"
#include "game.pb.h"

#include <chrono>

namespace game {

// ============================================================
// RoomServiceImpl
// ============================================================

RoomServiceImpl::RoomServiceImpl(RoomManager* room_mgr, Broadcast* broadcast)
    : room_mgr_(room_mgr)
    , broadcast_(broadcast) {
}

// ---- CreateRoom ----

std::optional<std::vector<uint8_t>> RoomServiceImpl::CreateRoom(const std::vector<uint8_t>& body) {
    // 1. 解析请求
    CreateRoomReq req;
    if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
        // Protobuf 解析失败 → 返回 nullopt → 上层发送 Error 帧
        return std::nullopt;
    }

    // 2. 构造房间配置
    GameRoom::Config cfg;
    cfg.max_players = req.max_players() > 0 ? req.max_players() : 4;

    // 3. 委托 RoomManager 创建房间
    auto result = room_mgr_->CreateRoom(req.player_id(), cfg);

    // 4. 创建成功后设置为 WAITING 状态（开放加入）
    if (result.ok) {
        auto* room = room_mgr_->GetRoom(result.room_id);
        if (room) {
            room->SetState(ROOM_STATE_WAITING);
        }
    }

    // 5. 构造响应
    CreateRoomRes res;
    res.set_success(result.ok);
    if (result.ok) {
        auto* room = room_mgr_->GetRoom(result.room_id);
        if (room) {
            *res.mutable_room_info() = room->ToProto();
        }
    } else {
        res.set_error_code(result.code);
        res.set_error_msg("创建房间失败");
    }

    // 6. 序列化响应
    std::string buf;
    res.SerializeToString(&buf);
    return std::vector<uint8_t>(buf.begin(), buf.end());
}

// ---- JoinRoom ----

std::optional<std::vector<uint8_t>> RoomServiceImpl::JoinRoom(const std::vector<uint8_t>& body) {
    // 1. 解析请求
    JoinRoomReq req;
    if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
        // Protobuf 解析失败 → 返回 nullopt → 上层发送 Error 帧
        return std::nullopt;
    }

    // 2. 委托 RoomManager（使用带通知的版本，自动广播 PlayerJoinNtf）
    Result result;
    if (broadcast_) {
        result = room_mgr_->JoinRoomAndNotify(req.room_id(), req.player_id(), broadcast_);
    } else {
        result = room_mgr_->JoinRoom(req.room_id(), req.player_id());
    }

    // 3. 构造响应
    JoinRoomRes res;
    res.set_success(result.ok);
    if (result.ok) {
        auto* room = room_mgr_->GetRoom(req.room_id());
        if (room) {
            *res.mutable_room_info() = room->ToProto();
        }
    } else {
        res.set_error_code(result.code);
        // 根据错误码设置错误信息
        switch (result.code) {
            case ERR_ROOM_NOT_FOUND:         res.set_error_msg("房间不存在"); break;
            case ERR_ROOM_FULL:              res.set_error_msg("房间已满"); break;
            case ERR_ROOM_NOT_JOINABLE:      res.set_error_msg("房间状态不允许加入"); break;
            case ERR_PLAYER_ALREADY_IN_ROOM: res.set_error_msg("玩家已在房间中"); break;
            default:                          res.set_error_msg("加入失败"); break;
        }
    }

    // 4. 序列化响应
    std::string buf;
    res.SerializeToString(&buf);
    return std::vector<uint8_t>(buf.begin(), buf.end());
}

// ---- LeaveRoom ----

std::optional<std::vector<uint8_t>> RoomServiceImpl::LeaveRoom(const std::vector<uint8_t>& body) {
    // 1. 解析请求
    LeaveRoomReq req;
    if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
        // Protobuf 解析失败 → 返回 nullopt → 上层发送 Error 帧
        return std::nullopt;
    }

    // 2. 委托 RoomManager（使用带通知的版本，自动广播 PlayerLeaveNtf）
    Result result;
    if (broadcast_) {
        result = room_mgr_->LeaveRoomAndNotify(req.room_id(), req.player_id(), broadcast_);
    } else {
        result = room_mgr_->LeaveRoom(req.room_id(), req.player_id());
    }

    // 3. 构造响应
    LeaveRoomRes res;
    res.set_success(result.ok);
    if (!result.ok) {
        res.set_error_code(result.code);
        switch (result.code) {
            case ERR_ROOM_NOT_FOUND:     res.set_error_msg("房间不存在"); break;
            case ERR_PLAYER_NOT_IN_ROOM: res.set_error_msg("玩家不在房间中"); break;
            default:                      res.set_error_msg("离开失败"); break;
        }
    }

    // 4. 序列化响应
    std::string buf;
    res.SerializeToString(&buf);
    return std::vector<uint8_t>(buf.begin(), buf.end());
}

// ---- SendMessage ----

std::optional<std::vector<uint8_t>> RoomServiceImpl::SendMessage(const std::vector<uint8_t>& body) {
    // 1. 解析请求
    SendMessageReq req;
    if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
        // Protobuf 解析失败 → 返回 nullopt → 上层发送 Error 帧
        return std::nullopt;
    }

    // 2. 检查房间是否存在
    auto* room = room_mgr_->GetRoom(req.room_id());
    if (!room) {
        SendMessageRes res;
        res.set_success(false);
        res.set_error_code(ERR_ROOM_NOT_FOUND);
        res.set_error_msg("房间不存在");
        std::string buf;
        res.SerializeToString(&buf);
        return std::vector<uint8_t>(buf.begin(), buf.end());
    }

    // 3. 构造 RoomBroadcastMsg 并广播
    if (broadcast_) {
        RoomBroadcastMsg msg;
        msg.set_room_id(req.room_id());
        msg.set_sender_id(req.sender_id());
        msg.set_content(req.content());
        msg.set_timestamp(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );

        std::string mbuf;
        msg.SerializeToString(&mbuf);
        std::vector<uint8_t> data(mbuf.begin(), mbuf.end());

        // 广播给房间内其他玩家（排除发送者自身）
        // 发送者通过 SendMessageRes 响应获知发送成功
        broadcast_->BroadcastToRoomExcept(req.room_id(), req.sender_id(), data);
    }

    // 4. 构造响应
    SendMessageRes res;
    res.set_success(true);

    std::string buf;
    res.SerializeToString(&buf);
    return std::vector<uint8_t>(buf.begin(), buf.end());
}

// ---- GetRoomList ----

std::optional<std::vector<uint8_t>> RoomServiceImpl::GetRoomList(const std::vector<uint8_t>& body) {
    // 1. 解析请求（GetRoomListReq 暂无字段，但保留未来扩展性）
    GetRoomListReq req;
    if (!body.empty()) {
        if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
            // Protobuf 解析失败 → 返回 nullopt → 上层发送 Error 帧
            return std::nullopt;
        }
    }

    // 2. 遍历所有房间
    GetRoomListRes res;
    for (const auto& room_id : room_mgr_->GetAllRoomIds()) {
        auto* room = room_mgr_->GetRoom(room_id);
        if (room && room->state() != ROOM_STATE_DESTROYED) {
            *res.add_rooms() = room->ToProto();
        }
    }

    // 3. 序列化响应
    std::string buf;
    res.SerializeToString(&buf);
    return std::vector<uint8_t>(buf.begin(), buf.end());
}

// ---- StartGame ----

std::optional<std::vector<uint8_t>> RoomServiceImpl::StartGame(const std::vector<uint8_t>& body) {
    // 1. 解析请求
    StartGameReq req;
    if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
        // Protobuf 解析失败 → 返回 nullopt → 上层发送 Error 帧
        return std::nullopt;
    }

    // 2. 委托 RoomManager（使用带通知的版本，自动广播 GameStartNtf）
    Result result;
    if (broadcast_) {
        result = room_mgr_->StartGameAndNotify(req.room_id(), req.player_id(), broadcast_);
    } else {
        result = room_mgr_->StartGame(req.room_id(), req.player_id());
    }

    // 3. 构造响应
    StartGameRes res;
    res.set_success(result.ok);
    if (!result.ok) {
        res.set_error_code(result.code);
        switch (result.code) {
            case ERR_ROOM_NOT_FOUND:   res.set_error_msg("房间不存在"); break;
            case ERR_NOT_OWNER:        res.set_error_msg("不是房主，无权开始游戏"); break;
            case ERR_WRONG_ROOM_STATE: res.set_error_msg("房间状态不允许开始游戏"); break;
            default:                    res.set_error_msg("开始游戏失败"); break;
        }
    } else {
        // 游戏开始成功 → 初始化并启动帧同步
        auto* room = room_mgr_->GetRoom(req.room_id());
        if (room) {
            if (!room->HasFrameSync()) {
                room->InitFrameSync(20);
                printf("[RoomService] 帧同步已初始化: room=%s, fps=20\n",
                       req.room_id().c_str());
            }
            room->StartFrameSync();
            printf("[RoomService] 帧同步已启动: room=%s\n", req.room_id().c_str());
        }
    }

    // 4. 序列化响应
    std::string buf;
    res.SerializeToString(&buf);
    return std::vector<uint8_t>(buf.begin(), buf.end());
}

// ---- SendInput ----

std::optional<std::vector<uint8_t>> RoomServiceImpl::SendInput(const std::vector<uint8_t>& body) {
    // 1. 解析请求
    PlayerInputReq req;
    if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
        return std::nullopt;
    }

    // 2. 查找玩家所在房间
    std::string room_id = room_mgr_->GetPlayerRoom(req.player_id());
    if (room_id.empty()) {
        SendInputRes res;
        res.set_success(false);
        res.set_error_code(ERR_PLAYER_NOT_IN_ROOM);
        res.set_error_msg("玩家不在任何房间中");
        std::string buf;
        res.SerializeToString(&buf);
        return std::vector<uint8_t>(buf.begin(), buf.end());
    }

    auto* room = room_mgr_->GetRoom(room_id);
    if (!room) {
        SendInputRes res;
        res.set_success(false);
        res.set_error_code(ERR_ROOM_NOT_FOUND);
        res.set_error_msg("房间不存在");
        std::string buf;
        res.SerializeToString(&buf);
        return std::vector<uint8_t>(buf.begin(), buf.end());
    }

    // 3. 将输入存入帧同步 InputBuffer
    std::vector<uint8_t> input(req.input_data().begin(), req.input_data().end());
    room->OnPlayerFrameInput(req.frame_no(), req.player_id(), input);

    // 4. 构造响应
    SendInputRes res;
    res.set_success(true);

    std::string buf;
    res.SerializeToString(&buf);
    return std::vector<uint8_t>(buf.begin(), buf.end());
}

} // namespace game
