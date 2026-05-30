#include "rpc/serializer.h"
#include "rpc/common.h"

#include <cstring>
#include <stdexcept>

namespace rpc {

// ============================================================
// 内部辅助
// ============================================================

void Serializer::WriteType(ValueType type) {
    buffer_.push_back(static_cast<uint8_t>(type));
}

void Serializer::WriteLength(uint32_t length) {
    uint32_t net = HostToNetwork32(length); // 将主机顺序length转化为网络字节序
    const auto* bytes = reinterpret_cast<const uint8_t*>(&net); // 转成字节指针
    buffer_.insert(buffer_.end(), bytes, bytes + sizeof(net));  // 追加写入缓冲区
}

void Serializer::WriteRawBytes(const uint8_t* data, size_t len) {
    buffer_.insert(buffer_.end(), data, data + len);
}

bool Serializer::CanRead(size_t need) const {
    return read_pos_ + need <= buffer_.size();
}

bool Serializer::ReadRawBytes(uint8_t* dest, size_t len) {
    if (!CanRead(len)) return false;
    std::memcpy(dest, buffer_.data() + read_pos_, len);
    read_pos_ += len;
    return true;
}

// ============================================================
// Writer 实现
// ============================================================

void Serializer::WriteInt32(int32_t value) {
    WriteType(ValueType::Int32);
    WriteLength(4);                 // 将 4 转化为网络序
    uint32_t net = HostToNetwork32(static_cast<uint32_t>(value));   //将要发送的数据value转化为网络序
    WriteRawBytes(reinterpret_cast<const uint8_t*>(&net), 4);   // 强转字节流追加写入缓冲区
}

void Serializer::WriteInt64(int64_t value) {
    WriteType(ValueType::Int64);
    WriteLength(8);
    uint64_t net = HostToNetwork64(static_cast<uint64_t>(value));
    WriteRawBytes(reinterpret_cast<const uint8_t*>(&net), 8);
}

void Serializer::WriteFloat(float value) {
    WriteType(ValueType::Float);
    WriteLength(4);
    // 把 float 的二进制表示当作 uint32_t 做字节序转换
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint32_t net = HostToNetwork32(bits);
    WriteRawBytes(reinterpret_cast<const uint8_t*>(&net), 4);
}

void Serializer::WriteDouble(double value) {
    WriteType(ValueType::Double);
    WriteLength(8);
    uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    uint64_t net = HostToNetwork64(bits);
    WriteRawBytes(reinterpret_cast<const uint8_t*>(&net), 8);
}

void Serializer::WriteString(const std::string& value) {
    WriteType(ValueType::String);
    WriteLength(static_cast<uint32_t>(value.size()));
    WriteRawBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

void Serializer::WriteBool(bool value) {
    WriteType(ValueType::Bool);
    WriteLength(1);
    uint8_t byte = value ? 1 : 0;
    buffer_.push_back(byte);
}

// ============================================================
// Reader 实现
// ============================================================

Serializer::Serializer(const std::vector<uint8_t>& data)
    : buffer_(data), read_pos_(0) {}

std::optional<int32_t> Serializer::ReadInt32() {
    // 1. 读 Type
    if (!CanRead(1)) return std::nullopt;
    uint8_t type_byte = buffer_[read_pos_];
    if (type_byte != static_cast<uint8_t>(ValueType::Int32)) return std::nullopt;
    read_pos_++;

    // 2. 读 Length
    if (!CanRead(4)) return std::nullopt;
    uint32_t length;
    std::memcpy(&length, buffer_.data() + read_pos_, 4);
    read_pos_ += 4;
    length = NetworkToHost32(length);
    if (length != 4) return std::nullopt; // 长度不对，数据损坏

    // 3. 读 Value
    if (!CanRead(4)) return std::nullopt;
    uint32_t net;
    std::memcpy(&net, buffer_.data() + read_pos_, 4);
    read_pos_ += 4;
    return static_cast<int32_t>(NetworkToHost32(net));
}

std::optional<int64_t> Serializer::ReadInt64() {
    if (!CanRead(1)) return std::nullopt;
    if (buffer_[read_pos_] != static_cast<uint8_t>(ValueType::Int64)) return std::nullopt;
    read_pos_++;

    if (!CanRead(4)) return std::nullopt;
    uint32_t length;
    std::memcpy(&length, buffer_.data() + read_pos_, 4);
    read_pos_ += 4;
    if (NetworkToHost32(length) != 8) return std::nullopt;

    if (!CanRead(8)) return std::nullopt;
    uint64_t net;
    std::memcpy(&net, buffer_.data() + read_pos_, 8);
    read_pos_ += 8;
    return static_cast<int64_t>(NetworkToHost64(net));
}

std::optional<float> Serializer::ReadFloat() {
    if (!CanRead(1)) return std::nullopt;
    if (buffer_[read_pos_] != static_cast<uint8_t>(ValueType::Float)) return std::nullopt;
    read_pos_++;

    if (!CanRead(4)) return std::nullopt;
    uint32_t length;
    std::memcpy(&length, buffer_.data() + read_pos_, 4);
    read_pos_ += 4;
    if (NetworkToHost32(length) != 4) return std::nullopt;

    if (!CanRead(4)) return std::nullopt;
    uint32_t net;
    std::memcpy(&net, buffer_.data() + read_pos_, 4);
    read_pos_ += 4;
    uint32_t host = NetworkToHost32(net);
    float result;
    std::memcpy(&result, &host, sizeof(result));
    return result;
}

std::optional<double> Serializer::ReadDouble() {
    if (!CanRead(1)) return std::nullopt;
    if (buffer_[read_pos_] != static_cast<uint8_t>(ValueType::Double)) return std::nullopt;
    read_pos_++;

    if (!CanRead(4)) return std::nullopt;
    uint32_t length;
    std::memcpy(&length, buffer_.data() + read_pos_, 4);
    read_pos_ += 4;
    if (NetworkToHost32(length) != 8) return std::nullopt;

    if (!CanRead(8)) return std::nullopt;
    uint64_t net;
    std::memcpy(&net, buffer_.data() + read_pos_, 8);
    read_pos_ += 8;
    uint64_t host = NetworkToHost64(net);
    double result;
    std::memcpy(&result, &host, sizeof(result));
    return result;
}

std::optional<std::string> Serializer::ReadString() {
    if (!CanRead(1)) return std::nullopt;
    if (buffer_[read_pos_] != static_cast<uint8_t>(ValueType::String)) return std::nullopt;
    read_pos_++;

    if (!CanRead(4)) return std::nullopt;
    uint32_t length;
    std::memcpy(&length, buffer_.data() + read_pos_, 4);
    read_pos_ += 4;
    length = NetworkToHost32(length);

    if (!CanRead(length)) return std::nullopt;
    std::string result(reinterpret_cast<const char*>(buffer_.data() + read_pos_), length);
    read_pos_ += length;
    return result;
}

std::optional<bool> Serializer::ReadBool() {
    if (!CanRead(1)) return std::nullopt;
    if (buffer_[read_pos_] != static_cast<uint8_t>(ValueType::Bool)) return std::nullopt;
    read_pos_++;

    if (!CanRead(4)) return std::nullopt;
    uint32_t length;
    std::memcpy(&length, buffer_.data() + read_pos_, 4);
    read_pos_ += 4;
    if (NetworkToHost32(length) != 1) return std::nullopt;

    if (!CanRead(1)) return std::nullopt;
    bool result = (buffer_[read_pos_] != 0);
    read_pos_++;
    return result;
}

} // namespace rpc
