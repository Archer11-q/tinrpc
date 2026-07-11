// ============================================================
// test_proto_vs_tlv — TLV 与 Protobuf 序列化对比测试
//
// 同一份数据分别用 TLV Serializer 和 Protobuf 序列化，
// 对比体积和速度，为序列化方案选择提供数据依据。
// ============================================================

#include "rpc/serializer.h"

#include "game.pb.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <string>
#include <vector>
#include <chrono>
#include <cmath>

// ============================================================
// 简易测试框架
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

void RunTest(const char* name, void (*fn)()) {
    printf("  %-50s ... ", name);
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
// 计时工具
// ============================================================

using Clock = std::chrono::high_resolution_clock;
using ns = std::chrono::nanoseconds;

// 返回平均每次操作的纳秒数
template <typename F>
double Benchmark(int iterations, F&& fn) {
    auto start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        fn();
    }
    auto end = Clock::now();
    return std::chrono::duration<double, std::nano>(end - start).count() / iterations;
}

// ============================================================
// 辅助：构造测试数据
// ============================================================

// 构建一个 RoomInfo（2 个玩家），同时返回等价的 TLV 序列化数据
// 供对比测试使用

game::RoomInfo BuildRoomInfo() {
    game::RoomInfo room;
    room.set_room_id("room_001");
    room.set_player_count(2);
    room.set_room_state(game::ROOM_STATE_WAITING);

    auto* p1 = room.add_players();
    p1->set_player_id("p001");
    p1->set_player_name("张三");
    p1->set_rank(game::RANK_GOLD);

    auto* p2 = room.add_players();
    p2->set_player_id("p002");
    p2->set_player_name("李四");
    p2->set_rank(game::RANK_SILVER);

    return room;
}

// TLV 手动编码同样的 RoomInfo 数据
// TLV 不支持 enum/repeated/nested，需要手动摊平编码：
//   room_id(string) → player_count(int32) → room_state(int32)
//   → 玩家数量(int32) → player1_id(string) → player1_name(string)
//   → player1_rank(int32) → player2_id(string) → ...
std::vector<uint8_t> EncodeRoomWithTLV(const game::RoomInfo& room) {
    rpc::Serializer ser;
    ser.WriteString(room.room_id());
    ser.WriteInt32(room.player_count());
    ser.WriteInt32(static_cast<int32_t>(room.room_state()));

    // repeated 字段：先写数量，再逐个写
    ser.WriteInt32(room.players_size());
    for (int i = 0; i < room.players_size(); ++i) {
        const auto& p = room.players(i);
        ser.WriteString(p.player_id());
        ser.WriteString(p.player_name());
        ser.WriteInt32(static_cast<int32_t>(p.rank()));
    }
    return ser.GetBuffer();
}

// TLV 解码 — 按编码顺序读回，重建 RoomInfo
game::RoomInfo DecodeRoomWithTLV(const std::vector<uint8_t>& data) {
    rpc::Serializer reader(data);

    game::RoomInfo room;

    auto room_id = reader.ReadString();
    auto count   = reader.ReadInt32();
    auto state   = reader.ReadInt32();
    auto n_players = reader.ReadInt32();

    // 检查所有字段都读成功
    (void)room_id; (void)count; (void)state; (void)n_players;

    if (room_id) room.set_room_id(*room_id);
    if (count) room.set_player_count(*count);
    if (state) room.set_room_state(static_cast<game::RoomStatus>(*state));

    if (n_players) {
        for (int i = 0; i < *n_players; ++i) {
            auto pid  = reader.ReadString();
            auto pname = reader.ReadString();
            auto prank = reader.ReadInt32();

            auto* p = room.add_players();
            if (pid) p->set_player_id(*pid);
            if (pname) p->set_player_name(*pname);
            if (prank) p->set_rank(static_cast<game::PlayerRank>(*prank));
        }
    }
    return room;
}

// ============================================================
// 测试 1：简单类型往返正确性
// ============================================================

void TestEchoProtobufRoundtrip() {
    // Proto 往返
    game::EchoRequest echo;
    echo.set_int_val(42);
    echo.set_str_val("hello");
    echo.set_dbl_val(3.14);
    echo.set_bool_val(true);

    std::string buf;
    assert(echo.SerializeToString(&buf));

    game::EchoRequest echo2;
    assert(echo2.ParseFromString(buf));

    assert(echo2.int_val() == 42);
    assert(echo2.str_val() == "hello");
    assert(std::fabs(echo2.dbl_val() - 3.14) < 1e-9);
    assert(echo2.bool_val() == true);
}

void TestEchoTLVRoundtrip() {
    // TLV 往返（等价数据）
    rpc::Serializer ser;
    ser.WriteInt32(42);
    ser.WriteString("hello");
    ser.WriteDouble(3.14);
    ser.WriteBool(true);

    rpc::Serializer reader(ser.GetBuffer());
    auto iv = reader.ReadInt32();
    auto sv = reader.ReadString();
    auto dv = reader.ReadDouble();
    auto bv = reader.ReadBool();

    assert(iv.has_value() && *iv == 42);
    assert(sv.has_value() && *sv == "hello");
    assert(dv.has_value() && std::fabs(*dv - 3.14) < 1e-9);
    assert(bv.has_value() && *bv == true);
}

// ============================================================
// 测试 2：RoomInfo 往返正确性
// ============================================================

void TestRoomProtobufRoundtrip() {
    game::RoomInfo room = BuildRoomInfo();

    std::string buf;
    assert(room.SerializeToString(&buf));
    assert(!buf.empty());

    game::RoomInfo room2;
    assert(room2.ParseFromString(buf));

    assert(room2.room_id() == "room_001");
    assert(room2.player_count() == 2);
    assert(room2.room_state() == game::ROOM_STATE_WAITING);
    assert(room2.players_size() == 2);
    assert(room2.players(0).player_id() == "p001");
    assert(room2.players(0).player_name() == "张三");
    assert(room2.players(1).player_id() == "p002");
}

void TestRoomTLVRoundtrip() {
    game::RoomInfo room = BuildRoomInfo();
    auto tlv_buf = EncodeRoomWithTLV(room);

    game::RoomInfo room2 = DecodeRoomWithTLV(tlv_buf);

    assert(room2.room_id() == "room_001");
    assert(room2.player_count() == 2);
    assert(room2.room_state() == game::ROOM_STATE_WAITING);
    assert(room2.players_size() == 2);
    assert(room2.players(0).player_id() == "p001");
    assert(room2.players(1).player_id() == "p002");
}

// ============================================================
// 测试 3：对比 — 相同数据，两种序列化方案
// ============================================================

void TestCompareSimpleSizeAndSpeed() {
    const int ITER = 500000;    // 循环次数

    // ---- TLV ----
    auto tlv_data = []() -> std::vector<uint8_t> {
        rpc::Serializer ser;
        ser.WriteInt32(42);
        ser.WriteString("hello world");
        ser.WriteDouble(3.14159);
        ser.WriteBool(true);
        return ser.GetBuffer();
    };

    std::vector<uint8_t> tlv_buf = tlv_data();
    size_t tlv_size = tlv_buf.size();

    double tlv_encode_ns = Benchmark(ITER, [&]() {
        volatile auto v = tlv_data(); (void)v;
    });

    double tlv_decode_ns = Benchmark(ITER, [&]() {
        rpc::Serializer reader(tlv_buf);
        volatile auto a = reader.ReadInt32();
        volatile auto b = reader.ReadString();
        volatile auto c = reader.ReadDouble();
        volatile auto d = reader.ReadBool();
        (void)a; (void)b; (void)c; (void)d;
    });

    // ---- Protobuf ----
    auto proto_data = []() -> std::string {
        game::EchoRequest req;
        req.set_int_val(42);
        req.set_str_val("hello world");
        req.set_dbl_val(3.14159);
        req.set_bool_val(true);
        std::string buf;
        req.SerializeToString(&buf);
        return buf;
    };

    std::string proto_buf = proto_data();
    size_t proto_size = proto_buf.size();

    double proto_encode_ns = Benchmark(ITER, [&]() {
        volatile auto v = proto_data(); (void)v;
    });

    double proto_decode_ns = Benchmark(ITER, [&]() {
        game::EchoRequest req;
        req.ParseFromString(proto_buf);
        volatile auto a = req.int_val();
        volatile auto b = req.str_val();
        volatile auto c = req.dbl_val();
        volatile auto d = req.bool_val();
        (void)a; (void)b; (void)c; (void)d;
    });

    // ---- 输出对比 ----
    printf("\n");
    printf("  ========== 简单类型对比（int32 + string + double + bool）==========\n");
    printf("  %-20s %10s %14s %14s\n", "方案", "体积(B)", "编码(ns/次)", "解码(ns/次)");
    printf("  %-20s %10zu %14.1f %14.1f\n", "TLV", tlv_size, tlv_encode_ns, tlv_decode_ns);
    printf("  %-20s %10zu %14.1f %14.1f\n", "Protobuf", proto_size, proto_encode_ns, proto_decode_ns);

    double size_ratio = (double)proto_size / tlv_size * 100.0;
    printf("  %-20s %10s %14s %14s\n", "Proto/TLV", "", "", "");
    printf("  %-20s %9.1f%% %13.1f%% %13.1f%%\n",
           "", size_ratio,
           proto_encode_ns / tlv_encode_ns * 100.0,
           proto_decode_ns / tlv_decode_ns * 100.0);
    printf("\n");
}

void TestCompareRoomSizeAndSpeed() {
    const int ITER = 100000;    // 循环次数

    game::RoomInfo room = BuildRoomInfo();

    // ---- TLV ----
    std::vector<uint8_t> tlv_buf = EncodeRoomWithTLV(room);
    size_t tlv_size = tlv_buf.size();

    double tlv_encode_ns = Benchmark(ITER, [&]() {
        volatile auto v = EncodeRoomWithTLV(room); (void)v;
    });

    double tlv_decode_ns = Benchmark(ITER, [&]() {
        volatile auto v = DecodeRoomWithTLV(tlv_buf); (void)v;
    });

    // ---- Protobuf ----
    std::string proto_buf;
    room.SerializeToString(&proto_buf);
    size_t proto_size = proto_buf.size();

    double proto_encode_ns = Benchmark(ITER, [&]() {
        std::string buf;
        room.SerializeToString(&buf);
        volatile size_t s = buf.size(); (void)s;    // 避免SerializeToString被编译器优化掉
    });

    double proto_decode_ns = Benchmark(ITER, [&]() {
        game::RoomInfo r;
        r.ParseFromString(proto_buf);
        volatile auto s = r.room_id(); (void)s;
    });

    // ---- 输出对比 ----
    printf("\n");
    printf("  ========== RoomInfo 对比（string×5 + int×5 + repeated）==========\n");
    printf("  %-20s %10s %14s %14s\n", "方案", "体积(B)", "编码(ns/次)", "解码(ns/次)");
    printf("  %-20s %10zu %14.1f %14.1f\n", "TLV", tlv_size, tlv_encode_ns, tlv_decode_ns);
    printf("  %-20s %10zu %14.1f %14.1f\n", "Protobuf", proto_size, proto_encode_ns, proto_decode_ns);

    double size_ratio = (double)proto_size / tlv_size * 100.0;
    printf("  %-20s %10s %14s %14s\n", "Proto/TLV", "", "", "");
    printf("  %-20s %9.1f%% %13.1f%% %13.1f%%\n",
           "", size_ratio,
           proto_encode_ns / tlv_encode_ns * 100.0,
           proto_decode_ns / tlv_decode_ns * 100.0);
    printf("\n");
}

// ============================================================
// 入口
// ============================================================

int main() {
    printf("=== TLV vs Protobuf 序列化对比测试 ===\n\n");

    // 正确性测试
    RunTest("Echo Protobuf 往返",       TestEchoProtobufRoundtrip);
    RunTest("Echo TLV 往返",            TestEchoTLVRoundtrip);
    RunTest("RoomInfo Protobuf 往返",   TestRoomProtobufRoundtrip);
    RunTest("RoomInfo TLV 往返",        TestRoomTLVRoundtrip);

    // 性能对比
    printf("\n--- 性能对比（50 万次循环）---\n");
    TestCompareSimpleSizeAndSpeed();
    TestCompareRoomSizeAndSpeed();

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
