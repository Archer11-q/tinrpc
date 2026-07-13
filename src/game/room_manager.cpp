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

std::string RoomManager::CreateRoom(const std::string& player_id,
                                     const GameRoom::Config& config) {
    std::string room_id = GenerateRoomId();
    auto room = std::make_unique<GameRoom>(room_id, player_id, config);
    rooms_[room_id] = std::move(room);
    return room_id;
}

bool RoomManager::JoinRoom(const std::string& room_id, const std::string& player_id) {
    auto* room = GetRoom(room_id);
    if (!room) {
        return false;  // 房间不存在
    }
    return room->AddPlayer(player_id);
}

bool RoomManager::LeaveRoom(const std::string& room_id, const std::string& player_id) {
    auto* room = GetRoom(room_id);
    if (!room) {
        return false;  // 房间不存在
    }
    return room->RemovePlayer(player_id);
}

GameRoom* RoomManager::GetRoom(const std::string& room_id) {
    auto it = rooms_.find(room_id);
    if (it == rooms_.end()) {
        return nullptr;
    }
    return it->second.get();
}

bool RoomManager::RemoveRoom(const std::string& room_id) {
    return rooms_.erase(room_id) > 0;
}

size_t RoomManager::CleanupDestroyed() {
    size_t removed = 0;
    for (auto it = rooms_.begin(); it != rooms_.end(); ) {
        if (it->second->state() == ROOM_STATE_DESTROYED) {
            it = rooms_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    return removed;
}

bool RoomManager::StartGame(const std::string& room_id,
                              const std::string& requester_id) {
    auto* room = GetRoom(room_id);
    if (!room) {
        return false;  // 房间不存在
    }

    // 只有房主可以开始游戏
    if (requester_id != room->owner_id()) {
        return false;
    }

    // 只有在 WAITING 状态下才能开始
    if (room->state() != ROOM_STATE_WAITING) {
        return false;
    }

    room->SetState(ROOM_STATE_PLAYING);
    return true;
}

// ============================================================
// 带通知的操作（成功后自动广播事件）
// ============================================================

bool RoomManager::JoinRoomAndNotify(const std::string& room_id,
                                     const std::string& player_id,
                                     Broadcast* broadcast) {
    if (!JoinRoom(room_id, player_id)) {
        return false;
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

        // 排除加入者自身（他自己知道加入了）
        broadcast->BroadcastToRoomExcept(room_id, player_id, data);
    }

    return true;
}

bool RoomManager::LeaveRoomAndNotify(const std::string& room_id,
                                      const std::string& player_id,
                                      Broadcast* broadcast) {
    // 先记录离开前的房间状态（用于判断房间是否已销毁）
    auto* room = GetRoom(room_id);
    if (!room) {
        return false;
    }

    if (!LeaveRoom(room_id, player_id)) {
        return false;
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

        // 房间未销毁 → 广播给剩余玩家
        if (room->state() != ROOM_STATE_DESTROYED) {
            broadcast->BroadcastToRoom(room_id, data);
        }
        // 房间已销毁 → 没有剩余玩家，不需要广播
    }

    return true;
}

bool RoomManager::StartGameAndNotify(const std::string& room_id,
                                      const std::string& requester_id,
                                      Broadcast* broadcast) {
    if (!StartGame(room_id, requester_id)) {
        return false;
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

    return true;
}

} // namespace game
