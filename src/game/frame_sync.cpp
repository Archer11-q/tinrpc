#include "game/frame_sync.h"

#include <algorithm>

namespace game {

// ============================================================
// FrameSyncManager
// ============================================================

FrameSyncManager::FrameSyncManager(TimerManager* timer,
                                     InputBuffer* input_buffer,
                                     int tick_interval_ms,
                                     size_t history_size)
    : timer_(timer)
    , input_buffer_(input_buffer)
    , tick_interval_ms_(tick_interval_ms)
    , max_history_size_(history_size) {
}

// ---- 帧循环 ----

static void RescheduleTick(TimerManager* timer, int interval_ms,
                            FrameSyncManager* self) {
    uint64_t id = timer->Schedule(interval_ms, [timer, interval_ms, self]() {
        if (!self->IsRunning()) return;
        self->Tick();
        RescheduleTick(timer, interval_ms, self);
    });
    (void)id;
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
    frame_no_++;

    auto inputs = input_buffer_->GetInput(frame_no_);

    // 存入帧历史（无论是否有输入都记录，追帧时回放空帧）
    FrameRecord rec;
    rec.frame_no = frame_no_;
    rec.inputs = inputs;
    frame_history_.push_back(std::move(rec));

    // 淘汰过期历史
    while (frame_history_.size() > max_history_size_) {
        frame_history_.pop_front();
    }

    // 回调广播
    if (frame_callback_ && !inputs.empty()) {
        frame_callback_(frame_no_, inputs);
    }

    return inputs.size();
}

// ---- 追帧 ----

std::vector<FrameSyncManager::FrameRecord>
FrameSyncManager::GetCatchUpFrames(uint32_t client_frame_no) const {
    std::vector<FrameRecord> result;

    // 客户端已追上或超前，无需补帧
    if (client_frame_no >= frame_no_) {
        return result;
    }

    // 从历史中找 client_frame_no+1 到 min(client_frame_no+3, frame_no_）
    // 每次最多 2 帧
    uint32_t start = client_frame_no + 1;
    uint32_t end = std::min(client_frame_no + 3, frame_no_);  // +3 即最多 2 帧

    for (auto& rec : frame_history_) {
        if (rec.frame_no >= start && rec.frame_no <= end) {
            result.push_back(rec);
        }
        if (rec.frame_no > end) break;
    }

    return result;
}

} // namespace game
