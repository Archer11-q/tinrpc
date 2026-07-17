#include "game/frame_sync.h"

namespace game {

// ============================================================
// FrameSyncManager
// ============================================================

FrameSyncManager::FrameSyncManager(TimerManager* timer,
                                     InputBuffer* input_buffer,
                                     int tick_interval_ms)
    : timer_(timer)
    , input_buffer_(input_buffer)
    , tick_interval_ms_(tick_interval_ms) {
}

// ---- 帧循环 ----

// 周期性 tick 辅助：Tick 完成后重新注册定时器
static void RescheduleTick(TimerManager* timer, int interval_ms,
                            FrameSyncManager* self) {
    uint64_t id = timer->Schedule(interval_ms, [timer, interval_ms, self]() {
        if (!self->IsRunning()) return;
        self->Tick();
        RescheduleTick(timer, interval_ms, self);
    });
    (void)id;  // 定时器 ID 由 TimerManager 管理，此处不需要取消单个 tick
}

void FrameSyncManager::Start() {
    if (running_) return;
    running_ = true;
    RescheduleTick(timer_, tick_interval_ms_, this);
}

void FrameSyncManager::Stop() {
    running_ = false;
}

// ---- 输入 ----

void FrameSyncManager::OnPlayerInput(uint32_t frame_no,
                                      const std::string& player_id,
                                      const std::vector<uint8_t>& input) {
    input_buffer_->AddInput(frame_no, player_id, input);
}

// ---- Tick ----

size_t FrameSyncManager::Tick() {
    frame_no_++;  // 帧号自增（起始帧号 = 1）

    // 从 InputBuffer 取出当前帧所有玩家的输入
    auto inputs = input_buffer_->GetInput(frame_no_);

    // 回调广播（有输入才广播，空帧跳过）
    if (frame_callback_ && !inputs.empty()) {
        frame_callback_(frame_no_, inputs);
    }

    return inputs.size();
}

} // namespace game
