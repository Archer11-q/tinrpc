#pragma once

#include "game/game_room.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace game {

// ============================================================
// RoomManager — 房间管理器
//
// 职责：
// - 创建/查找/销毁房间
// - JoinRoom / LeaveRoom 统一入口（校验 + 委托给 GameRoom）
// - 房间 ID 生成
//
// 不感知网络，只操作数据。
// ============================================================
class RoomManager {
public:
    RoomManager() = default;

    // 禁止拷贝
    RoomManager(const RoomManager&) = delete;
    RoomManager& operator=(const RoomManager&) = delete;

    // 创建房间，房主自动加入，返回 room_id
    std::string CreateRoom(const std::string& player_id,
                           const GameRoom::Config& config);

    // 加入房间。返回 false 表示：房间不存在 / 人数已满 / 已加入 / 状态不允许
    bool JoinRoom(const std::string& room_id, const std::string& player_id);

    // 离开房间。返回 false 表示：房间不存在 / 玩家不在房间内
    // 离开后若房间为空，GameRoom 内部自动标记 DESTROYED
    bool LeaveRoom(const std::string& room_id, const std::string& player_id);

    // 查找房间，不存在返回 nullptr
    GameRoom* GetRoom(const std::string& room_id);

    // 移除房间（从 map 中删除）
    bool RemoveRoom(const std::string& room_id);

    // 房间总数
    size_t room_count() const { return rooms_.size(); }

    // 清理已销毁的房间（遍历 map 移除 state == DESTROYED 的）
    size_t CleanupDestroyed();

private:
    std::string GenerateRoomId();

    std::unordered_map<std::string, std::unique_ptr<GameRoom>> rooms_;
    uint64_t room_id_counter_ = 1;
};

} // namespace game
