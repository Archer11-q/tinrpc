#include "game/game_room.h"

#include <algorithm>

namespace game {

GameRoom::GameRoom(const std::string& room_id,
                   const std::string& owner_id,
                   const Config& config)
    : room_id_(room_id)
    , owner_id_(owner_id)
    , state_(ROOM_STATE_IDLE)
    , max_players_(config.max_players)
{
    // 创建房间时，房主自动加入
    players_.push_back(owner_id);
}

Result GameRoom::AddPlayer(const std::string& player_id) {
    // 只在 IDLE 或 WAITING 状态下允许加入
    if (state_ != ROOM_STATE_IDLE && state_ != ROOM_STATE_WAITING) {
        return Result::Failure(ERR_ROOM_NOT_JOINABLE);
    }

    // 检查人数上限
    if (is_full()) {
        return Result::Failure(ERR_ROOM_FULL);
    }

    // 检查是否已在房间内
    if (HasPlayer(player_id)) {
        return Result::Failure(ERR_PLAYER_ALREADY_IN_ROOM);
    }

    players_.push_back(player_id);
    return Result::Success();
}

Result GameRoom::RemovePlayer(const std::string& player_id) {
    auto it = std::find(players_.begin(), players_.end(), player_id);
    if (it == players_.end()) {
        return Result::Failure(ERR_PLAYER_NOT_IN_ROOM);
    }

    players_.erase(it);

    // 房间为空 → 标记销毁
    if (players_.empty()) {
        state_ = ROOM_STATE_DESTROYED;
    }

    return Result::Success();
}

bool GameRoom::HasPlayer(const std::string& player_id) const {
    return std::find(players_.begin(), players_.end(), player_id) != players_.end();
}

RoomInfo GameRoom::ToProto() const {
    RoomInfo info;
    info.set_room_id(room_id_);
    info.set_player_count(static_cast<int32_t>(players_.size()));
    info.set_max_players(max_players_);
    info.set_room_state(state_);
    // 玩家列表由上层（GameService）填充 PlayerInfo 详情
    // 这里只填 room 基本信息
    return info;
}

// ---- 帧同步 ----

void GameRoom::InitFrameSync(int fps, size_t history_size,
                               size_t snapshot_max) {
    int interval_ms = 1000 / fps;
    input_buffer_ = std::make_unique<InputBuffer>(history_size);
    snapshot_mgr_ = std::make_unique<SnapshotManager>(snapshot_max);
    frame_sync_   = std::make_unique<FrameSyncManager>(
        &timer_, input_buffer_.get(), interval_ms, history_size);
}

void GameRoom::StartFrameSync() {
    if (frame_sync_) {
        frame_sync_->Start();
    }
}

void GameRoom::StopFrameSync() {
    if (frame_sync_) {
        frame_sync_->Stop();
    }
}

void GameRoom::OnPlayerFrameInput(uint32_t frame_no,
                                    const std::string& player_id,
                                    const std::vector<uint8_t>& input) {
    if (frame_sync_) {
        frame_sync_->OnPlayerInput(frame_no, player_id, input);
    }
}

} // namespace game
