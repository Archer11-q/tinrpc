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

/**
 * @brief MatchService — 匹配服务（匹配队列 + 房间创建 + 通知 + 超时）
 *
 * 职责：
 * - 封装 MatchQueue + RoomManager + Broadcast + TimerManager 的协作
 * - 玩家入队/取消
 * - 匹配成功 → 自动创建房间 → 通知双方（MatchFoundNtf） → 超时计时
 * - 超时未接受 → 释放房间 → 重新入队
 *
 * @note 所有方法在 EventLoop IO 线程调用，单线程无锁。
 */
class MatchService {
public:
    /** @brief 构造函数
     *  @param mq              匹配队列指针（不持有所有权）
     *  @param room_mgr        房间管理器指针（不持有所有权）
     *  @param broadcast       广播器指针（不持有所有权）
     *  @param timer           定时器管理器指针（不持有所有权）
     *  @param room_timeout_ms 匹配成功后等待双方确认的超时（毫秒），默认 30s
     */
    MatchService(MatchQueue* mq, RoomManager* room_mgr, Broadcast* broadcast, TimerManager* timer,
                 int64_t room_timeout_ms = 30000);

    /** @brief 入队
     *  @param player_id 玩家 ID
     *  @param elo_score 玩家 ELO 分数
     */
    void EnterQueue(const std::string& player_id, double elo_score);

    /** @brief 取消匹配
     *  @param player_id 玩家 ID
     */
    void CancelMatch(const std::string& player_id);

    /** @brief 执行一轮批量匹配
     *  @return 匹配成功次数
     */
    size_t TryMatch();

    // 查询
    bool IsInQueue(const std::string& player_id) const; ///< 是否在队列中
    size_t QueueSize() const; ///< 当前队列长度

private:
    /** @brief 匹配成功后：创建房间 + 通知双方 + 启动超时定时器
     *  @param p1     玩家1 ID
     *  @param p2     玩家2 ID
     *  @param score1 玩家1 ELO 分数
     *  @param score2 玩家2 ELO 分数
     */
    void OnMatchSuccess(const std::string& p1, const std::string& p2, double score1, double score2);

    MatchQueue* mq_;
    RoomManager* room_mgr_;
    Broadcast* broadcast_;
    TimerManager* timer_;
    int64_t room_timeout_ms_;
};

} // namespace game
