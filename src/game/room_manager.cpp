#include "game/room_manager.h"

#include <cstdio>
#include <sstream>
#include <iomanip>

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

} // namespace game
