#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <cstddef>

namespace game {

// ============================================================
// SessionManager — 会话管理器（断线重连基础）
//
// 职责：
// - 管理客户端连接会话（create/validate/destroy）
// - 心跳超时检测（ACTIVE → DISCONNECTED）
// - 宽限期管理（DISCONNECTED 期间允许重连）
// - 宽限期到期处理（EXPIRED → 退房 + 退队）
//
// 当前状态（v0.10）：接口已定义，实现待第 10 周
//
// 线程模型：所有方法在 EventLoop IO 线程调用，单线程无锁。
// ============================================================
class SessionManager {
public:
    // Tick 返回结构
    struct TickResult {
        size_t disconnected = 0;  // 新标记为断连的 session 数
        size_t expired      = 0;  // 宽限期到期的 session 数
    };

    // max_sessions:         最大并发 session 数
    // heartbeat_timeout_ms: 心跳超时（毫秒），默认 15s
    // grace_period_ms:      断连宽限期（毫秒），默认 30s
    SessionManager(size_t max_sessions = 1000,
                   int64_t heartbeat_timeout_ms = 15000,
                   int64_t grace_period_ms = 30000);

    // 禁止拷贝
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // ---- 接口定义（待下一阶段实现） ----

    // 创建 session（登录时调用）
    // 返回 session_id（调用方需转发给客户端保存）
    std::string CreateSession(const std::string& player_id);

    // 验证 session（请求/重连时调用，成功则刷新心跳计时器）
    // 返回 player_id；session 无效/过期返回空串
    std::string ValidateSession(const std::string& session_id);

    // 心跳刷新（客户端定期 Ping 时调用）
    // 返回 true 表示 session 有效且已刷新
    bool Heartbeat(const std::string& session_id);

    // 销毁 session（登出 / 宽限期到期 / 主动踢出）
    void DestroySession(const std::string& session_id);

    // Tick：检查心跳超时 + 宽限期到期（需外部 Timer 驱动）
    TickResult Tick();

    // ---- 查询 ----

    size_t ActiveCount()       const;
    size_t DisconnectedCount() const;
    size_t TotalCount()        const;

private:
    // 生成唯一 session_id
    std::string GenerateSessionId();

    // Session 状态机
    enum class SessionState { ACTIVE, DISCONNECTED, EXPIRED };

    struct Session {
        std::string session_id;
        std::string player_id;
        SessionState state = SessionState::ACTIVE;
        int64_t last_heartbeat_ms  = 0;   // 最后心跳时间
        int64_t disconnect_time_ms = 0;   // 断连时刻（仅 DISCONNECTED 状态有效）
    };

    // session_id → Session
    std::unordered_map<std::string, Session> sessions_;

    // player_id → session_id（快速反查）
    std::unordered_map<std::string, std::string> player_to_session_;

    size_t   max_sessions_;
    int64_t  heartbeat_timeout_ms_;
    int64_t  grace_period_ms_;
    uint64_t id_counter_ = 1;
};

} // namespace game
