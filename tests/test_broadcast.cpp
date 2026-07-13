// ============================================================
// test_broadcast — 房间广播功能测试
//
// 覆盖：BroadcastToRoom / BroadcastToRoomExcept / Protobuf 序列化往返
// ============================================================

#include "game/game_room.h"
#include "game/room_manager.h"
#include "game/broadcast.h"
#include "game.pb.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <string>
#include <vector>
#include <map>
#include <chrono>

// ============================================================
// 简易测试框架
// ============================================================

static int g_passed = 0;
static int g_failed = 0;

void RunTest(const char* name, void (*fn)()) {
    printf("  %-55s ... ", name);
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
// 辅助：Mock 发送回调 — 记录所有发送调用
// ============================================================

struct SendRecord {
    std::string player_id;
    std::vector<uint8_t> data;
};

// ============================================================
// 任务1：BroadcastToRoom 遍历所有成员发送
// ============================================================

// 1. 向房间广播，所有玩家收到消息
void TestBroadcastToAllPlayers() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    // 创建房间 → 房主 owner 自动加入
    std::string room_id = mgr.CreateRoom("owner", cfg);
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);

    // 2 个玩家加入
    assert(mgr.JoinRoom(room_id, "player_2"));
    assert(mgr.JoinRoom(room_id, "player_3"));
    assert(mgr.GetRoom(room_id)->player_count() == 3);  // owner + 2

    // Mock 发送回调：记录所有发送
    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& player_id,
                                 const std::vector<uint8_t>& data) {
        records.push_back({player_id, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    // 序列化 Protobuf 消息
    game::RoomBroadcastMsg msg;
    msg.set_room_id(room_id);
    msg.set_sender_id("owner");
    msg.set_content("hello everyone!");
    msg.set_timestamp(1000);

    std::string serialized;
    assert(msg.SerializeToString(&serialized));
    std::vector<uint8_t> data(serialized.begin(), serialized.end());

    // 广播
    size_t sent = broadcast.BroadcastToRoom(room_id, data);

    assert(sent == 3);  // 3 个玩家都收到
    assert(records.size() == 3);

    // 验证每个玩家收到正确的消息
    for (const auto& rec : records) {
        game::RoomBroadcastMsg received;
        assert(received.ParseFromArray(rec.data.data(), static_cast<int>(rec.data.size())));
        assert(received.room_id() == room_id);
        assert(received.sender_id() == "owner");
        assert(received.content() == "hello everyone!");
        assert(received.timestamp() == 1000);
    }
}

// 2. 房间不存在返回 0
void TestBroadcastToNonExistentRoom() {
    game::RoomManager mgr;

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& player_id,
                                 const std::vector<uint8_t>& data) {
        records.push_back({player_id, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    game::RoomBroadcastMsg msg;
    msg.set_content("nobody will receive this");

    std::string serialized;
    msg.SerializeToString(&serialized);
    std::vector<uint8_t> data(serialized.begin(), serialized.end());

    size_t sent = broadcast.BroadcastToRoom("room_999", data);
    assert(sent == 0);
    assert(records.empty());
}

// 3. 房间只有房主 → 广播只发给 1 人
void TestBroadcastToOnePlayer() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    std::string room_id = mgr.CreateRoom("solo_player", cfg);

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& player_id,
                                 const std::vector<uint8_t>& data) {
        records.push_back({player_id, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    game::RoomBroadcastMsg msg;
    msg.set_content("solo message");

    std::string serialized;
    msg.SerializeToString(&serialized);
    std::vector<uint8_t> data(serialized.begin(), serialized.end());

    size_t sent = broadcast.BroadcastToRoom(room_id, data);
    assert(sent == 1);
    assert(records.size() == 1);
    assert(records[0].player_id == "solo_player");
}

// ============================================================
// 任务2：Protobuf 序列化/反序列化
// ============================================================

// 4. RoomBroadcastMsg 完整往返
void TestBroadcastMsgRoundtrip() {
    game::RoomBroadcastMsg msg;
    msg.set_room_id("room_001");
    msg.set_sender_id("player_a");
    msg.set_content("这是一条广播消息");
    msg.set_timestamp(1713001234567);

    // 序列化
    std::string buf;
    assert(msg.SerializeToString(&buf));
    assert(!buf.empty());

    // 反序列化
    game::RoomBroadcastMsg msg2;
    assert(msg2.ParseFromString(buf));

    assert(msg2.room_id() == "room_001");
    assert(msg2.sender_id() == "player_a");
    assert(msg2.content() == "这是一条广播消息");
    assert(msg2.timestamp() == 1713001234567);
}

// 5. RoomBroadcastMsg 空消息（只有默认值）
void TestBroadcastMsgEmpty() {
    game::RoomBroadcastMsg msg;
    msg.set_room_id("r001");

    std::string buf;
    assert(msg.SerializeToString(&buf));

    game::RoomBroadcastMsg msg2;
    assert(msg2.ParseFromString(buf));
    assert(msg2.room_id() == "r001");
    assert(msg2.sender_id() == "");    // proto3 默认空串
    assert(msg2.content() == "");
    assert(msg2.timestamp() == 0);     // proto3 默认 0
}

// ============================================================
// BroadcastToRoomExcept — 排除指定玩家
// ============================================================

// 6. 排除发送者自身（服务端推送时的常见场景）
void TestBroadcastExceptSender() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    std::string room_id = mgr.CreateRoom("owner", cfg);
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);
    mgr.JoinRoom(room_id, "player_2");
    mgr.JoinRoom(room_id, "player_3");

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& player_id,
                                 const std::vector<uint8_t>& data) {
        records.push_back({player_id, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    game::RoomBroadcastMsg msg;
    msg.set_content("broadcast except sender");

    std::string serialized;
    msg.SerializeToString(&serialized);
    std::vector<uint8_t> data(serialized.begin(), serialized.end());

    // 排除 owner，只发给 player_2 和 player_3
    size_t sent = broadcast.BroadcastToRoomExcept(room_id, "owner", data);

    assert(sent == 2);
    assert(records.size() == 2);

    // 验证 owner 不在接收者中
    for (const auto& rec : records) {
        assert(rec.player_id != "owner");
    }
}

// 7. 排除不存在的玩家 → 正常发给所有人
void TestBroadcastExceptNonExistent() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    std::string room_id = mgr.CreateRoom("owner", cfg);
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);
    mgr.JoinRoom(room_id, "player_2");

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& player_id,
                                 const std::vector<uint8_t>& data) {
        records.push_back({player_id, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    game::RoomBroadcastMsg msg;
    msg.set_content("test");

    std::string serialized;
    msg.SerializeToString(&serialized);
    std::vector<uint8_t> data(serialized.begin(), serialized.end());

    // 排除不存在的玩家 → 不影响，发给所有人
    size_t sent = broadcast.BroadcastToRoomExcept(room_id, "stranger", data);

    assert(sent == 2);  // owner + player_2
    assert(records.size() == 2);
}

// 8. BroadcastToRoomExcept 房间不存在返回 0
void TestBroadcastExceptNonExistentRoom() {
    game::RoomManager mgr;

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& player_id,
                                 const std::vector<uint8_t>& data) {
        records.push_back({player_id, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    game::RoomBroadcastMsg msg;
    msg.set_content("test");
    std::string serialized;
    msg.SerializeToString(&serialized);
    std::vector<uint8_t> data(serialized.begin(), serialized.end());

    size_t sent = broadcast.BroadcastToRoomExcept("room_999", "anyone", data);
    assert(sent == 0);
    assert(records.empty());
}

// ============================================================
// 入口
// ============================================================

int main() {
    printf("=== Broadcast 房间广播测试 ===\n\n");

    printf("[任务1] BroadcastToRoom 遍历所有成员发送\n");
    RunTest("向房间广播，所有玩家收到",          TestBroadcastToAllPlayers);
    RunTest("房间不存在返回 0",                   TestBroadcastToNonExistentRoom);
    RunTest("房间只有 1 人，广播只发 1 人",       TestBroadcastToOnePlayer);

    printf("\n[任务2] Protobuf 序列化/反序列化\n");
    RunTest("RoomBroadcastMsg 完整往返",          TestBroadcastMsgRoundtrip);
    RunTest("RoomBroadcastMsg 空消息",            TestBroadcastMsgEmpty);

    printf("\n[额外] BroadcastToRoomExcept 排除指定玩家\n");
    RunTest("排除发送者自身",                     TestBroadcastExceptSender);
    RunTest("排除不存在的玩家 → 发给所有人",      TestBroadcastExceptNonExistent);
    RunTest("Except 房间不存在返回 0",             TestBroadcastExceptNonExistentRoom);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
