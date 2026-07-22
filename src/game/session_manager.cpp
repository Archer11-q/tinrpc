#include "game/session_manager.h"

#include <sstream>
#include <iomanip>
#include <chrono>

namespace game {

// ============================================================
// 当前状态：接口定义完成，实现待第 10 周
//
// 以下为骨架代码 + 预期逻辑的 TODO 注释。
// ============================================================

SessionManager::SessionManager(size_t max_sessions,
                                 int64_t heartbeat_timeout_ms,
                                 int64_t grace_period_ms)
    : max_sessions_(max_sessions)
    , heartbeat_timeout_ms_(heartbeat_timeout_ms)
    , grace_period_ms_(grace_period_ms) {
}

// ---- 待实现 ----

std::string SessionManager::GenerateSessionId() {
    // TODO: 实现 UUID 生成
    //   格式: "sess_" + 递增计数器（简化版）
    //   生产环境建议: std::random_device + hex 编码
    std::ostringstream oss;
    oss << "sess_" << std::setfill('0') << std::setw(8) << id_counter_++;
    return oss.str();
}

std::string SessionManager::CreateSession(const std::string& player_id) {
    // TODO: 实现 CreateSession（第 10 周）
    //
    // 预期逻辑:
    // 1. 检查 max_sessions_（超过则拒绝，返回空串）
    // 2. 同一 player_id 已存在 session:
    //    - 如果旧 session 是 DISCONNECTED → 复用: 恢复 ACTIVE + 重置心跳
    //    - 如果旧 session 是 ACTIVE → 覆盖（踢旧登新）
    //    - 如果旧 session 是 EXPIRED → 删除旧 + 创建新
    // 3. 生成 session_id → 存入 sessions_ map
    // 4. 建立 player_to_session_[player_id] = session_id
    // 5. 返回 session_id

    (void)player_id;
    return "";  // 占位
}

std::string SessionManager::ValidateSession(const std::string& session_id) {
    // TODO: 实现 ValidateSession（第 10 周）
    //
    // 预期逻辑:
    // 1. sessions_.find(session_id)
    // 2. 不存在 → 返回空串
    // 3. state == EXPIRED → 返回空串
    // 4. state == DISCONNECTED:
    //    - 恢复为 ACTIVE
    //    - last_heartbeat_ms = now()
    //    - 返回 player_id
    // 5. state == ACTIVE:
    //    - 更新 last_heartbeat_ms = now()
    //    - 返回 player_id

    (void)session_id;
    return "";  // 占位
}

bool SessionManager::Heartbeat(const std::string& session_id) {
    // TODO: 实现 Heartbeat（第 10 周）
    //
    // 预期逻辑:
    // 1. sessions_.find(session_id)
    // 2. 不存在 → 返回 false
    // 3. state == EXPIRED → 返回 false
    // 4. 更新 last_heartbeat_ms = now()
    // 5. 如果 state == DISCONNECTED → 恢复为 ACTIVE（心跳期间重连）
    // 6. 返回 true

    (void)session_id;
    return false;  // 占位
}

void SessionManager::DestroySession(const std::string& session_id) {
    // TODO: 实现 DestroySession（第 10 周）
    //
    // 预期逻辑:
    // 1. sessions_.find(session_id)
    // 2. 不存在 → return
    // 3. player_to_session_.erase(session.player_id)
    // 4. sessions_.erase(it)

    (void)session_id;
    // 占位：不执行任何操作
}

SessionManager::TickResult SessionManager::Tick() {
    // TODO: 实现 Tick（第 10 周）
    //
    // 预期逻辑（详见 docs/reconnect-design.md 第六章）:
    // 1. 遍历 sessions_
    // 2. ACTIVE 且 heartbeat 超时 → DISCONNECTED，记录 disconnect_time
    // 3. DISCONNECTED 且超过 grace_period → EXPIRED
    // 4. 对每个 EXPIRED session:
    //    a. 外部回调通知：LeaveRoom + CancelMatch
    //    b. DestroySession(session_id)
    // 5. 返回 TickResult{disconnected_count, expired_count}

    return {0, 0};  // 占位
}

size_t SessionManager::ActiveCount() const {
    size_t count = 0;
    for (auto& [id, s] : sessions_) {
        if (s.state == SessionState::ACTIVE) count++;
    }
    return count;
}

size_t SessionManager::DisconnectedCount() const {
    size_t count = 0;
    for (auto& [id, s] : sessions_) {
        if (s.state == SessionState::DISCONNECTED) count++;
    }
    return count;
}

size_t SessionManager::TotalCount() const {
    return sessions_.size();
}

} // namespace game
