#pragma once

#include "game/room_manager.h"
#include "game/match_service.h"
#include "game/broadcast.h"
#include "game/timer_manager.h"
#include "game/frame_sync.h"
#include "rpc/event_loop.h"
#include "rpc/dispatch.h"
#include "rpc/connection.h"
#include "rpc/protocol.h"

#include <unordered_map>
#include <string>
#include <functional>
#include <memory>

namespace game {

// ============================================================
// GameService — 游戏服务端集中入口
//
// 持有并组装: RoomManager + MatchService + Broadcast + TimerManager
//             + FrameSyncManager(per-room) + Dispatch(全部RPC)
//
// 职责：串联"匹配→房间→帧同步"完整流程
//
// 线程模型：所有方法在 EventLoop IO 线程调用。
// ============================================================
class GameService {
public:
    // 发送回调: player_id → 序列化数据 → 通过 Connection 发送
    using SendToPlayerFn = std::function<void(const std::string& player_id,
                                               const std::vector<uint8_t>& data)>;

    // disconnect_cb: 断连时外部清理回调（如 fd → player_id 映射）
    using OnDisconnectFn = std::function<void(int fd)>;

    GameService();

    // 禁止拷贝
    GameService(const GameService&) = delete;
    GameService& operator=(const GameService&) = delete;

    // ---- 核心组件 ----

    RoomManager&   room_mgr()    { return room_mgr_; }
    MatchQueue&    match_queue() { return match_queue_; }
    TimerManager&  timer()       { return timer_; }
    Broadcast&     broadcast()   { return *broadcast_; }
    rpc::Dispatch& dispatch()    { return dispatch_; }

    // ---- 玩家连接管理 ----

    // 注册/注销玩家连接（Login/断连时调用）
    void RegisterPlayerConn(const std::string& player_id, rpc::Connection* conn);
    void UnregisterPlayerConn(const std::string& player_id);
    rpc::Connection* GetPlayerConn(const std::string& player_id) const;

    // 处理客户端帧回调（替代外部 FrameCallback 设置）
    void SetFrameCallback(FrameSyncManager* fsm, const std::string& room_id);

    // ---- 启动 ----

    // 启动 EventLoop（阻塞）
    void Run(uint16_t port);

    // 停止
    void Stop();

private:
    // 服务端帧回调：解析请求 → Dispatch 分发 → 发送响应
    void OnServerFrame(const rpc::Frame& frame, rpc::Connection* conn);

    // 断连回调
    void OnPlayerDisconnected(int fd);

    // 匹配成功回调
    void OnMatchFound(const std::string& p1, double s1,
                       const std::string& p2, double s2);

    RoomManager   room_mgr_;
    MatchQueue    match_queue_;
    TimerManager  timer_;
    std::unique_ptr<Broadcast> broadcast_;

    rpc::EventLoop loop_;
    rpc::Dispatch  dispatch_;

    // player_id → Connection
    std::unordered_map<std::string, rpc::Connection*> player_conns_;
    // fd → player_id
    std::unordered_map<int, std::string> fd_to_player_;
};

} // namespace game
