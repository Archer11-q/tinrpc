#pragma once

#include "game/match_queue.h"
#include "game/room_manager.h"
#include "game/broadcast.h"
#include "game/timer_manager.h"
#include "game.pb.h"

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <chrono>

namespace game {

// ============================================================
// MatchService — 匹配服务（匹配队列 + 房间创建 + 通知 + 超时）
//
// 职责：
// - 封装 MatchQueue + RoomManager + Broadcast + TimerManager 的协作
// - 玩家入队/取消
// - 匹配成功 → 自动创建房间 → 通知双方（MatchFoundNtf） → 超时计时
// - 超时未接受 → 释放房间 → 重新入队
//
// 线程模型：所有方法在 EventLoop IO 线程调用，单线程无锁。
// ============================================================
class MatchService {
public:
    // room_timeout_ms: 匹配成功后等待双方确认的超时（毫秒），默认 30s
    MatchService(MatchQueue* mq, RoomManager* room_mgr,
                  Broadcast* broadcast, TimerManager* timer,
                  int64_t room_timeout_ms = 30000);

    // 入队
    void EnterQueue(const std::string& player_id, double elo_score);

    // 取消匹配
    void CancelMatch(const std::string& player_id);

    // 执行一轮批量匹配
    size_t TryMatch();

    // 查询
    bool   IsInQueue(const std::string& player_id) const;
    size_t QueueSize() const;

private:
    // 匹配成功后：创建房间 + 通知双方 + 启动超时定时器
    void OnMatchSuccess(const std::string& p1, const std::string& p2,
                         double score1, double score2);

    MatchQueue*  mq_;
    RoomManager* room_mgr_;
    Broadcast*   broadcast_;
    TimerManager* timer_;
    int64_t      room_timeout_ms_;
};

} // namespace game
