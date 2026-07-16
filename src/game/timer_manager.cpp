#include "game/timer_manager.h"

#include <algorithm>

namespace game {

uint64_t TimerManager::Schedule(int64_t delay_ms, Callback callback) {
    Timer t;
    t.id = next_id_++;
    t.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
    t.callback = std::move(callback);
    t.cancelled = false;

    heap_.push_back(std::move(t));
    std::push_heap(heap_.begin(), heap_.end(), std::greater<Timer>{});

    return t.id;
}

void TimerManager::Cancel(uint64_t timer_id) {
    // 惰性删除：只标记，不立即移除
    // 在 Tick() 中清理堆顶的 cancelled timer
    for (auto& t : heap_) {
        if (t.id == timer_id) {
            t.cancelled = true;
            t.callback = nullptr;  // 释放回调持有的资源
            return;
        }
    }
}

void TimerManager::CleanCancelled() {
    // 清理堆顶所有已取消的 timer
    while (!heap_.empty() && heap_.front().cancelled) {
        std::pop_heap(heap_.begin(), heap_.end(), std::greater<Timer>{});
        heap_.pop_back();
    }
}

size_t TimerManager::Tick() {
    CleanCancelled();

    size_t fired = 0;
    auto now = std::chrono::steady_clock::now();

    // 堆顶到期 → 弹出 → 执行回调 → 继续检查
    while (!heap_.empty() && heap_.front().deadline <= now) {
        Timer t = std::move(heap_.front());

        std::pop_heap(heap_.begin(), heap_.end(), std::greater<Timer>{});
        heap_.pop_back();

        if (!t.cancelled && t.callback) {
            t.callback();
            fired++;
        }
    }

    return fired;
}

} // namespace game
