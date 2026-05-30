#include "rpc/buffer.h"
#include "rpc/common.h"

#include <cstring>

namespace rpc {

void Buffer::Append(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
}

std::optional<std::vector<uint8_t>> Buffer::TryPopFrame() {
    // 至少需要 6 字节才能读魔数（2）+ 总长度（4）
    if (buf_.size() < 6) return std::nullopt;

    // 1. 校验魔数
    uint16_t magic;
    std::memcpy(&magic, buf_.data(), 2);
    if (NetworkToHost16(magic) != kProtocolMagic) {
        // 数据损坏（TCP 层面不应发生，说明对端协议不对）
        return std::nullopt;
    }

    // 2. 读取总长度
    uint32_t total_len;
    std::memcpy(&total_len, buf_.data() + 2, 4);
    total_len = NetworkToHost32(total_len);

    // 3. 合法性校验
    if (total_len < kFrameHeaderSize || total_len > kMaxFrameSize) {
        return std::nullopt;  // 数据异常
    }

    // 4. 缓冲区字节数不够一帧 → 等待更多数据
    if (buf_.size() < total_len) return std::nullopt;

    // 5. 切出一帧
    std::vector<uint8_t> frame(buf_.begin(), buf_.begin() + total_len);

    // 6. 消费已切出的字节
    buf_.erase(buf_.begin(), buf_.begin() + total_len);

    return frame;
}

} // namespace rpc
