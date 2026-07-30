#pragma once

#include "game/game_room.h"
#include "game/room_manager.h"

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace game {

/**
 * @brief Broadcast — 房间内广播
 *
 * 职责：
 * - 持有 RoomManager 引用，通过 room_id 查找房间
 * - 遍历房间内所有玩家，调用外部注入的发送回调
 *
 * 不持有 Connection，发送逻辑由外部回调完成。
 */

/// @brief 发送回调：接收 player_id 和序列化后的消息，负责实际发送
using SendToPlayer =
    std::function<void(const std::string& player_id, const std::vector<uint8_t>& data)>;

class Broadcast {
public:
    /** @brief 构造函数
     *  @param room_mgr RoomManager 指针（不持有所有权）
     *  @param send_fn  发送回调（由 GameService 提供，负责 player_id → Connection 查表并发包）
     */
    Broadcast(RoomManager* room_mgr, SendToPlayer send_fn);

    // 禁止拷贝
    Broadcast(const Broadcast&) = delete;
    Broadcast& operator=(const Broadcast&) = delete;

    /** @brief 向房间内所有玩家发送消息
     *  @param room_id 房间 ID
     *  @param data    序列化后的消息
     *  @return 实际发送人数（房间内玩家数），房间不存在返回 0
     */
    size_t BroadcastToRoom(const std::string& room_id, const std::vector<uint8_t>& data);

    /** @brief 向房间内除某玩家外的所有人发送消息
     *  @param room_id           房间 ID
     *  @param exclude_player_id 不发送的目标玩家 ID
     *  @param data              序列化后的消息
     *  @return 实际发送人数
     */
    size_t BroadcastToRoomExcept(const std::string& room_id, const std::string& exclude_player_id,
                                 const std::vector<uint8_t>& data);

private:
    RoomManager* room_mgr_; ///< 不持有所有权
    SendToPlayer send_fn_;
};

} // namespace game
