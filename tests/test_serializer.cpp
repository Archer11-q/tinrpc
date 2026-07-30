#include "rpc/serializer.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <vector>
#include <string>
#include <cmath>

// ============================================================
// 简易测试框架 — 零依赖，纯手工
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

void RunTest(const char* name, void (*fn)()) {
    printf("  %-40s ... ", name);
    try {
        fn();
        printf("[PASS]\n");
        g_passed++;
    } catch (const std::exception& e) {
        printf("[FAIL] exception: %s\n", e.what());
        g_failed++;
    } catch (...) {
        printf("[FAIL] unknown exception\n");
        g_failed++;
    }
}

// ============================================================
// 测试用例
// ============================================================

void TestWriteReadInt32() {
    rpc::Serializer ser;
    ser.WriteInt32(0);
    ser.WriteInt32(42);
    ser.WriteInt32(-1);
    ser.WriteInt32(2147483647); // INT32_MAX

    rpc::Serializer reader(ser.GetBuffer());
    auto v0 = reader.ReadInt32();
    auto v1 = reader.ReadInt32();
    auto v2 = reader.ReadInt32();
    auto v3 = reader.ReadInt32();

    assert(v0.has_value() && *v0 == 0);
    assert(v1.has_value() && *v1 == 42);
    assert(v2.has_value() && *v2 == -1);
    assert(v3.has_value() && *v3 == 2147483647);
}

void TestWriteReadInt64() {
    rpc::Serializer ser;
    ser.WriteInt64(0);
    ser.WriteInt64(9223372036854775807LL); // INT64_MAX

    rpc::Serializer reader(ser.GetBuffer());
    auto v0 = reader.ReadInt64();
    auto v1 = reader.ReadInt64();

    assert(v0.has_value() && *v0 == 0);
    assert(v1.has_value() && *v1 == 9223372036854775807LL);
}

void TestWriteReadFloat() {
    rpc::Serializer ser;
    ser.WriteFloat(0.0f);
    ser.WriteFloat(3.14f);
    ser.WriteFloat(-2.718f);

    rpc::Serializer reader(ser.GetBuffer());
    auto v0 = reader.ReadFloat();
    auto v1 = reader.ReadFloat();
    auto v2 = reader.ReadFloat();

    assert(v0.has_value() && std::fabs(*v0 - 0.0f) < 0.0001f);
    assert(v1.has_value() && std::fabs(*v1 - 3.14f) < 0.0001f);
    assert(v2.has_value() && std::fabs(*v2 - (-2.718f)) < 0.0001f);
}

void TestWriteReadDouble() {
    rpc::Serializer ser;
    ser.WriteDouble(0.0);
    ser.WriteDouble(3.141592653589793);
    ser.WriteDouble(-1.4142135623730951);

    rpc::Serializer reader(ser.GetBuffer());
    auto v0 = reader.ReadDouble();
    auto v1 = reader.ReadDouble();
    auto v2 = reader.ReadDouble();

    assert(v0.has_value() && std::fabs(*v0 - 0.0) < 1e-9);
    assert(v1.has_value() && std::fabs(*v1 - 3.141592653589793) < 1e-9);
    assert(v2.has_value() && std::fabs(*v2 - (-1.4142135623730951)) < 1e-9);
}

void TestWriteReadString() {
    rpc::Serializer ser;
    ser.WriteString("");
    ser.WriteString("hello");
    ser.WriteString("RPC framework");

    rpc::Serializer reader(ser.GetBuffer());
    auto v0 = reader.ReadString();
    auto v1 = reader.ReadString();
    auto v2 = reader.ReadString();

    assert(v0.has_value() && *v0 == "");
    assert(v1.has_value() && *v1 == "hello");
    assert(v2.has_value() && *v2 == "RPC framework");
}

void TestWriteReadBool() {
    rpc::Serializer ser;
    ser.WriteBool(true);
    ser.WriteBool(false);
    ser.WriteBool(true);

    rpc::Serializer reader(ser.GetBuffer());
    auto v0 = reader.ReadBool();
    auto v1 = reader.ReadBool();
    auto v2 = reader.ReadBool();

    assert(v0.has_value() && *v0 == true);
    assert(v1.has_value() && *v1 == false);
    assert(v2.has_value() && *v2 == true);
}

void TestMultipleTypes() {
    // 混合多种类型写入，再按序读出
    rpc::Serializer ser;
    ser.WriteInt32(100);
    ser.WriteString("test");
    ser.WriteBool(true);
    ser.WriteDouble(3.14);

    rpc::Serializer reader(ser.GetBuffer());
    auto i = reader.ReadInt32();
    auto s = reader.ReadString();
    auto b = reader.ReadBool();
    auto d = reader.ReadDouble();

    assert(i.has_value() && *i == 100);
    assert(s.has_value() && *s == "test");
    assert(b.has_value() && *b == true);
    assert(d.has_value() && std::fabs(*d - 3.14) < 1e-9);
}

void TestTypeMismatch() {
    // 写入 Int32，尝试读成 String —— 必须返回 nullopt
    rpc::Serializer ser;
    ser.WriteInt32(42);

    rpc::Serializer reader(ser.GetBuffer());
    auto s = reader.ReadString();
    assert(!s.has_value()); // Type 不匹配
}

void TestReadPastEnd() {
    // 构造一个截断的 buffer（int32 需要 9 字节，只给 3 字节）
    std::vector<uint8_t> broken = {0x01, 0x00, 0x00}; // 缺 6 字节
    rpc::Serializer reader(broken);
    auto v = reader.ReadInt32();
    assert(!v.has_value()); // 越界读取必须返回 nullopt
}

void TestEmptyBuffer() {
    std::vector<uint8_t> empty;
    rpc::Serializer reader(empty);
    auto v = reader.ReadInt32();
    assert(!v.has_value());
}

void TestLengthMismatch() {
    // 手工构造一个 Length 声明为 8 但实际 value 只有 4 字节的数据
    // Frame: Type=int32(0x01) | Length=8(0x00000008) | Value=0x00000000(4 bytes)
    // 注意：值字段是大端的
    std::vector<uint8_t> malicious = {
        0x01, // Type = Int32
        0x00, 0x00, 0x00, 0x08, // Length = 8 (声称 8 字节！)
        0x00, 0x00, 0x00, 0x2A // Value = 42 但只有 4 字节
    };
    rpc::Serializer reader(malicious);
    // 读 Type 和 Length 成功，读 Value 时 Length 声明了 8 字节但 Value 实际只有 4 字节在 buffer 里
    // 但我们期望 Int32 的 Length 就是 4，所以这里会返回 nullopt
    auto v = reader.ReadInt32();
    assert(!v.has_value()); // 长度不匹配 4
}

// ============================================================
// 入口
// ============================================================

int main() {
    printf("=== Serializer Unit Tests ===\n\n");

    RunTest("TestWriteReadInt32", TestWriteReadInt32);
    RunTest("TestWriteReadInt64", TestWriteReadInt64);
    RunTest("TestWriteReadFloat", TestWriteReadFloat);
    RunTest("TestWriteReadDouble", TestWriteReadDouble);
    RunTest("TestWriteReadString", TestWriteReadString);
    RunTest("TestWriteReadBool", TestWriteReadBool);
    RunTest("TestMultipleTypes", TestMultipleTypes);
    RunTest("TestTypeMismatch", TestTypeMismatch);
    RunTest("TestReadPastEnd", TestReadPastEnd);
    RunTest("TestEmptyBuffer", TestEmptyBuffer);
    RunTest("TestLengthMismatch", TestLengthMismatch);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
