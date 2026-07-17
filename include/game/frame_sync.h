#pragma once

#include "game/timer_manager.h"
#include "game/input_buffer.h"
#include "game.pb.h"

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <functional>

namespace game {

// ============================================================
// FrameSyncManager — 帧同步管理器
//
// 职责：
// - 维护帧号计数器（从 1 开始递增）
// - 接收玩家输入，存入 InputBuffer
// - 由 TimerManager 驱动 Tick（默认 50ms = 20fps）
// - 每帧收集 InputBuffer 中该帧的所有输入，通过回调广播
//
// 不持有 TimerManager / InputBuffer 所有权（由外部 Room 传入）。
//
// 线程模型：所有方法在 EventLoop IO 线程调用，单线程无锁。
// ============================================================
class FrameSyncManager {
public:
    // 帧广播回调
    // 参数：frame_no + (player_id → input_data) 映射
    using FrameCallback = std::function<void(
        uint32_t frame_no,
        const std::unordered_map<std::string, std::vector<uint8_t>>& inputs)>;

    // tick_interval_ms: 帧间隔（默认 50ms = 20fps）
    FrameSyncManager(TimerManager* timer, InputBuffer* input_buffer,
                     int tick_interval_ms = 50);

    // 禁止拷贝
    FrameSyncManager(const FrameSyncManager&) = delete;
    FrameSyncManager& operator=(const FrameSyncManager&) = delete;

    // ---- 帧循环 ----

    // 启动帧同步（注册定时器，开始 Tick）
    void Start();

    // 停止帧同步（取消定时器）
    void Stop();

    // ---- 输入 ----

    // 接收玩家的帧输入（通常从 RPC handler 调用）
    void OnPlayerInput(uint32_t frame_no, const std::string& player_id,
                       const std::vector<uint8_t>& input);

    // ---- 广播回调 ----

    void SetFrameCallback(FrameCallback cb) { frame_callback_ = std::move(cb); }

    // ---- 查询 ----

    uint32_t CurrentFrame() const { return frame_no_; }
    bool     IsRunning()    const { return running_; }
    int      Fps()          const { return 1000 / tick_interval_ms_; }

    // ---- 手动 Tick（测试用） ----

    // 执行一帧：自增帧号 → 收集输入 → 回调广播
    // 返回本帧收集到的玩家数
    size_t Tick();

private:
    TimerManager* timer_;
    InputBuffer*  input_buffer_;
    int tick_interval_ms_;           // 帧间隔（毫秒）
    uint32_t frame_no_ = 0;          // 当前帧号
    uint64_t tick_timer_id_ = 0;     // 定时器 ID
    bool running_ = false;
    FrameCallback frame_callback_;   // 帧广播回调
};

} // namespace game
