#include "game/room_manager.h"
#include "game/broadcast.h"
#include "game.pb.h"

#include <cstdio>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace game {

std::string RoomManager::GenerateRoomId() {
    // 格式：room_001, room_002, ...
    std::ostringstream oss;
    oss << "room_" << std::setfill('0') << std::setw(3) << room_id_counter_++;
    return oss.str();
}

// ============================================================
// player_room_ 映射维护
// ============================================================

void RoomManager::RemovePlayerFromMap(const std::string& player_id) {
    player_room_.erase(player_id);
}

void RoomManager::ClearRoomPlayers(GameRoom* room) {
    for (const auto& pid : room->players()) {
        RemovePlayerFromMap(pid);
    }
}

std::string RoomManager::GetPlayerRoom(const std::string& player_id) const {
    auto it = player_room_.find(player_id);
    return it != player_room_.end() ? it->second : "";
}

// ============================================================
// 房间 CRUD
// ============================================================

Result RoomManager::CreateRoom(const std::string& player_id,
                                const GameRoom::Config& config,
                                int64_t timeout_ms) {
    // 检查玩家是否已在其他房间
    auto it = player_room_.find(player_id);
    if (it != player_room_.end()) {
        return Result::Failure(ERR_PLAYER_ALREADY_IN_ROOM);
    }

    std::string room_id = GenerateRoomId();
    auto room = std::make_unique<GameRoom>(room_id, player_id, config);

    // 注册玩家→房间映射
    player_room_[player_id] = room_id;

    // 注册超时定时器
    if (timeout_ms > 0) {
        GameRoom* raw_ptr = room.get();  // 在 move 之前取裸指针
        rooms_[room_id] = std::move(room);

        raw_ptr->timer().Schedule(timeout_ms, [raw_ptr]() {
            // 游戏中不超时销毁（游戏时长由游戏逻辑控制）
            if (raw_ptr->state() == ROOM_STATE_PLAYING) return;
            raw_ptr->SetState(ROOM_STATE_DESTROYED);
        });
    } else {
        rooms_[room_id] = std::move(room);
    }

    return Result::CreateSuccess(room_id);
}

Result RoomManager::JoinRoom(const std::string& room_id, const std::string& player_id) {
    auto* room = GetRoom(room_id);
    if (!room) {
        return Result::Failure(ERR_ROOM_NOT_FOUND);
    }

    // 检查玩家是否已在其他房间
    auto it = player_room_.find(player_id);
    if (it != player_room_.end() && it->second != room_id) {
        return Result::Failure(ERR_PLAYER_ALREADY_IN_ROOM);
    }

    // 委托给 GameRoom（校验人数/状态/重复）
    Result r = room->AddPlayer(player_id);
    if (!r.ok) {
        return r;  // 透传 GameRoom 的错误码
    }

    // 注册玩家→房间映射
    player_room_[player_id] = room_id;
    return Result::Success();
}

Result RoomManager::LeaveRoom(const std::string& room_id, const std::string& player_id) {
    auto* room = GetRoom(room_id);
    if (!room) {
        return Result::Failure(ERR_ROOM_NOT_FOUND);
    }

    Result r = room->RemovePlayer(player_id);
    if (!r.ok) {
        return r;  // 透传 GameRoom 的错误码
    }

    // 清除玩家→房间映射
    RemovePlayerFromMap(player_id);

    return Result::Success();
}

Result RoomManager::StartGame(const std::string& room_id,
                               const std::string& requester_id) {
    auto* room = GetRoom(room_id);
    if (!room) {
        return Result::Failure(ERR_ROOM_NOT_FOUND);
    }

    // 只有房主可以开始游戏
    if (requester_id != room->owner_id()) {
        return Result::Failure(ERR_NOT_OWNER);
    }

    // 只有在 WAITING 状态下才能开始
    if (room->state() != ROOM_STATE_WAITING) {
        return Result::Failure(ERR_WRONG_ROOM_STATE);
    }

    room->SetState(ROOM_STATE_PLAYING);
    return Result::Success();
}

// ============================================================
// 查询 / 移除 / 清理
// ============================================================

GameRoom* RoomManager::GetRoom(const std::string& room_id) {
    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) {
        return nullptr;
    }
    return it->second.get();
}

bool RoomManager::RemoveRoom(const std::string& room_id) {
    auto* room = GetRoom(room_id);
    if (room) {
        ClearRoomPlayers(room);
    }
    return rooms_.erase(room_id) > 0;
}

size_t RoomManager::CleanupDestroyed() {
    size_t removed = 0;
    for (auto it = rooms_.begin(); it != rooms_.end(); ) {
        if (it->second->state() == ROOM_STATE_DESTROYED) {
            ClearRoomPlayers(it->second.get());
            it = rooms_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    return removed;
}

size_t RoomManager::CheckRoomTimeout() {
    // 驱动所有房间的定时器
    for (auto& [id, room] : rooms_) {
        room->timer().Tick();
    }
    // 清理已标记销毁的房间
    return CleanupDestroyed();
}

// ============================================================
// 带通知的操作（成功后自动广播事件）
// ============================================================

Result RoomManager::JoinRoomAndNotify(const std::string& room_id,
                                       const std::string& player_id,
                                       Broadcast* broadcast) {
    Result r = JoinRoom(room_id, player_id);
    if (!r.ok) {
        return r;
    }

    // 加入成功 → 向房间内其他人广播 PlayerJoinNtf
    if (broadcast) {
        auto* room = GetRoom(room_id);

        PlayerJoinNtf ntf;
        ntf.set_room_id(room_id);
        ntf.set_player_id(player_id);
        ntf.set_player_count(room->player_count());

        std::string buf;
        ntf.SerializeToString(&buf);
        std::vector<uint8_t> data(buf.begin(), buf.end());

        // 排除加入者自身
        broadcast->BroadcastToRoomExcept(room_id, player_id, data);
    }

    return Result::Success();
}

Result RoomManager::LeaveRoomAndNotify(const std::string& room_id,
                                        const std::string& player_id,
                                        Broadcast* broadcast) {
    auto* room = GetRoom(room_id);
    if (!room) {
        return Result::Failure(ERR_ROOM_NOT_FOUND);
    }

    Result r = LeaveRoom(room_id, player_id);
    if (!r.ok) {
        return r;
    }

    // 离开成功 → 向剩余玩家广播 PlayerLeaveNtf
    if (broadcast) {
        PlayerLeaveNtf ntf;
        ntf.set_room_id(room_id);
        ntf.set_player_id(player_id);
        ntf.set_player_count(room->player_count());

        std::string buf;
        ntf.SerializeToString(&buf);
        std::vector<uint8_t> data(buf.begin(), buf.end());

        // 房间未销毁 → 广播给剩余玩家；已销毁 → 无剩余玩家，不广播
        if (room->state() != ROOM_STATE_DESTROYED) {
            broadcast->BroadcastToRoom(room_id, data);
        }
    }

    return Result::Success();
}

Result RoomManager::StartGameAndNotify(const std::string& room_id,
                                        const std::string& requester_id,
                                        Broadcast* broadcast) {
    Result r = StartGame(room_id, requester_id);
    if (!r.ok) {
        return r;
    }

    // 开始成功 → 向所有人广播 GameStartNtf
    if (broadcast) {
        GameStartNtf ntf;
        ntf.set_room_id(room_id);
        ntf.set_timestamp(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );

        std::string buf;
        ntf.SerializeToString(&buf);
        std::vector<uint8_t> data(buf.begin(), buf.end());

        // 通知所有人（包括发起者）
        broadcast->BroadcastToRoom(room_id, data);
    }

    return Result::Success();
}

} // namespace game
