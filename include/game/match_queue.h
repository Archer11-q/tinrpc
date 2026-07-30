#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <functional>

namespace game {

/**
 * @brief 匹配队列
 *
 * 职责：
 * - 维护按 ELO 分数排序的匹配池（升序）
 * - 玩家入队/离队
 * - 为指定玩家查找最近 ELO 分数的对手（超时逐步放宽分差）
 *
 * 内部用 std::vector 按 elo_score 升序排列，std::lower_bound 二分插入。
 * 不使用 priority_queue：匹配需要找"最近分数"的邻居，而非取最大/最小值。
 *
 * 线程模型：所有方法在 EventLoop IO 线程调用，单线程无锁。
 */
class MatchQueue {
public:
    /**
     * @brief 构造匹配队列
     * @param max_wait_sec 最大等待时间（超时后分差不再放宽），默认 30s
     * @param elo_range_init 初始可接受 ELO 分差，默认 100
     * @param elo_range_expand_per_sec 每秒放宽的分差，默认 20
     */
    MatchQueue(int max_wait_sec = 30, double elo_range_init = 100.0,
               double elo_range_expand_per_sec = 20.0);

    /// 匹配成功回调（p1, score1, p2, score2）
    using MatchCallback =
        std::function<void(const std::string& p1, double s1, const std::string& p2, double s2)>;

    /**
     * @brief 玩家入队
     * @param player_id 玩家 ID
     * @param elo_score ELO 分数
     * @note 已在队列中则更新分数并重新排序
     */
    void EnterQueue(const std::string& player_id, double elo_score);

    /// 取消匹配（离队）
    void CancelMatch(const std::string& player_id);
    /// 同 CancelMatch
    void LeaveQueue(const std::string& player_id) {
        CancelMatch(player_id);
    }

    /// 设置匹配成功回调
    void SetMatchCallback(MatchCallback cb) {
        on_match_ = std::move(cb);
    }

    /**
     * @brief 为指定玩家查找对手
     * @param player_id 玩家 ID
     * @return 匹配到的 player_id（空串表示暂无合适对手）
     * @note 匹配成功后双方自动离队
     */
    std::string FindMatch(const std::string& player_id);

    /**
     * @brief 批量匹配：扫描有序队列，相邻分差在范围内的配对出队
     * @return 本次匹配成功的 (player_id, opponent_id) 列表
     */
    std::vector<std::pair<std::string, std::string>> TryMatch();

    // ---- 查询 ----

    size_t QueueSize() const {
        return queue_.size();
    } ///< 队列大小
    bool IsEmpty() const {
        return queue_.empty();
    } ///< 队列是否为空
    bool IsInQueue(const std::string& player_id) const; ///< 玩家是否在队列中
    double GetScore(const std::string& player_id) const; ///< 获取玩家分数

private:
    /// 队列条目
    struct Entry {
        std::string player_id; ///< 玩家 ID
        double elo_score = 1200.0; ///< ELO 分数（默认 1200）
        int64_t enqueue_time_ms = 0; ///< 入队时间戳（毫秒）
    };

    /// 按 elo_score 升序排列
    std::vector<Entry> queue_;

    int max_wait_sec_; ///< 最大等待时间
    double elo_range_init_; ///< 初始可接受分差
    double elo_range_expand_per_sec_; ///< 每秒放宽分差
    MatchCallback on_match_; ///< 匹配成功回调

    /// 获取当前时间（毫秒）
    static int64_t NowMs();

    /// 二分查找 player_id
    int FindIndex(const std::string& player_id) const;

    /// 计算当前可接受的分差（基于等待时间）
    double CurrentEloRange(int64_t enqueue_time_ms) const;
};

} // namespace game
