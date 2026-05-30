#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>

namespace rpc {

// ============================================================
// Buffer — 接收缓冲区，负责从 TCP 字节流中切分出完整帧
//
// 核心职责：解决 TCP 粘包/拆包问题
// - 粘包：一次 recv 收到多帧 → TryPopFrame 逐帧弹出
// - 拆包：一帧分多次 recv 到达 → 累积到够才弹出
//
// Buffer 只识别帧边界（魔数 + 总长度），不解析帧内容。
// 帧内容的解析由 ProtocolFrame::Decode 负责。
// ============================================================
class Buffer {
public:
    Buffer() = default;

    // 追加原始字节（通常来自 socket recv）
    void Append(const uint8_t* data, size_t len);

    // 尝试弹出一个完整帧的原始字节
    // 缓冲区不足一帧 → 返回 nullopt（调用方继续 recv）
    // 魔数错误/长度异常  → 返回 nullopt（调用方关闭连接）
    std::optional<std::vector<uint8_t>> TryPopFrame();

    // 当前缓冲区中未处理的字节数
    size_t Size() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
};

} // namespace rpc
