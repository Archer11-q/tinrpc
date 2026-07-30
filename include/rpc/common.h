#pragma once

#include <cstdint> // 定义固定宽度整数类型，如 uint8_t、uint32_t、uint64_t
#include <cstddef>

namespace rpc {

/// @brief 值类型标记 — TLV 编码中的 T（Type）
/// @details 接收方根据 Type 决定如何解释后续字节
// ============================================================
enum class ValueType : uint8_t { // 强转无符号单字节，避免隐式类型转换
    Int32 = 0x01, ///< 32 位有符号整数
    Int64 = 0x02, ///< 64 位有符号整数
    Float = 0x03, ///< 32 位浮点数
    Double = 0x04, ///< 64 位浮点数
    String = 0x05, ///< UTF-8 字符串
    Bool = 0x06, ///< 布尔值
};

/// @brief 字节序转换 — 主机序 ↔ 网络序（大端）
/// @details 用位运算实现，零依赖，跨 Windows/Linux 一致
// ============================================================

/// @brief 主机序 → 网络序（32 位，小端 → 大端）
/// @param host32 主机字节序的 32 位整数
/// @return 网络字节序的 32 位整数
inline uint32_t HostToNetwork32(uint32_t host32) {
    return ((host32 & 0x000000FF) << 24) | // 取最低分字节，移到最高位
           ((host32 & 0x0000FF00) << 8) | ((host32 & 0x00FF0000) >> 8) |
           ((host32 & 0xFF000000) >> 24);
}

/// @brief 网络序 → 主机序（32 位，大端 → 小端）
/// @param net32 网络字节序的 32 位整数
/// @return 主机字节序的 32 位整数
inline uint32_t NetworkToHost32(uint32_t net32) {
    // 大端转小端和反过来是完全相同的位运算
    return HostToNetwork32(net32);
}

/// @brief 主机序 → 网络序（64 位，小端 → 大端）
/// @param host64 主机字节序的 64 位整数
/// @return 网络字节序的 64 位整数
inline uint64_t HostToNetwork64(uint64_t host64) {
    return ((host64 & 0x00000000000000FFULL) << 56) | ((host64 & 0x000000000000FF00ULL) << 40) |
           ((host64 & 0x0000000000FF0000ULL) << 24) | ((host64 & 0x00000000FF000000ULL) << 8) |
           ((host64 & 0x000000FF00000000ULL) >> 8) | ((host64 & 0x0000FF0000000000ULL) >> 24) |
           ((host64 & 0x00FF000000000000ULL) >> 40) | ((host64 & 0xFF00000000000000ULL) >> 56);
}

/// @brief 网络序 → 主机序（64 位，大端 → 小端）
/// @param net64 网络字节序的 64 位整数
/// @return 主机字节序的 64 位整数
inline uint64_t NetworkToHost64(uint64_t net64) {
    return HostToNetwork64(net64);
}

/// @brief 消息类型标记 — 协议帧层使用
// ============================================================
enum class MessageType : uint8_t {
    Request = 0x01, ///< 请求（客户端 → 服务端）
    Response = 0x02, ///< 正常响应（服务端 → 客户端）
    Error = 0x03, ///< 错误响应（服务端 → 客户端）
};

/// @brief 协议帧常量
// ============================================================
constexpr uint16_t kProtocolMagic = 0xBABE; ///< 帧魔数
constexpr size_t kFrameHeaderSize = 13; ///< 帧头固定字节数
constexpr size_t kMaxFrameSize = 10 * 1024 * 1024; ///< 单帧上限 10MB

/// @brief 2 字节字节序转换（用于方法名长度字段）
// ============================================================

/// @brief 主机序 → 网络序（16 位）
/// @param host16 主机字节序的 16 位整数
/// @return 网络字节序的 16 位整数
inline uint16_t HostToNetwork16(uint16_t host16) {
    return ((host16 & 0x00FF) << 8) | ((host16 & 0xFF00) >> 8);
}

/// @brief 网络序 → 主机序（16 位）
/// @param net16 网络字节序的 16 位整数
/// @return 主机字节序的 16 位整数
inline uint16_t NetworkToHost16(uint16_t net16) {
    return HostToNetwork16(net16);
}

} // namespace rpc
