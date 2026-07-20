#include "game/match_queue.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace game {

// ============================================================
// MatchQueue
// ============================================================

MatchQueue::MatchQueue(int max_wait_sec,
                         double elo_range_init,
                         double elo_range_expand_per_sec)
    : max_wait_sec_(max_wait_sec)
    , elo_range_init_(elo_range_init)
    , elo_range_expand_per_sec_(elo_range_expand_per_sec) {
}

int64_t MatchQueue::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

int MatchQueue::FindIndex(const std::string& player_id) const {
    for (size_t i = 0; i < queue_.size(); i++) {
        if (queue_[i].player_id == player_id) return static_cast<int>(i);
    }
    return -1;
}

double MatchQueue::CurrentEloRange(int64_t enqueue_time_ms) const {
    int64_t waited_ms = NowMs() - enqueue_time_ms;
    double waited_sec = static_cast<double>(waited_ms) / 1000.0;

    // 超过最大等待时间 → 不再放宽
    if (waited_sec >= max_wait_sec_) {
        return elo_range_init_ + elo_range_expand_per_sec_ * max_wait_sec_;
    }

    return elo_range_init_ + elo_range_expand_per_sec_ * waited_sec;
}

// ---- 队列操作 ----

void MatchQueue::EnterQueue(const std::string& player_id, double elo_score) {
    // 已在队列中 → 更新分数
    int idx = FindIndex(player_id);
    if (idx >= 0) {
        queue_[static_cast<size_t>(idx)].elo_score = elo_score;
        // 重新排序
        std::sort(queue_.begin(), queue_.end(),
                  [](const Entry& a, const Entry& b) {
                      return a.elo_score < b.elo_score;
                  });
        return;
    }

    Entry e;
    e.player_id        = player_id;
    e.elo_score        = elo_score;
    e.enqueue_time_ms  = NowMs();

    // 二分插入保持升序
    auto it = std::lower_bound(queue_.begin(), queue_.end(), e.elo_score,
        [](const Entry& entry, double score) {
            return entry.elo_score < score;
        });
    queue_.insert(it, std::move(e));
}

void MatchQueue::LeaveQueue(const std::string& player_id) {
    int idx = FindIndex(player_id);
    if (idx >= 0) {
        queue_.erase(queue_.begin() + idx);
    }
}

std::string MatchQueue::FindMatch(const std::string& player_id) {
    int idx = FindIndex(player_id);
    if (idx < 0) return "";  // 不在队列中

    double my_score = queue_[static_cast<size_t>(idx)].elo_score;
    double my_range = CurrentEloRange(
        queue_[static_cast<size_t>(idx)].enqueue_time_ms);

    // 向两侧搜索最近 ELO 的对手（在分差范围内的）
    int best_idx   = -1;
    double best_diff = std::numeric_limits<double>::max();

    for (size_t i = 0; i < queue_.size(); i++) {
        if (static_cast<int>(i) == idx) continue;

        double diff = std::abs(queue_[i].elo_score - my_score);
        if (diff <= my_range && diff < best_diff) {
            best_diff = diff;
            best_idx  = static_cast<int>(i);
        }
    }

    if (best_idx < 0) return "";  // 无合适对手

    std::string opponent = queue_[static_cast<size_t>(best_idx)].player_id;

    // 双方离队
    // 注意：erase 顺序要从大到小，否则索引失效
    if (idx > best_idx) {
        queue_.erase(queue_.begin() + idx);
        queue_.erase(queue_.begin() + best_idx);
    } else {
        queue_.erase(queue_.begin() + best_idx);
        queue_.erase(queue_.begin() + idx);
    }

    return opponent;
}

bool MatchQueue::IsInQueue(const std::string& player_id) const {
    return FindIndex(player_id) >= 0;
}

double MatchQueue::GetScore(const std::string& player_id) const {
    int idx = FindIndex(player_id);
    return idx >= 0 ? queue_[static_cast<size_t>(idx)].elo_score : 0.0;
}

// ---- 批量匹配 ----

std::vector<std::pair<std::string, std::string>> MatchQueue::TryMatch() {
    std::vector<std::pair<std::string, std::string>> matched;

    if (queue_.size() < 2) return matched;

    // 标记已配对的索引（避免重复消费）
    std::vector<bool> paired(queue_.size(), false);

    for (size_t i = 0; i + 1 < queue_.size(); i++) {
        if (paired[i]) continue;

        const auto& a = queue_[i];
        const auto& b = queue_[i + 1];

        double diff = std::abs(a.elo_score - b.elo_score);
        double range_a = CurrentEloRange(a.enqueue_time_ms);
        double range_b = CurrentEloRange(b.enqueue_time_ms);

        // 双方分差都在可接受范围内
        if (diff <= range_a && diff <= range_b) {
            matched.emplace_back(a.player_id, b.player_id);
            paired[i]     = true;
            paired[i + 1] = true;
            i++;  // 跳过下一个
        }
    }

    // 从后往前删除已配对玩家（保证索引有效）
    for (int i = static_cast<int>(queue_.size()) - 1; i >= 0; i--) {
        if (paired[static_cast<size_t>(i)]) {
            queue_.erase(queue_.begin() + i);
        }
    }

    return matched;
}

} // namespace game
