#include "game/input_buffer.h"

#include <algorithm>

namespace game {

// ============================================================
// InputBuffer
// ============================================================

InputBuffer::InputBuffer(size_t max_frames)
    : max_frames_(max_frames) {
}

auto InputBuffer::FindFrame(uint32_t frame_no) -> decltype(buffer_.begin()) {
    // deque 按 frame_no 升序，二分查找
    auto it = std::lower_bound(
        buffer_.begin(), buffer_.end(), frame_no,
        [](const FrameInput& fi, uint32_t no) { return fi.frame_no < no; }
    );
    if (it != buffer_.end() && it->frame_no == frame_no) {
        return it;
    }
    return buffer_.end();
}

void InputBuffer::AddInput(uint32_t frame_no, const std::string& player_id,
                            const std::vector<uint8_t>& input) {
    // 1. 查找是否已有该帧
    auto it = std::lower_bound(
        buffer_.begin(), buffer_.end(), frame_no,
        [](const FrameInput& fi, uint32_t no) { return fi.frame_no < no; }
    );

    if (it != buffer_.end() && it->frame_no == frame_no) {
        // 帧已存在：追加该玩家的输入（覆盖旧值）
        it->players[player_id] = input;
    } else {
        // 帧不存在：创建新帧并插入有序位置
        FrameInput fi;
        fi.frame_no = frame_no;
        fi.players[player_id] = input;
        buffer_.insert(it, std::move(fi));
    }

    // 2. 超量时淘汰最旧的帧
    while (buffer_.size() > max_frames_) {
        buffer_.pop_front();
    }
}

std::unordered_map<std::string, std::vector<uint8_t>>
InputBuffer::GetInput(uint32_t frame_no) {
    auto it = FindFrame(frame_no);
    if (it == buffer_.end()) {
        return {};  // 帧不存在或已过期
    }

    auto result = std::move(it->players);
    buffer_.erase(it);
    return result;
}

void InputBuffer::ClearUpTo(uint32_t frame_no) {
    // 移除所有 frame_no < 指定值的帧
    while (!buffer_.empty() && buffer_.front().frame_no < frame_no) {
        buffer_.pop_front();
    }
}

void InputBuffer::Clear() {
    buffer_.clear();
}

} // namespace game
