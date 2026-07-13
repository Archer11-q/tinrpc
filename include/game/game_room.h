#pragma once

#include "game/timer_manager.h"
#include "game.pb.h"

#include <string>
#include <vector>
#include <cstdint>

namespace game {

// ============================================================
// GameRoom — 游戏房间状态机
//
// 职责：
// - 管理房间内玩家列表
// - 维护房间状态（IDLE → WAITING → PLAYING → FINISHED → DESTROYED）
// - 通过内置 TimerManager 管理超时
//
// 不感知网络，只操作数据和状态。
// ============================================================
class GameRoom {
public:
    // 房间创建配置
    struct Config {
        int max_players = 4;  // 最大人数，默认 4
    };

    // 构造：传入 room_id、房主 ID、配置
    GameRoom(const std::string& room_id,
             const std::string& owner_id,
             const Config& config);

    // 禁止拷贝
    GameRoom(const GameRoom&) = delete;
    GameRoom& operator=(const GameRoom&) = delete;

    // ---- 查询 ----

    const std::string& room_id()     const { return room_id_; }
    RoomStatus         state()       const { return state_; }
    int                player_count() const { return static_cast<int>(players_.size()); }
    int                max_players()  const { return max_players_; }
    bool               is_full()      const { return player_count() >= max_players_; }
    bool               is_empty()     const { return players_.empty(); }

    const std::string& owner_id() const { return owner_id_; }

    // ---- 玩家操作 ----

    // 添加玩家。返回 false 表示：人数已满 / 已存在 / 状态不允许
    bool AddPlayer(const std::string& player_id);

    // 移除玩家。返回 false 表示玩家不在房间内
    // 移除后若房间为空，自动将状态设为 DESTROYED
    bool RemovePlayer(const std::string& player_id);

    // 检查玩家是否在房间内
    bool HasPlayer(const std::string& player_id) const;

    // 获取玩家列表（只读）
    const std::vector<std::string>& players() const { return players_; }

    // ---- 状态管理 ----

    void SetState(RoomStatus new_state) { state_ = new_state; }

    // ---- 定时器 ----

    TimerManager& timer() { return timer_; }

    // ---- 导出 ----

    // 导出为 protobuf RoomInfo（供查询/广播使用）
    RoomInfo ToProto() const;

private:
    std::string room_id_;
    std::string owner_id_;          // 房主 player_id
    RoomStatus  state_ = ROOM_STATE_IDLE;
    std::vector<std::string> players_;  // 玩家 ID 列表
    int         max_players_;
    TimerManager timer_;            // 房间专属定时器（管理超时等）
};

} // namespace game
