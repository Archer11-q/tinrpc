#include "game/broadcast.h"

namespace game {

Broadcast::Broadcast(RoomManager* room_mgr, SendToPlayer send_fn)
    : room_mgr_(room_mgr), send_fn_(std::move(send_fn)) {
}

size_t Broadcast::BroadcastToRoom(const std::string& room_id, const std::vector<uint8_t>& data) {
    GameRoom* room = room_mgr_->GetRoom(room_id);
    if (!room) {
        return 0; // 房间不存在
    }

    size_t sent = 0;
    for (const auto& player_id : room->players()) {
        send_fn_(player_id, data);
        sent++;
    }
    return sent;
}

size_t Broadcast::BroadcastToRoomExcept(const std::string& room_id,
                                        const std::string& exclude_player_id,
                                        const std::vector<uint8_t>& data) {
    GameRoom* room = room_mgr_->GetRoom(room_id);
    if (!room) {
        return 0; // 房间不存在
    }

    size_t sent = 0;
    for (const auto& player_id : room->players()) {
        if (player_id == exclude_player_id) {
            continue;
        }
        send_fn_(player_id, data);
        sent++;
    }
    return sent;
}

} // namespace game
