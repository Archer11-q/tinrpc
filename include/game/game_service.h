#pragma once

#include "game/room_manager.h"
#include "game/room_service.h"
#include "game/server_metrics.h"
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

/**
 * @brief GameService — 游戏服务端集中入口
 *
 * 持有并组装: RoomManager + MatchService + Broadcast + TimerManager
 *             + FrameSyncManager(per-room) + Dispatch(全部RPC)
 *
 * 职责：串联"匹配→房间→帧同步"完整流程
 *
 * @note 所有方法在 EventLoop IO 线程调用。
 */
class GameService {
public:
    /// @brief 发送回调: player_id → 序列化数据 → 通过 Connection 发送
    using SendToPlayerFn =
        std::function<void(const std::string& player_id, const std::vector<uint8_t>& data)>;

    /// @brief disconnect_cb: 断连时外部清理回调（如 fd → player_id 映射）
    using OnDisconnectFn = std::function<void(int fd)>;

    GameService();

    // 禁止拷贝
    GameService(const GameService&) = delete;
    GameService& operator=(const GameService&) = delete;

    // ---- 核心组件 ----

    RoomManager& room_mgr() {
        return room_mgr_;
    } ///< 房间管理器
    MatchQueue& match_queue() {
        return match_queue_;
    } ///< 匹配队列
    TimerManager& timer() {
        return timer_;
    } ///< 定时器管理器
    Broadcast& broadcast() {
        return *broadcast_;
    } ///< 广播器
    rpc::Dispatch& dispatch() {
        return dispatch_;
    } ///< RPC 分发器

    // ---- 玩家连接管理 ----

    /** @brief 注册玩家连接（Login 时调用）
     *  @param player_id 玩家 ID
     *  @param conn      Connection 指针
     */
    void RegisterPlayerConn(const std::string& player_id, rpc::Connection* conn);

    /** @brief 注销玩家连接（断连/登出时调用）
     *  @param player_id 玩家 ID
     */
    void UnregisterPlayerConn(const std::string& player_id);

    /** @brief 获取玩家连接
     *  @param player_id 玩家 ID
     *  @return Connection 指针，未找到返回 nullptr
     */
    rpc::Connection* GetPlayerConn(const std::string& player_id) const;

    /// @brief 处理客户端帧回调（替代外部 FrameCallback 设置）
    void SetFrameCallback(FrameSyncManager* fsm, const std::string& room_id);

    // ---- 启动 ----

    /** @brief 启动 EventLoop（阻塞）
     *  @param port 监听端口
     */
    void Run(uint16_t port);

    /// @brief 停止服务
    void Stop();

private:
    /// @brief 服务端帧回调：解析请求 → Dispatch 分发 → 发送响应
    void OnServerFrame(const rpc::Frame& frame, rpc::Connection* conn);

    /// @brief 断连回调
    void OnPlayerDisconnected(int fd);

    /// @brief 匹配成功回调
    void OnMatchFound(const std::string& p1, double s1, const std::string& p2, double s2);

    RoomManager room_mgr_;
    MatchQueue match_queue_;
    TimerManager timer_;
    std::unique_ptr<Broadcast> broadcast_;
    std::unique_ptr<RoomServiceImpl> room_svc_;
    ServerMetrics metrics_;

    rpc::EventLoop loop_;
    rpc::Dispatch dispatch_;

    /// @brief player_id → Connection 映射
    std::unordered_map<std::string, rpc::Connection*> player_conns_;
    /// @brief fd → player_id 映射
    std::unordered_map<int, std::string> fd_to_player_;
};

} // namespace game
