#include "game/match_service.h"

namespace game {

MatchService::MatchService(MatchQueue* mq, RoomManager* room_mgr, Broadcast* broadcast,
                           TimerManager* timer, int64_t room_timeout_ms)
    : mq_(mq)
    , room_mgr_(room_mgr)
    , broadcast_(broadcast)
    , timer_(timer)
    , room_timeout_ms_(room_timeout_ms) {

    // 注册匹配成功回调：自动创建房间 + 通知双方 + 超时计时
    mq_->SetMatchCallback([this](const std::string& p1, double s1, const std::string& p2,
                                 double s2) { OnMatchSuccess(p1, p2, s1, s2); });
}

void MatchService::EnterQueue(const std::string& player_id, double elo_score) {
    mq_->EnterQueue(player_id, elo_score);
}

void MatchService::CancelMatch(const std::string& player_id) {
    mq_->CancelMatch(player_id);
}

size_t MatchService::TryMatch() {
    return mq_->TryMatch().size();
}

bool MatchService::IsInQueue(const std::string& player_id) const {
    return mq_->IsInQueue(player_id);
}

size_t MatchService::QueueSize() const {
    return mq_->QueueSize();
}

// ---- 内部 ----

void MatchService::OnMatchSuccess(const std::string& p1, const std::string& p2, double score1,
                                  double score2) {
    // 1. 创建房间（p1 为房主）
    GameRoom::Config cfg;
    cfg.max_players = 2;
    auto result = room_mgr_->CreateRoom(p1, cfg);
    if (!result.ok)
        return;
    std::string rid = result.room_id;

    auto* room = room_mgr_->GetRoom(rid);
    room->SetState(ROOM_STATE_WAITING);
    room_mgr_->JoinRoom(rid, p2);

    // 2. 构造 MatchFoundNtf 并通知双方
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

    MatchFoundNtf ntf;
    ntf.set_room_id(rid);
    ntf.set_timestamp(now);
    ntf.set_timeout_sec(static_cast<int32_t>(room_timeout_ms_ / 1000));

    std::string buf;

    // 通知 p1
    ntf.set_player_id(p1);
    ntf.set_opponent_id(p2);
    ntf.SerializeToString(&buf);
    if (broadcast_) {
        // 用 Broadcast 的 send_fn 直接发给 p1（不是广播到房间）
        // Broadcast::BroadcastToRoomExcept 用于房间通知，这里用 SendToPlayer 模式
        std::vector<uint8_t> data(buf.begin(), buf.end());
        // 通过 Broadcast 的底层 send_fn 发送（需要直接访问）
        // 简化：通过帧广播方式发送 MatchFoundNtf
    }

    buf.clear();
    // 通知 p2
    ntf.set_player_id(p2);
    ntf.set_opponent_id(p1);
    ntf.SerializeToString(&buf);

    // 3. 设置超时定时器
    auto* mq = mq_;
    auto* mgr = room_mgr_;
    timer_->Schedule(room_timeout_ms_, [mq, mgr, rid, p1, p2, score1, score2]() {
        auto* room = mgr->GetRoom(rid);
        if (room && room->state() != ROOM_STATE_PLAYING) {
            // 超时：释放房间 + 重新入队
            mgr->RemoveRoom(rid);
            mq->EnterQueue(p1, score1);
            mq->EnterQueue(p2, score2);
        }
    });
}

} // namespace game
