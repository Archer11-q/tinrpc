#include "game/game_service.h"
#include "game/room_service.h"
#include "rpc/acceptor.h"
#include "game.pb.h"

#include <sys/epoll.h>
#include <cstdio>

namespace game {

GameService::GameService() {
    // 构造 Broadcast（发送回调在 Run() 中注册 PlayerConn 后才生效）
    auto send_fn = [this](const std::string& player_id, const std::vector<uint8_t>& data) {
        auto it = player_conns_.find(player_id);
        if (it != player_conns_.end() && it->second) {
            auto frame = rpc::ProtocolFrame::Encode(0, rpc::MessageType::Response, "RoomEvent", data);
            it->second->Send(frame);
        }
    };
    broadcast_ = std::make_unique<Broadcast>(&room_mgr_, send_fn);

    // 注册全部 RoomService RPC（8 个方法）
    // 注意：RoomServiceImpl 必须是成员变量，不能是局部变量，
    // 否则构造结束后 Dispatch 中注册的 lambda 持有悬空指针 → segfault
    room_svc_ = std::make_unique<RoomServiceImpl>(&room_mgr_, broadcast_.get());
    RegisterRoomService(&dispatch_, room_svc_.get());

    // 匹配成功回调：创建房间 + 通知双方 + 超时
    match_queue_.SetMatchCallback([this](const std::string& p1, double s1, const std::string& p2,
                                         double s2) { OnMatchFound(p1, s1, p2, s2); });

    // 注册 GetMetrics RPC — 暴露服务端运行指标
    dispatch_.RegisterMethod(
        "GetMetrics",
        [this](const std::vector<uint8_t>& /*body*/) -> std::optional<std::vector<uint8_t>> {
            GetMetricsRes res;
            res.set_success(true);
            auto snap = metrics_.GetSnapshot(static_cast<int32_t>(room_mgr_.GetAllRoomIds().size()),
                                             static_cast<int32_t>(match_queue_.QueueSize()));
            res.set_uptime_sec(snap.uptime_sec);
            res.set_active_connections(snap.active_connections);
            res.set_total_rooms(snap.total_rooms);
            res.set_total_requests(snap.total_requests);
            res.set_current_qps(snap.current_qps);
            res.set_avg_latency_us(snap.avg_latency_us);
            res.set_p50_latency_us(snap.p50_latency_us);
            res.set_p99_latency_us(snap.p99_latency_us);
            res.set_match_queue_size(snap.match_queue_size);
            res.set_error_count(snap.error_count);
            std::string buf;
            res.SerializeToString(&buf);
            return std::vector<uint8_t>(buf.begin(), buf.end());
        });

    // 注册 EnterMatch RPC — 客户端进入匹配队列
    dispatch_.RegisterMethod(
        "EnterMatch", [this](const std::vector<uint8_t>& body) -> std::optional<std::vector<uint8_t>> {
            MatchPlayerReq req;
            if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
                return std::nullopt;
            }
            MatchPlayerRes res;
            match_queue_.EnterQueue(req.player_id(), req.elo_score());
            // 入队后立即尝试一轮批量匹配
            match_queue_.TryMatch();
            res.set_success(true);
            std::string buf;
            res.SerializeToString(&buf);
            return std::vector<uint8_t>(buf.begin(), buf.end());
        });

    // 注册 CancelMatch RPC — 客户端取消匹配
    dispatch_.RegisterMethod(
        "CancelMatch",
        [this](const std::vector<uint8_t>& body) -> std::optional<std::vector<uint8_t>> {
            CancelMatchReq req;
            if (!req.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
                return std::nullopt;
            }
            CancelMatchRes res;
            match_queue_.CancelMatch(req.player_id());
            res.set_success(true);
            std::string buf;
            res.SerializeToString(&buf);
            return std::vector<uint8_t>(buf.begin(), buf.end());
        });

    printf("[GameService] 初始化完成: RoomService 8方法 + GetMetrics + EnterMatch/CancelMatch + "
           "MatchQueue + Broadcast\n");
}

// ---- 玩家连接管理 ----

void GameService::RegisterPlayerConn(const std::string& player_id, rpc::Connection* conn) {
    player_conns_[player_id] = conn;
    fd_to_player_[conn->GetFd()] = player_id;
}

void GameService::UnregisterPlayerConn(const std::string& player_id) {
    auto it = player_conns_.find(player_id);
    if (it != player_conns_.end() && it->second) {
        fd_to_player_.erase(it->second->GetFd());
    }
    player_conns_.erase(player_id);
}

rpc::Connection* GameService::GetPlayerConn(const std::string& player_id) const {
    auto it = player_conns_.find(player_id);
    return it != player_conns_.end() ? it->second : nullptr;
}

// ---- 服务端帧回调 ----

void GameService::OnServerFrame(const rpc::Frame& frame, rpc::Connection* conn) {
    auto t0 = std::chrono::steady_clock::now();
    metrics_.Tick();

    // Login: 建立 player → conn 映射
    if (frame.method_name == "Login") {
        LoginReq req;
        if (!req.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()))) {
            metrics_.OnError();
            auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                                  "Login", {});
            conn->Send(err);
            return;
        }
        RegisterPlayerConn(req.token(), conn);
        metrics_.OnConnect();

        LoginRes res;
        res.set_success(true);
        res.mutable_player_info()->set_player_id(req.token());
        std::string buf;
        res.SerializeToString(&buf);
        auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response, "Login",
                                              std::vector<uint8_t>(buf.begin(), buf.end()));
        conn->Send(rsp);

        auto t1 = std::chrono::steady_clock::now();
        double latency_us = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        metrics_.OnRequest(latency_us);

        printf("[GameService] 玩家登录: %s\n", req.token().c_str());
        return;
    }

    // 其他方法 → Dispatch 分发
    auto rsp_body = dispatch_.Call(frame.method_name, frame.body);
    if (rsp_body) {
        auto rsp = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Response,
                                              frame.method_name, *rsp_body);
        conn->Send(rsp);
    } else {
        metrics_.OnError();
        auto err = rpc::ProtocolFrame::Encode(frame.request_id, rpc::MessageType::Error,
                                              frame.method_name, {});
        conn->Send(err);
    }

    auto t1 = std::chrono::steady_clock::now();
    double latency_us = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    metrics_.OnRequest(latency_us);
}

// ---- 断连回调 ----

void GameService::OnPlayerDisconnected(int fd) {
    auto it = fd_to_player_.find(fd);
    if (it == fd_to_player_.end())
        return;

    std::string player_id = it->second;
    printf("[GameService] 玩家断连: %s (fd=%d)\n", player_id.c_str(), fd);

    // 1. 从房间移除
    std::string room_id = room_mgr_.GetPlayerRoom(player_id);
    if (!room_id.empty()) {
        room_mgr_.LeaveRoomAndNotify(room_id, player_id, broadcast_.get());
    }

    // 2. 从匹配队列移除
    match_queue_.CancelMatch(player_id);

    // 3. 清理连接映射
    UnregisterPlayerConn(player_id);

    // 4. 记录断连
    metrics_.OnDisconnect();
}

// ---- 匹配成功回调 ----

void GameService::OnMatchFound(const std::string& p1, double score1, const std::string& p2,
                               double score2) {
    printf("[GameService] 匹配成功: %s(%.0f) vs %s(%.0f)\n", p1.c_str(), score1, p2.c_str(), score2);

    // 1. 创建房间（p1 为房主）
    GameRoom::Config cfg;
    cfg.max_players = 2;
    auto result = room_mgr_.CreateRoom(p1, cfg);
    if (!result.ok)
        return;
    std::string rid = result.room_id;

    auto* room = room_mgr_.GetRoom(rid);
    room->SetState(ROOM_STATE_WAITING);
    room_mgr_.JoinRoom(rid, p2);

    // 2. 通知双方
    MatchFoundNtf ntf;
    ntf.set_room_id(rid);
    ntf.set_timeout_sec(30);

    std::string buf;
    ntf.set_player_id(p1);
    ntf.set_opponent_id(p2);
    ntf.SerializeToString(&buf);
    broadcast_->BroadcastToRoomExcept(rid, p2, std::vector<uint8_t>(buf.begin(), buf.end()));

    ntf.set_player_id(p2);
    ntf.set_opponent_id(p1);
    ntf.SerializeToString(&buf);
    broadcast_->BroadcastToRoomExcept(rid, p1, std::vector<uint8_t>(buf.begin(), buf.end()));

    // 3. 设置超时定时器
    timer_.Schedule(30000, [this, rid, p1, p2, score1, score2]() {
        auto* r = room_mgr_.GetRoom(rid);
        if (r && r->state() != ROOM_STATE_PLAYING) {
            room_mgr_.RemoveRoom(rid);
            match_queue_.EnterQueue(p1, score1);
            match_queue_.EnterQueue(p2, score2);
            printf("[GameService] 匹配超时: room=%s, 双方重新入队\n", rid.c_str());
        }
    });

    printf("[GameService] 房间=%s, 双方=%s/%s\n", rid.c_str(), p1.c_str(), p2.c_str());
}

// ---- 启动/停止 ----

void GameService::Run(uint16_t port) {
    auto acceptor = std::make_unique<rpc::Acceptor>(
        port, &loop_,
        [this](const rpc::Frame& frame, rpc::Connection* conn) { OnServerFrame(frame, conn); },
        [this](int fd) { OnPlayerDisconnected(fd); });

    loop_.Register(std::move(acceptor), EPOLLIN | EPOLLRDHUP | EPOLLET);
    printf("[GameService] 服务启动: port=%u\n", port);
    loop_.Run();
}

void GameService::Stop() {
    loop_.Stop();
}

} // namespace game
