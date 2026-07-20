#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace game {

// ============================================================
// MatchQueue — 匹配队列
//
// 职责：
// - 维护按 ELO 分数排序的匹配池（升序）
// - 玩家入队/离队
// - 为指定玩家查找最近 ELO 分数的对手（超时逐步放宽分差）
//
// 内部用 std::vector 按 elo_score 升序排列，std::lower_bound 二分插入。
// 不使用 priority_queue：匹配需要找"最近分数"的邻居，而非取最大/最小值。
//
// 线程模型：所有方法在 EventLoop IO 线程调用，单线程无锁。
// ============================================================
class MatchQueue {
public:
    // max_wait_sec:       最大等待时间（超时后分差不再放宽）
    // elo_range_init:     初始可接受 ELO 分差
    // elo_range_expand:   每秒放宽的分差
    MatchQueue(int max_wait_sec = 30,
               double elo_range_init = 100.0,
               double elo_range_expand_per_sec = 20.0);

    // 入队
    void EnterQueue(const std::string& player_id, double elo_score);

    // 离队
    void LeaveQueue(const std::string& player_id);

    // 为指定玩家查找对手
    // 返回匹配到的 player_id（空串表示暂无合适对手）
    // 匹配成功后双方自动离队
    std::string FindMatch(const std::string& player_id);

    // 批量匹配：扫描有序队列，相邻分差在范围内的配对出队
    // 返回本次匹配成功的 (player_id, opponent_id) 列表
    std::vector<std::pair<std::string, std::string>> TryMatch();

    // 查询
    size_t QueueSize()                  const { return queue_.size(); }
    bool   IsEmpty()                    const { return queue_.empty(); }
    bool   IsInQueue(const std::string& player_id) const;
    double GetScore(const std::string& player_id) const;

private:
    struct Entry {
        std::string player_id;
        double      elo_score = 1200.0;    // 默认初始分
        int64_t     enqueue_time_ms = 0;   // 入队时间戳
    };

    // 按 elo_score 升序排列
    std::vector<Entry> queue_;

    int    max_wait_sec_;
    double elo_range_init_;
    double elo_range_expand_per_sec_;

    // 获取当前时间（毫秒）
    static int64_t NowMs();

    // 二分查找 player_id
    int FindIndex(const std::string& player_id) const;

    // 计算当前可接受的分差（基于等待时间）
    double CurrentEloRange(int64_t enqueue_time_ms) const;
};

} // namespace game
