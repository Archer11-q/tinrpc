#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include "common.h"

namespace rpc {

// ============================================================
/// @brief Frame — 解码后的帧结构体（值类型）
// ============================================================
struct Frame {
    uint32_t request_id;
    MessageType msg_type;
    std::string method_name;
    std::vector<uint8_t> body; // 序列化后的参数或返回值
};

// ============================================================
/// @brief ProtocolFrame — 帧编码/解码（无状态，纯静态方法）
///
/// Encode: Frame 字段 → 完整帧字节序列
/// Decode: 完整帧字节序列 → Frame（失败返回 nullopt）
///
/// 帧格式：
/// ┌─────────┬──────────┬───────────┬───────────┬──────────┬──────────┬──────────┐
/// │ 魔数     │ 总长度    │ 请求 ID    │ 消息类型    │ 方法名长度 │ 方法名     │ body     │
/// │ 2 bytes  │ 4 bytes   │ 4 bytes    │ 1 byte     │ 2 bytes   │ N bytes   │ M bytes  │
/// └─────────┴──────────┴───────────┴───────────┴──────────┴──────────┴──────────┘
// ============================================================
class ProtocolFrame {
public:
    /// @brief 编码：各字段 → 字节序列
    /// @param request_id 请求 ID
    /// @param msg_type 消息类型
    /// @param method_name 方法名
    /// @param body 请求/响应 body 字节
    /// @return 完整帧字节序列
    static std::vector<uint8_t> Encode(uint32_t request_id, MessageType msg_type,
                                       const std::string& method_name,
                                       const std::vector<uint8_t>& body);

    /// @brief 解码：字节序列 → Frame
    /// @param raw_frame 一帧的完整字节（由 Buffer 负责切分后传入）
    /// @return 解码后的 Frame；失败返回 nullopt
    static std::optional<Frame> Decode(const std::vector<uint8_t>& raw_frame);
};

} // namespace rpc
