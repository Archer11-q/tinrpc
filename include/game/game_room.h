#pragma once

#include "game/timer_manager.h"
#include "game/input_buffer.h"
#include "game/frame_sync.h"
#include "game/snapshot_manager.h"
#include "game.pb.h"

#include <string>
#include <vector>
#include <cstdint>

namespace game {

// ============================================================
// Result — 操作结果（ok + ErrorCode）
//
// 提供 operator bool() 向后兼容旧代码中 assert(result) 的写法。
// 调用方可通过 .code 获取具体错误码。
// ============================================================
struct Result {
    bool      ok   = true;
    ErrorCode code = ERR_NONE;
    std::string room_id;  // 仅 CreateRoom 成功时有效

    Result() : ok(true), code(ERR_NONE) {}
    Result(bool ok_, ErrorCode code_) : ok(ok_), code(code_) {}

    explicit operator bool() const { return ok; }

    static Result Success() { return {}; }
    static Result Failure(ErrorCode c) { return {false, c}; }
    static Result CreateSuccess(const std::string& rid) {
        Result r;
        r.room_id = rid;
        return r;
    }
};

// ============================================================
// GameRoom — 游戏房间状态机
//
// 职责：
// - 管理房间内玩家列表
// - 维护房间状态（IDLE → WAITING → PLAYING → FINISHED → DESTROYED）
// - 通过内置 TimerManager 管理超时
//
// 线程模型：所有方法必须在 EventLoop IO 线程调用，由调用方保证。
// 当前阶段无锁——players_ 和 state_ 的读写全部收敛在同一个 IO 线程内。
// 未来若 ThreadPool worker 需要修改房间状态，通过 eventfd 投回 IO 线程执行。
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

    // 添加玩家。失败时 .code 对应：ROOM_FULL / ROOM_NOT_JOINABLE / PLAYER_ALREADY_IN_ROOM
    Result AddPlayer(const std::string& player_id);

    // 移除玩家。失败时 .code 对应：PLAYER_NOT_IN_ROOM
    // 移除后若房间为空，自动将状态设为 DESTROYED
    Result RemovePlayer(const std::string& player_id);

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

    // ---- 帧同步（v0.9） ----

    // 初始化帧同步系统（游戏开始前调用一次）
    void InitFrameSync(int fps = 20, size_t history_size = 120,
                        size_t snapshot_max = 60);

    // 启动/停止帧同步
    void StartFrameSync();
    void StopFrameSync();

    // 接收玩家输入
    void OnPlayerFrameInput(uint32_t frame_no, const std::string& player_id,
                             const std::vector<uint8_t>& input);

    // 查询帧同步组件
    FrameSyncManager*  GetFrameSync()      { return frame_sync_.get(); }
    InputBuffer*       GetInputBuffer()    { return input_buffer_.get(); }
    SnapshotManager*   GetSnapshotManager() { return snapshot_mgr_.get(); }
    bool               HasFrameSync() const { return frame_sync_ != nullptr; }

private:
    std::string room_id_;
    std::string owner_id_;          // 房主 player_id
    RoomStatus  state_ = ROOM_STATE_IDLE;
    std::vector<std::string> players_;  // 玩家 ID 列表
    int         max_players_;
    TimerManager timer_;            // 房间专属定时器（管理超时等）

    // 帧同步组件（v0.9，optional — 仅 PLAYING 状态使用）
    std::unique_ptr<InputBuffer>      input_buffer_;
    std::unique_ptr<FrameSyncManager> frame_sync_;
    std::unique_ptr<SnapshotManager>  snapshot_mgr_;
};

} // namespace game
