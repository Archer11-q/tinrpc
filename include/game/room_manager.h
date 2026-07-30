#pragma once

#include "game/game_room.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace game {

class Broadcast; // 前向声明，避免循环依赖

/**
 * @brief 房间管理器
 *
 * 职责：
 * - 创建/查找/销毁房间
 * - JoinRoom / LeaveRoom / StartGame 统一入口（校验 + 委托给 GameRoom）
 * - 房间 ID 生成 + 玩家→房间映射（player_room_）
 * - JoinRoomAndNotify / LeaveRoomAndNotify / StartGameAndNotify
 *   操作成功后自动广播房间事件通知
 * - 超时管理：CreateRoom 自动注册超时定时器，CheckRoomTimeout 轮询到期
 *
 * 线程模型：所有方法必须在 EventLoop IO 线程调用，由调用方保证。
 * 当前阶段无锁——所有状态变更收敛在同一个 IO 线程内。
 *
 * 不感知网络，只操作数据。
 */
class RoomManager {
public:
    /// 默认房间超时 5 分钟（毫秒）
    static constexpr int64_t kDefaultRoomTimeoutMs = 5 * 60 * 1000;

    RoomManager() = default;

    // 禁止拷贝
    RoomManager(const RoomManager&) = delete;
    RoomManager& operator=(const RoomManager&) = delete;

    /**
     * @brief 创建房间，房主自动加入
     * @param player_id 房主 ID
     * @param config 房间配置（最大人数等）
     * @param timeout_ms 房间超时时间（毫秒），到期后若状态非 PLAYING 则自动标记 DESTROYED，设为 0 不注册超时
     * @return 成功: result.ok = true, result.room_id 为新房号；
     *         失败: ERR_PLAYER_ALREADY_IN_ROOM（房主已在其他房间）
     */
    Result CreateRoom(const std::string& player_id, const GameRoom::Config& config,
                      int64_t timeout_ms = kDefaultRoomTimeoutMs);

    // ---- 基础操作（不触发广播） ----

    /**
     * @brief 加入房间
     * @param room_id 房间 ID
     * @param player_id 玩家 ID
     * @return 失败时 .code 对应 ERR_ROOM_NOT_FOUND / ERR_ROOM_FULL /
     *         ERR_ROOM_NOT_JOINABLE / ERR_PLAYER_ALREADY_IN_ROOM
     */
    Result JoinRoom(const std::string& room_id, const std::string& player_id);

    /**
     * @brief 离开房间
     * @param room_id 房间 ID
     * @param player_id 玩家 ID
     * @return 失败时 .code 对应 ERR_ROOM_NOT_FOUND / ERR_PLAYER_NOT_IN_ROOM
     * @note 离开后若房间为空，GameRoom 内部自动标记 DESTROYED
     */
    Result LeaveRoom(const std::string& room_id, const std::string& player_id);

    /**
     * @brief 开始游戏
     * @param room_id 房间 ID
     * @param requester_id 请求者 ID（必须是房主）
     * @return 失败时 .code 对应 ERR_ROOM_NOT_FOUND / ERR_NOT_OWNER / ERR_WRONG_ROOM_STATE
     */
    Result StartGame(const std::string& room_id, const std::string& requester_id);

    // ---- 带通知的操作（成功后自动广播事件） ----

    /**
     * @brief 加入房间 + 自动广播 PlayerJoinNtf（排除加入者自身）
     * @param room_id 房间 ID
     * @param player_id 玩家 ID
     * @param broadcast Broadcast 实例指针
     * @return 同 JoinRoom
     */
    Result JoinRoomAndNotify(const std::string& room_id, const std::string& player_id,
                             Broadcast* broadcast);

    /**
     * @brief 离开房间 + 自动广播 PlayerLeaveNtf 给剩余玩家
     * @param room_id 房间 ID
     * @param player_id 玩家 ID
     * @param broadcast Broadcast 实例指针
     * @return 同 LeaveRoom
     */
    Result LeaveRoomAndNotify(const std::string& room_id, const std::string& player_id,
                              Broadcast* broadcast);

    /**
     * @brief 开始游戏 + 自动广播 GameStartNtf 给所有人
     * @param room_id 房间 ID
     * @param requester_id 请求者 ID
     * @param broadcast Broadcast 实例指针
     * @return 同 StartGame
     */
    Result StartGameAndNotify(const std::string& room_id, const std::string& requester_id,
                              Broadcast* broadcast);

    /// 查找房间，不存在返回 nullptr
    GameRoom* GetRoom(const std::string& room_id);

    /// 移除房间（从 map 中删除，同时清除该房间所有玩家的映射）
    bool RemoveRoom(const std::string& room_id);

    /// 房间总数
    size_t room_count() const {
        return rooms_.size();
    }

    /**
     * @brief 清理已销毁的房间
     * @return 本次清理的房间数
     * @note 遍历 map 移除 state == DESTROYED 的，同时清除对应玩家的映射
     */
    size_t CleanupDestroyed();

    /**
     * @brief 轮询所有房间的定时器，触发到期回调，清理已销毁房间
     * @return 本次销毁的房间数
     */
    size_t CheckRoomTimeout();

    /// 查询玩家当前所在的房间 ID（不在任何房间返回空串）
    std::string GetPlayerRoom(const std::string& player_id) const;

    /// 获取所有房间 ID 列表（供 GetRoomList 等查询使用）
    std::vector<std::string> GetAllRoomIds() const;

private:
    std::string GenerateRoomId();

    // 从 player_room_ 中移除指定玩家
    void RemovePlayerFromMap(const std::string& player_id);

    // 清除房间内所有玩家的 player_room_ 映射
    void ClearRoomPlayers(GameRoom* room);

    std::unordered_map<std::string, std::unique_ptr<GameRoom>> rooms_;
    std::unordered_map<std::string, std::string> player_room_; // player_id → room_id
    uint64_t room_id_counter_ = 1;
};

} // namespace game
