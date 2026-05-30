#pragma once

#include <cstdint>  // 定义固定宽度整数类型，如 uint8_t、uint32_t、uint64_t
#include <cstddef>

namespace rpc {

// ============================================================
// 值类型标记 — TLV 编码中的 T（Type）
// 接收方根据 Type 决定如何解释后续字节
// ============================================================
enum class ValueType : uint8_t {    // 强转无符号单字节，避免隐式类型转换
    Int32  = 0x01,
    Int64  = 0x02,
    Float  = 0x03,
    Double = 0x04,
    String = 0x05,
    Bool   = 0x06,
};

// ============================================================
// 字节序转换 — 主机序 ↔ 网络序（大端）
// 用位运算实现，零依赖，跨 Windows/Linux 一致
// ============================================================
// 处理 4 字节：小端 --> 大端
inline uint32_t HostToNetwork32(uint32_t host32) {
    return ((host32 & 0x000000FF) << 24) |  // 取最低分字节，移到最高位
           ((host32 & 0x0000FF00) << 8)  |
           ((host32 & 0x00FF0000) >> 8)  |
           ((host32 & 0xFF000000) >> 24);
}

// 大端 --> 小端
inline uint32_t NetworkToHost32(uint32_t net32) {
    // 大端转小端和反过来是完全相同的位运算
    return HostToNetwork32(net32);
}

// 处理8字节：小端 -->  大端
inline uint64_t HostToNetwork64(uint64_t host64) {
    return ((host64 & 0x00000000000000FFULL) << 56) |
           ((host64 & 0x000000000000FF00ULL) << 40) |
           ((host64 & 0x0000000000FF0000ULL) << 24) |
           ((host64 & 0x00000000FF000000ULL) << 8)  |
           ((host64 & 0x000000FF00000000ULL) >> 8)  |
           ((host64 & 0x0000FF0000000000ULL) >> 24) |
           ((host64 & 0x00FF000000000000ULL) >> 40) |
           ((host64 & 0xFF00000000000000ULL) >> 56);
}

// 大端  --> 小端
inline uint64_t NetworkToHost64(uint64_t net64) {
    return HostToNetwork64(net64);
}

} // namespace rpc
