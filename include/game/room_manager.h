#pragma once

#include "game/game_room.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace game {

class Broadcast;  // 前向声明，避免循环依赖

// ============================================================
// RoomManager — 房间管理器
//
// 职责：
// - 创建/查找/销毁房间
// - JoinRoom / LeaveRoom 统一入口（校验 + 委托给 GameRoom）
// - 房间 ID 生成
// - JoinRoomAndNotify / LeaveRoomAndNotify / StartGameAndNotify
//   操作成功后自动广播房间事件通知
// - 超时管理：CreateRoom 自动注册超时定时器，CheckRoomTimeout 轮询到期
//
// 不感知网络，只操作数据。
// ============================================================
class RoomManager {
public:
    // 默认房间超时 5 分钟（毫秒）
    static constexpr int64_t kDefaultRoomTimeoutMs = 5 * 60 * 1000;

    RoomManager() = default;

    // 禁止拷贝
    RoomManager(const RoomManager&) = delete;
    RoomManager& operator=(const RoomManager&) = delete;

    // 创建房间，房主自动加入，返回 room_id
    // timeout_ms: 房间超时时间（毫秒），到期后若状态非 PLAYING 则自动标记 DESTROYED
    //             设为 0 表示不注册超时
    std::string CreateRoom(const std::string& player_id,
                           const GameRoom::Config& config,
                           int64_t timeout_ms = kDefaultRoomTimeoutMs);

    // ---- 基础操作（不触发广播） ----

    // 加入房间。返回 false 表示：房间不存在 / 人数已满 / 已加入 / 状态不允许
    bool JoinRoom(const std::string& room_id, const std::string& player_id);

    // 离开房间。返回 false 表示：房间不存在 / 玩家不在房间内
    // 离开后若房间为空，GameRoom 内部自动标记 DESTROYED
    bool LeaveRoom(const std::string& room_id, const std::string& player_id);

    // 开始游戏。返回 false 表示：房间不存在 / 不是房主 / 状态不允许
    bool StartGame(const std::string& room_id, const std::string& requester_id);

    // ---- 带通知的操作（成功后自动广播事件） ----

    // 加入房间 + 自动广播 PlayerJoinNtf（排除加入者自身）
    bool JoinRoomAndNotify(const std::string& room_id,
                           const std::string& player_id,
                           Broadcast* broadcast);

    // 离开房间 + 自动广播 PlayerLeaveNtf 给剩余玩家
    bool LeaveRoomAndNotify(const std::string& room_id,
                            const std::string& player_id,
                            Broadcast* broadcast);

    // 开始游戏 + 自动广播 GameStartNtf 给所有人
    bool StartGameAndNotify(const std::string& room_id,
                            const std::string& requester_id,
                            Broadcast* broadcast);

    // 查找房间，不存在返回 nullptr
    GameRoom* GetRoom(const std::string& room_id);

    // 移除房间（从 map 中删除）
    bool RemoveRoom(const std::string& room_id);

    // 房间总数
    size_t room_count() const { return rooms_.size(); }

    // 清理已销毁的房间（遍历 map 移除 state == DESTROYED 的）
    size_t CleanupDestroyed();

    // 轮询所有房间的定时器，触发到期回调，清理已销毁房间
    // 返回本次销毁的房间数
    size_t CheckRoomTimeout();

private:
    std::string GenerateRoomId();

    std::unordered_map<std::string, std::unique_ptr<GameRoom>> rooms_;
    uint64_t room_id_counter_ = 1;
};

} // namespace game
