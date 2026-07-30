#pragma once

#include "game/timer_manager.h"
#include "game/input_buffer.h"
#include "game.pb.h"

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <deque>
#include <functional>

namespace game {

/**
 * @brief 帧同步管理器
 *
 * 职责：
 * - 维护帧号计数器（从 1 开始递增）
 * - 接收玩家输入，存入 InputBuffer
 * - 由 TimerManager 驱动 Tick（默认 50ms = 20fps）
 * - 每帧收集 InputBuffer 中该帧的所有输入，通过回调广播
 * - 保存帧历史，支持慢客户端追帧（catch-up）
 *
 * 不持有 TimerManager / InputBuffer 所有权（由外部 Room 传入）。
 *
 * 线程模型：所有方法在 EventLoop IO 线程调用，单线程无锁。
 */
class FrameSyncManager {
public:
    /// 帧输入映射：player_id → input_data
    using FrameInputs = std::unordered_map<std::string, std::vector<uint8_t>>;

    /// 帧广播回调（参数：frame_no + 本帧所有玩家输入）
    using FrameCallback = std::function<void(uint32_t frame_no, const FrameInputs& inputs)>;

    /// 帧历史记录
    struct FrameRecord {
        uint32_t frame_no = 0; ///< 帧号
        FrameInputs inputs; ///< 该帧所有玩家的输入
    };

    /**
     * @brief 构造帧同步管理器
     * @param timer 定时器管理器指针（不持有所有权）
     * @param input_buffer 输入缓冲区指针（不持有所有权）
     * @param tick_interval_ms 帧间隔（默认 50ms = 20fps）
     * @param history_size 帧历史缓冲区大小（默认 120 帧 ≈ 6s @20fps）
     */
    FrameSyncManager(TimerManager* timer, InputBuffer* input_buffer, int tick_interval_ms = 50,
                     size_t history_size = 120);

    // 禁止拷贝
    FrameSyncManager(const FrameSyncManager&) = delete;
    FrameSyncManager& operator=(const FrameSyncManager&) = delete;

    // ---- 帧循环 ----

    /// 启动帧同步（开始定时 Tick）
    void Start();
    /// 停止帧同步
    void Stop();

    // ---- 输入 ----

    /**
     * @brief 接收玩家帧输入
     * @param frame_no 帧号
     * @param player_id 玩家 ID
     * @param input 序列化后的输入数据
     */
    void OnPlayerInput(uint32_t frame_no, const std::string& player_id,
                       const std::vector<uint8_t>& input);

    // ---- 广播回调 ----

    /// 设置帧广播回调（每帧 Tick 后触发）
    void SetFrameCallback(FrameCallback cb) {
        frame_callback_ = std::move(cb);
    }

    // ---- 追帧（catch-up） ----

    /**
     * @brief 获取客户端落后的帧数据（每次最多 2 帧）
     * @param client_frame_no 客户端当前帧号
     * @return 客户端缺失的帧列表（按帧号升序，每次最多 2 帧）
     */
    std::vector<FrameRecord> GetCatchUpFrames(uint32_t client_frame_no) const;

    // ---- 查询 ----

    uint32_t CurrentFrame() const {
        return frame_no_;
    } ///< 当前帧号
    bool IsRunning() const {
        return running_;
    } ///< 是否正在运行
    int Fps() const {
        return 1000 / tick_interval_ms_;
    } ///< 帧率
    size_t HistorySize() const {
        return frame_history_.size();
    } ///< 当前帧历史条数
    size_t MaxHistorySize() const {
        return max_history_size_;
    } ///< 最大帧历史条数

    /**
     * @brief 手动推进一帧（测试用）
     * @return 本帧收集到的输入数
     */
    size_t Tick();

private:
    TimerManager* timer_;
    InputBuffer* input_buffer_;
    int tick_interval_ms_;
    uint32_t frame_no_ = 0;
    uint64_t tick_timer_id_ = 0;
    bool running_ = false;
    FrameCallback frame_callback_;

    // 帧历史（追帧用）
    std::deque<FrameRecord> frame_history_;
    size_t max_history_size_;
};

} // namespace game
