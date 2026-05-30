#include "rpc/protocol.h"
#include "rpc/buffer.h"
#include "rpc/serializer.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <vector>
#include <string>

// ============================================================
// 简易测试框架
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
// 辅助：用 Serializer 生成一段合法的 body
// ============================================================
std::vector<uint8_t> MakeBody(int32_t a, int32_t b) {
    rpc::Serializer ser;
    ser.WriteInt32(a);
    ser.WriteInt32(b);
    return ser.GetBuffer();
}

// ============================================================
// ProtocolFrame 测试
// ============================================================

void TestEncodeDecodeRoundTrip() {
    auto body = MakeBody(3, 5);
    auto frame_bytes = rpc::ProtocolFrame::Encode(
        1, rpc::MessageType::Request, "Add", body);

    auto frame = rpc::ProtocolFrame::Decode(frame_bytes);
    assert(frame.has_value());
    assert(frame->request_id == 1);
    assert(frame->msg_type == rpc::MessageType::Request);
    assert(frame->method_name == "Add");
    assert(frame->body.size() == body.size());
    assert(std::memcmp(frame->body.data(), body.data(), body.size()) == 0);
}

void TestEncodeDecodeEmptyMethodName() {
    std::vector<uint8_t> empty_body;
    auto frame_bytes = rpc::ProtocolFrame::Encode(
        0, rpc::MessageType::Response, "", empty_body);

    auto frame = rpc::ProtocolFrame::Decode(frame_bytes);
    assert(frame.has_value());
    assert(frame->method_name == "");
    assert(frame->body.empty());
}

void TestEncodeDecodeLargeBody() {
    // 构造一个 1KB 的 body
    std::vector<uint8_t> large_body(1024, 0xAB);
    auto frame_bytes = rpc::ProtocolFrame::Encode(
        42, rpc::MessageType::Request, "BatchProcess", large_body);

    auto frame = rpc::ProtocolFrame::Decode(frame_bytes);
    assert(frame.has_value());
    assert(frame->request_id == 42);
    assert(frame->method_name == "BatchProcess");
    assert(frame->body.size() == 1024);
}

void TestDecodeInvalidMagic() {
    std::vector<uint8_t> corrupted = {
        0xBA, 0xBF,                                     // 错误魔数
        0x00, 0x00, 0x00, 0x0D,                         // 总长度 = 13
        0x00, 0x00, 0x00, 0x01,                         // req id = 1
        0x01,                                           // msg type
        0x00, 0x00                                      // 方法名长度 = 0
    };
    auto frame = rpc::ProtocolFrame::Decode(corrupted);
    assert(!frame.has_value());
}

void TestDecodeTooSmall() {
    std::vector<uint8_t> small = {0xBA, 0xBE, 0x00};  // 只有 3 字节
    auto frame = rpc::ProtocolFrame::Decode(small);
    assert(!frame.has_value());
}

void TestDecodeTotalLenTooSmall() {
    // total_len < 13
    std::vector<uint8_t> data = {
        0xBA, 0xBE,                                     // 魔数正确
        0x00, 0x00, 0x00, 0x0A,                         // 总长度 = 10（小于帧头 13）
        0x00, 0x00, 0x00, 0x01,                         // req id
        0x01,                                           // msg type
        0x00, 0x00                                      // 方法名长度
    };
    auto frame = rpc::ProtocolFrame::Decode(data);
    assert(!frame.has_value());
}

void TestDecodeLengthExceedsData() {
    // total_len 声称 100 但实际数据只有 20 字节
    std::vector<uint8_t> data(20, 0x00);
    data[0] = 0xBA; data[1] = 0xBE;
    uint32_t fake_len = rpc::HostToNetwork32(100);
    std::memcpy(&data[2], &fake_len, 4);

    auto frame = rpc::ProtocolFrame::Decode(data);
    assert(!frame.has_value());
}

void TestDecodeInvalidMessageType() {
    std::vector<uint8_t> data = {
        0xBA, 0xBE,
        0x00, 0x00, 0x00, 0x0D,                         // total_len = 13
        0x00, 0x00, 0x00, 0x01,
        0xFF,                                           // 非法消息类型
        0x00, 0x00
    };
    auto frame = rpc::ProtocolFrame::Decode(data);
    assert(!frame.has_value());
}

void TestDecodeMethodNameLenExceedsRemaining() {
    std::vector<uint8_t> data = {
        0xBA, 0xBE,
        0x00, 0x00, 0x00, 0x0F,                         // total_len = 15
        0x00, 0x00, 0x00, 0x01,
        0x01,                                           // Request
        0x00, 0x0A                                      // 方法名长度 = 10，但只剩 2 字节
    };
    auto frame = rpc::ProtocolFrame::Decode(data);
    assert(!frame.has_value());
}

// ============================================================
// Buffer 测试
// ============================================================

void TestBufferSingleCompleteFrame() {
    auto body = MakeBody(1, 2);
    auto frame_bytes = rpc::ProtocolFrame::Encode(
        1, rpc::MessageType::Request, "Add", body);

    rpc::Buffer buf;
    buf.Append(frame_bytes.data(), frame_bytes.size());

    auto popped = buf.TryPopFrame();
    assert(popped.has_value());
    assert(popped->size() == frame_bytes.size());
    assert(buf.Size() == 0);
}

void TestBufferStickyPackets() {
    // 两帧粘在一起到达
    auto body1 = MakeBody(1, 2);
    auto body2 = MakeBody(3, 4);
    auto f1 = rpc::ProtocolFrame::Encode(1, rpc::MessageType::Request, "Add", body1);
    auto f2 = rpc::ProtocolFrame::Encode(2, rpc::MessageType::Request, "Mul", body2);

    rpc::Buffer buf;
    buf.Append(f1.data(), f1.size());
    buf.Append(f2.data(), f2.size());  // 粘包

    auto p1 = buf.TryPopFrame();
    assert(p1.has_value());
    assert(p1->size() == f1.size());

    auto p2 = buf.TryPopFrame();
    assert(p2.has_value());
    assert(p2->size() == f2.size());

    assert(buf.Size() == 0);
}

void TestBufferSplitPacket() {
    // 一帧分三次到达
    auto body = MakeBody(100, 200);
    auto frame = rpc::ProtocolFrame::Encode(
        5, rpc::MessageType::Response, "Result", body);

    rpc::Buffer buf;

    // 第一次只给前 5 字节（不够读帧头）
    buf.Append(frame.data(), 5);
    auto p1 = buf.TryPopFrame();
    assert(!p1.has_value());

    // 第二次给到第 12 字节（帧头还差 1 字节）
    buf.Append(frame.data() + 5, 7);
    auto p2 = buf.TryPopFrame();
    assert(!p2.has_value());

    // 第三次给完剩余
    buf.Append(frame.data() + 12, frame.size() - 12);
    auto p3 = buf.TryPopFrame();
    assert(p3.has_value());
    assert(p3->size() == frame.size());
    assert(buf.Size() == 0);
}

void TestBufferStickyAndSplitCombo() {
    auto body1 = MakeBody(10, 20);
    auto body2 = MakeBody(30, 40);
    auto f1 = rpc::ProtocolFrame::Encode(1, rpc::MessageType::Request, "X", body1);
    auto f2 = rpc::ProtocolFrame::Encode(2, rpc::MessageType::Request, "Y", body2);

    rpc::Buffer buf;

    // 第一帧的前一半 + 第二帧的前一半（粘在一起，但两帧都不完整）
    // 先 Append f1 的前半部分
    size_t half1 = f1.size() / 2;
    buf.Append(f1.data(), half1);
    auto p1 = buf.TryPopFrame();
    assert(!p1.has_value());

    // 再 Append f1 的后半部分 + f2 的全部（粘包）
    buf.Append(f1.data() + half1, f1.size() - half1);
    buf.Append(f2.data(), f2.size());

    auto p2 = buf.TryPopFrame();
    assert(p2.has_value());
    assert(p2->size() == f1.size());

    auto p3 = buf.TryPopFrame();
    assert(p3.has_value());
    assert(p3->size() == f2.size());

    assert(buf.Size() == 0);
}

void TestBufferInvalidMagic() {
    std::vector<uint8_t> garbage = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    rpc::Buffer buf;
    buf.Append(garbage.data(), garbage.size());

    auto popped = buf.TryPopFrame();
    assert(!popped.has_value());  // 魔数不对，拒绝
}

void TestBufferEmpty() {
    rpc::Buffer buf;
    auto popped = buf.TryPopFrame();
    assert(!popped.has_value());
}

void TestBufferMultiplePops() {
    // 连续 Append 3 帧，逐帧弹出
    auto body = MakeBody(0, 0);
    rpc::Buffer buf;
    for (int i = 0; i < 3; i++) {
        auto f = rpc::ProtocolFrame::Encode(
            static_cast<uint32_t>(i), rpc::MessageType::Request,
            "Test", body);
        buf.Append(f.data(), f.size());
    }

    for (int i = 0; i < 3; i++) {
        auto popped = buf.TryPopFrame();
        assert(popped.has_value());
        // 解码验证 request_id 正确
        auto decoded = rpc::ProtocolFrame::Decode(*popped);
        assert(decoded.has_value());
        assert(decoded->request_id == static_cast<uint32_t>(i));
    }
    assert(buf.Size() == 0);
}

// ============================================================
// 入口
// ============================================================

int main() {
    printf("=== Protocol Frame Tests ===\n\n");

    // ProtocolFrame 编解码
    RunTest("TestEncodeDecodeRoundTrip",       TestEncodeDecodeRoundTrip);
    RunTest("TestEncodeDecodeEmptyMethodName", TestEncodeDecodeEmptyMethodName);
    RunTest("TestEncodeDecodeLargeBody",        TestEncodeDecodeLargeBody);
    RunTest("TestDecodeInvalidMagic",           TestDecodeInvalidMagic);
    RunTest("TestDecodeTooSmall",               TestDecodeTooSmall);
    RunTest("TestDecodeTotalLenTooSmall",       TestDecodeTotalLenTooSmall);
    RunTest("TestDecodeLengthExceedsData",      TestDecodeLengthExceedsData);
    RunTest("TestDecodeInvalidMessageType",     TestDecodeInvalidMessageType);
    RunTest("TestDecodeMethodNameLenExceedsRemaining", TestDecodeMethodNameLenExceedsRemaining);

    printf("\n--- Buffer Tests ---\n\n");

    // Buffer 粘包/拆包
    RunTest("TestBufferSingleCompleteFrame",    TestBufferSingleCompleteFrame);
    RunTest("TestBufferStickyPackets",           TestBufferStickyPackets);
    RunTest("TestBufferSplitPacket",             TestBufferSplitPacket);
    RunTest("TestBufferStickyAndSplitCombo",     TestBufferStickyAndSplitCombo);
    RunTest("TestBufferInvalidMagic",            TestBufferInvalidMagic);
    RunTest("TestBufferEmpty",                   TestBufferEmpty);
    RunTest("TestBufferMultiplePops",            TestBufferMultiplePops);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
