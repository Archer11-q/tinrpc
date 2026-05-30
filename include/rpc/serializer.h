#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <optional>
#include "common.h"

namespace rpc {

// ============================================================
// Serializer — TLV 二进制序列化/反序列化
//
// 写入（序列化）：Writer 模式，默认构造
//   Serializer ser;
//   ser.WriteInt32(3);
//   ser.WriteString("hello");
//   auto bytes = ser.GetBuffer();  // 得到编码后的字节序列
//
// 读取（反序列化）：Reader 模式，用字节序列构造
//   Serializer ser(bytes);
//   auto a = ser.ReadInt32();       // optional<int32_t>
//   auto b = ser.ReadString();      // optional<string>
//   if (!a || !b) { /* 数据损坏 */ }
// ============================================================
class Serializer {
public:
    // ---- Writer 模式 ----
    Serializer() = default;

    void WriteInt32(int32_t value);
    void WriteInt64(int64_t value);
    void WriteFloat(float value);
    void WriteDouble(double value);
    void WriteString(const std::string& value);
    void WriteBool(bool value);

    // 获取已编码的字节数据（只读引用，避免拷贝）
    const std::vector<uint8_t>& GetBuffer() const { return buffer_; }

    // ---- Reader 模式 ----
    explicit Serializer(const std::vector<uint8_t>& data);

    std::optional<int32_t>     ReadInt32();
    std::optional<int64_t>     ReadInt64();
    std::optional<float>       ReadFloat();
    std::optional<double>      ReadDouble();
    std::optional<std::string> ReadString();
    std::optional<bool>        ReadBool();

private:
    // ---- 写入辅助 ----
    void WriteType(ValueType type);                         // 获取 1 字节 Type 标记
    void WriteLength(uint32_t length);                      // 获取 4 字节 Length（网络序）
    void WriteRawBytes(const uint8_t* data, size_t len);    // 追加原始字节到 buffer_

    // ---- 读取辅助 ----
    // 检查剩余可读字节数是否 >= need，不够返回 false
    bool CanRead(size_t need) const;
    // 读原始字节到 dest（不检查类型，用于复制 Value 部分）
    bool ReadRawBytes(uint8_t* dest, size_t len);

    std::vector<uint8_t> buffer_;  // 数据缓冲区
    size_t read_pos_ = 0;          // 当前读位置（仅 Reader 模式使用）
};

} // namespace rpc
