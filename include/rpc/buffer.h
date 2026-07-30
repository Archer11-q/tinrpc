#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>

namespace rpc {

// ============================================================
/// @brief Buffer — 接收缓冲区，负责从 TCP 字节流中切分出完整帧
///
/// 核心职责：解决 TCP 粘包/拆包问题
/// - 粘包：一次 recv 收到多帧 → TryPopFrame 逐帧弹出
/// - 拆包：一帧分多次 recv 到达 → 累积到够才弹出
///
/// Buffer 只识别帧边界（魔数 + 总长度），不解析帧内容。
/// 帧内容的解析由 ProtocolFrame::Decode 负责。
// ============================================================
class Buffer {
public:
    Buffer() = default;

    /// @brief 追加原始字节（通常来自 socket recv）
    /// @param data 原始字节指针
    /// @param len  字节长度
    void Append(const uint8_t* data, size_t len);

    /// @brief 尝试弹出一个完整帧的原始字节
    /// @return 完整帧字节；缓冲区不足一帧 → nullopt（调用方继续 recv）；
    ///         魔数错误/长度异常 → nullopt（调用方关闭连接）
    std::optional<std::vector<uint8_t>> TryPopFrame();

    // 当前缓冲区中未处理的字节数
    size_t Size() const {
        return buf_.size();
    }

private:
    std::vector<uint8_t> buf_;
};

} // namespace rpc
