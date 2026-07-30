#include "rpc/protocol.h"
#include "rpc/common.h"

#include <cstring>

namespace rpc {

std::vector<uint8_t> ProtocolFrame::Encode(uint32_t request_id, // 请求ID
                                           MessageType msg_type, // 消息类型
                                           const std::string& method_name, // 方法名
                                           const std::vector<uint8_t>& body) // 序列化后的参数或返回值
{
    uint32_t total_len = kFrameHeaderSize + method_name.size() +
                         body.size(); // 帧总长度 = 固定头 + 方法名 + body
    std::vector<uint8_t> frame; // 帧字节序列框架： 魔数 + 总长度 + 请求ID + 消息类型 + 方法名长度 + 方法名 + body
    frame.reserve(total_len);

    // 1. 魔数（2 字节，大端）
    uint16_t magic_net = HostToNetwork16(kProtocolMagic);
    const auto* magic_bytes = reinterpret_cast<const uint8_t*>(&magic_net);
    frame.insert(frame.end(), magic_bytes, magic_bytes + 2);

    // 2. 总长度（4 字节，大端）
    uint32_t total_net = HostToNetwork32(total_len);
    const auto* total_bytes = reinterpret_cast<const uint8_t*>(&total_net);
    frame.insert(frame.end(), total_bytes, total_bytes + 4);

    // 3. 请求 ID（4 字节，大端）
    uint32_t rid_net = HostToNetwork32(request_id);
    const auto* rid_bytes = reinterpret_cast<const uint8_t*>(&rid_net);
    frame.insert(frame.end(), rid_bytes, rid_bytes + 4);

    // 4. 消息类型（1 字节）
    frame.push_back(static_cast<uint8_t>(msg_type));

    // 5. 方法名长度（2 字节，大端）
    uint16_t mname_len = static_cast<uint16_t>(method_name.size());
    uint16_t mname_len_net = HostToNetwork16(mname_len);
    const auto* mname_len_bytes = reinterpret_cast<const uint8_t*>(&mname_len_net);
    frame.insert(frame.end(), mname_len_bytes, mname_len_bytes + 2);

    // 6. 方法名（N 字节）
    frame.insert(frame.end(), method_name.begin(), method_name.end());

    // 7. body（M 字节）
    frame.insert(frame.end(), body.begin(), body.end());

    return frame;
}

std::optional<Frame> ProtocolFrame::Decode(const std::vector<uint8_t>& raw_frame) {
    size_t size = raw_frame.size(); // 帧字节序列框架实际总大小

    // 1. 长度至少为帧头大小
    if (size < kFrameHeaderSize)
        return std::nullopt;

    size_t pos = 0;

    // 2. 魔数
    uint16_t magic;
    std::memcpy(&magic, raw_frame.data() + pos, 2);
    pos += 2;
    if (NetworkToHost16(magic) != kProtocolMagic)
        return std::nullopt;

    // 3. 总长度
    uint32_t total_len;
    std::memcpy(&total_len, raw_frame.data() + pos, 4);
    pos += 4;
    total_len = NetworkToHost32(total_len);
    if (total_len < kFrameHeaderSize || total_len > kMaxFrameSize)
        return std::nullopt;
    if (total_len != size)
        return std::nullopt; // 声称的长度与实际不符

    // 4. 请求 ID
    uint32_t request_id;
    std::memcpy(&request_id, raw_frame.data() + pos, 4);
    pos += 4;
    request_id = NetworkToHost32(request_id);

    // 5. 消息类型
    uint8_t msg_byte = raw_frame[pos];
    pos += 1;
    if (msg_byte != static_cast<uint8_t>(MessageType::Request) &&
        msg_byte != static_cast<uint8_t>(MessageType::Response) &&
        msg_byte != static_cast<uint8_t>(MessageType::Error)) {
        return std::nullopt;
    }
    MessageType msg_type = static_cast<MessageType>(msg_byte);

    // 6. 方法名长度
    uint16_t mname_len;
    std::memcpy(&mname_len, raw_frame.data() + pos, 2);
    pos += 2;
    mname_len = NetworkToHost16(mname_len);

    // 方法名长度不能超过剩余字节数
    size_t remaining = size - pos;
    if (mname_len > remaining)
        return std::nullopt;

    // 7. 方法名
    std::string method_name(reinterpret_cast<const char*>(raw_frame.data() + pos), mname_len);
    pos += mname_len;

    // 8. body（剩余全部字节）
    std::vector<uint8_t> body(raw_frame.begin() + pos, raw_frame.end());

    return Frame{request_id, msg_type, std::move(method_name), std::move(body)};
}

} // namespace rpc
