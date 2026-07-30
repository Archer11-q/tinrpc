#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <chrono>
#include <string>

namespace game {

/**
 * @brief 基于小顶堆的定时器管理器
 *
 * 职责：
 * - 注册定时任务（delay_ms 后触发 callback）
 * - 惰性取消（Cancel 后标记删除，Tick 时跳过）
 * - Tick 由外部驱动（GameRoom 或 EventLoop 定时调用）
 *
 * 线程安全：所有操作必须在同一线程调用（设计上与 EventLoop
 * 单线程模型一致），不加锁。
 *
 * 使用示例：
 * @code
 *   TimerManager tm;
 *   auto id = tm.Schedule(5000, []{ printf("5秒到了\n"); });
 *   tm.Schedule(10000, []{ printf("10秒到了\n"); });
 *   tm.Tick();  // 每秒调一次，自动触发到期回调
 * @endcode
 */
class TimerManager {
public:
    using Callback = std::function<void()>;

    TimerManager() = default;
    ~TimerManager() = default;

    // 禁止拷贝
    TimerManager(const TimerManager&) = delete;
    TimerManager& operator=(const TimerManager&) = delete;

    /**
     * @brief 注册定时任务
     * @param delay_ms 延迟毫秒数
     * @param callback 到期时执行的回调
     * @return timer_id，可用于 Cancel
     */
    uint64_t Schedule(int64_t delay_ms, Callback callback);

    /// 取消定时任务（惰性删除，Tick 时跳过）
    void Cancel(uint64_t timer_id);

    /**
     * @brief 驱动定时器：检查堆顶，触发所有到期回调
     * @return 本次触发的回调数量
     */
    size_t Tick();

    /// 查询待触发定时器数量（含已取消但未清理的）
    size_t PendingCount() const {
        return heap_.size();
    }

private:
    // 定时器节点
    struct Timer {
        uint64_t id;
        std::chrono::steady_clock::time_point deadline;
        Callback callback;
        bool cancelled = false;

        // 小顶堆：deadline 小的在堆顶
        bool operator>(const Timer& other) const {
            return deadline > other.deadline;
        }
    };

    // 清理堆顶所有已取消的 timer（惰性删除）
    void CleanCancelled();

    uint64_t next_id_ = 1;

    // 用 vector + push_heap/pop_heap 手动管理小顶堆
    // 选择 vector 而非 priority_queue，因为需要惰性删除 +
    // 遍历标记 cancelled（priority_queue 不暴露底层容器）
    std::vector<Timer> heap_;
};

} // namespace game
