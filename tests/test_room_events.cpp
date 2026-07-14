// ============================================================
// test_room_events — 房间事件通知自动广播测试
//
// 覆盖：JoinRoomAndNotify / LeaveRoomAndNotify / StartGameAndNotify
//       验证广播接收者正确、消息内容正确、失败时不广播
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
#include <set>
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
// 辅助：Mock 发送回调 + 接收者收集
// ============================================================

struct SendRecord {
    std::string player_id;
    std::vector<uint8_t> data;
};

// ============================================================
// Protobuf 通知消息序列化往返
// ============================================================

// 1. PlayerJoinNtf 序列化往返
void TestPlayerJoinNtfRoundtrip() {
    game::PlayerJoinNtf ntf;
    ntf.set_room_id("room_001");
    ntf.set_player_id("player_a");
    ntf.set_player_count(3);

    std::string buf;
    assert(ntf.SerializeToString(&buf));

    game::PlayerJoinNtf ntf2;
    assert(ntf2.ParseFromString(buf));
    assert(ntf2.room_id() == "room_001");
    assert(ntf2.player_id() == "player_a");
    assert(ntf2.player_count() == 3);
}

// 2. PlayerLeaveNtf 序列化往返
void TestPlayerLeaveNtfRoundtrip() {
    game::PlayerLeaveNtf ntf;
    ntf.set_room_id("room_001");
    ntf.set_player_id("player_b");
    ntf.set_player_count(1);

    std::string buf;
    assert(ntf.SerializeToString(&buf));

    game::PlayerLeaveNtf ntf2;
    assert(ntf2.ParseFromString(buf));
    assert(ntf2.room_id() == "room_001");
    assert(ntf2.player_id() == "player_b");
    assert(ntf2.player_count() == 1);
}

// 3. GameStartNtf 序列化往返
void TestGameStartNtfRoundtrip() {
    game::GameStartNtf ntf;
    ntf.set_room_id("room_001");
    ntf.set_timestamp(1713001234567);

    std::string buf;
    assert(ntf.SerializeToString(&buf));

    game::GameStartNtf ntf2;
    assert(ntf2.ParseFromString(buf));
    assert(ntf2.room_id() == "room_001");
    assert(ntf2.timestamp() == 1713001234567);
}

// ============================================================
// JoinRoomAndNotify — 加入成功自动广播 PlayerJoinNtf
// ============================================================

// 4. 玩家加入 → 其他人收到 PlayerJoinNtf，加入者自己收不到
void TestJoinAndNotifyOthersReceive() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    auto _r_room_id = mgr.CreateRoom("owner", cfg); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);

    // Mock 发送回调
    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& pid,
                                 const std::vector<uint8_t>& data) {
        records.push_back({pid, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    // player_2 加入
    assert(mgr.JoinRoomAndNotify(room_id, "player_2", &broadcast));

    // 验证：只有 owner 收到通知（player_2 自己被排除）
    assert(records.size() == 1);
    assert(records[0].player_id == "owner");

    // 验证通知内容正确
    game::PlayerJoinNtf ntf;
    assert(ntf.ParseFromArray(records[0].data.data(),
                              static_cast<int>(records[0].data.size())));
    assert(ntf.room_id() == room_id);
    assert(ntf.player_id() == "player_2");
    assert(ntf.player_count() == 2);  // owner + player_2
}

// 5. 房主（唯一玩家）时另一个加入 → 只有房主收到通知
void TestJoinAndNotifySoloOwner() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_room_id = mgr.CreateRoom("owner", cfg); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& pid,
                                 const std::vector<uint8_t>& data) {
        records.push_back({pid, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    // 第一个人加入（只有 owner 在房间）
    assert(mgr.JoinRoomAndNotify(room_id, "player_2", &broadcast));
    assert(records.size() == 1);
    assert(records[0].player_id == "owner");  // 只有 owner 收到
}

// 6. 多人加入 → 每加入一人，已在房间内的所有人都收到通知
void TestJoinAndNotifyMultipleJoins() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    auto _r_room_id = mgr.CreateRoom("owner", cfg); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);

    std::vector<SendRecord> records;
    records.reserve(8);  // 预分配，避免 reallocation
    auto send_mock = [&records](const std::string& pid,
                                 const std::vector<uint8_t>& data) {
        SendRecord rec;
        rec.player_id = pid;
        rec.data = data;
        records.push_back(std::move(rec));
    };

    game::Broadcast broadcast(&mgr, send_mock);

    // player_2 加入 → owner 收到
    assert(mgr.JoinRoomAndNotify(room_id, "player_2", &broadcast));

    // player_3 加入 → owner + player_2 都收到
    assert(mgr.JoinRoomAndNotify(room_id, "player_3", &broadcast));

    // 总共 3 条记录（1 + 2）
    assert(records.size() == 3);

    // 第一条：player_2 加入，owner 收到
    assert(records[0].player_id == "owner");
    {
        assert(!records[0].data.empty());  // 数据不为空
        game::PlayerJoinNtf ntf;
        bool ok = ntf.ParseFromArray(records[0].data.data(),
                                     static_cast<int>(records[0].data.size()));
        assert(ok);
        assert(ntf.player_id() == "player_2");
    }

    // 第二、三条：player_3 加入，owner 和 player_2 收到
    std::set<std::string> receivers;
    for (size_t i = 1; i < records.size(); i++) {
        receivers.insert(records[i].player_id);
        game::PlayerJoinNtf ntf;
        ntf.ParseFromArray(records[i].data.data(),
                          static_cast<int>(records[i].data.size()));
        assert(ntf.player_id() == "player_3");
    }
    assert(receivers.size() == 2);
    assert(receivers.count("owner") == 1);
    assert(receivers.count("player_2") == 1);
}

// 7. JoinRoomAndNotify 失败 → 不广播
void TestJoinAndNotifyFailure() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 2;

    auto _r_room_id = mgr.CreateRoom("owner", cfg); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);
    mgr.JoinRoom(room_id, "player_2");  // 已满

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& pid,
                                 const std::vector<uint8_t>& data) {
        records.push_back({pid, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    // player_3 加入失败（满员）
    assert(!mgr.JoinRoomAndNotify(room_id, "player_3", &broadcast));
    assert(records.empty());  // 没有广播
}

// ============================================================
// LeaveRoomAndNotify — 离开成功自动广播 PlayerLeaveNtf
// ============================================================

// 8. 玩家离开 → 剩余玩家收到 PlayerLeaveNtf
void TestLeaveAndNotifyRemainingReceive() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    auto _r_room_id = mgr.CreateRoom("owner", cfg); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);
    mgr.JoinRoom(room_id, "player_2");
    mgr.JoinRoom(room_id, "player_3");

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& pid,
                                 const std::vector<uint8_t>& data) {
        records.push_back({pid, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    // player_3 离开 → owner 和 player_2 收到通知
    assert(mgr.LeaveRoomAndNotify(room_id, "player_3", &broadcast));

    assert(records.size() == 2);  // owner + player_2
    for (const auto& rec : records) {
        assert(rec.player_id != "player_3");  // 离开者不收到
        game::PlayerLeaveNtf ntf;
        assert(ntf.ParseFromArray(rec.data.data(),
                                  static_cast<int>(rec.data.size())));
        assert(ntf.room_id() == room_id);
        assert(ntf.player_id() == "player_3");
        assert(ntf.player_count() == 2);  // 离开后剩 2 人
    }
}

// 9. 最后一人离开 → 房间销毁，无剩余玩家，不广播
void TestLeaveAndNotifyLastPlayer() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_room_id = mgr.CreateRoom("owner", cfg); assert(_r_room_id); std::string room_id = _r_room_id.room_id;

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& pid,
                                 const std::vector<uint8_t>& data) {
        records.push_back({pid, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    // owner 离开（最后一人）
    assert(mgr.LeaveRoomAndNotify(room_id, "owner", &broadcast));

    // 房间已销毁，无剩余玩家，不广播
    assert(records.empty());
    assert(mgr.GetRoom(room_id)->state() == game::ROOM_STATE_DESTROYED);
}

// 10. LeaveRoomAndNotify 失败 → 不广播
void TestLeaveAndNotifyFailure() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_room_id = mgr.CreateRoom("owner", cfg); assert(_r_room_id); std::string room_id = _r_room_id.room_id;

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& pid,
                                 const std::vector<uint8_t>& data) {
        records.push_back({pid, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    // stranger 不在房间 → 离开失败
    assert(!mgr.LeaveRoomAndNotify(room_id, "stranger", &broadcast));
    assert(records.empty());  // 没有广播
}

// ============================================================
// StartGameAndNotify — 开始成功自动广播 GameStartNtf
// ============================================================

// 11. 房主开始游戏 → 所有人收到 GameStartNtf（含房主自己）
void TestStartGameAndNotifyAllReceive() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    auto _r_room_id = mgr.CreateRoom("owner", cfg); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);
    mgr.JoinRoom(room_id, "player_2");
    mgr.JoinRoom(room_id, "player_3");

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& pid,
                                 const std::vector<uint8_t>& data) {
        records.push_back({pid, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    // 房主开始游戏
    assert(mgr.StartGameAndNotify(room_id, "owner", &broadcast));

    // 所有人（owner + player_2 + player_3）都收到
    assert(records.size() == 3);

    std::set<std::string> receivers;
    for (const auto& rec : records) {
        receivers.insert(rec.player_id);
        game::GameStartNtf ntf;
        assert(ntf.ParseFromArray(rec.data.data(),
                                  static_cast<int>(rec.data.size())));
        assert(ntf.room_id() == room_id);
        assert(ntf.timestamp() > 0);  // 时间戳有效
    }
    assert(receivers.size() == 3);
    assert(receivers.count("owner") == 1);
    assert(receivers.count("player_2") == 1);
    assert(receivers.count("player_3") == 1);

    // 状态已变为 PLAYING
    assert(mgr.GetRoom(room_id)->state() == game::ROOM_STATE_PLAYING);
}

// 12. 非房主开始游戏 → 失败，不广播
void TestStartGameAndNotifyNotOwner() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_room_id = mgr.CreateRoom("owner", cfg); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);
    mgr.JoinRoom(room_id, "player_2");

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& pid,
                                 const std::vector<uint8_t>& data) {
        records.push_back({pid, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    // player_2 不是房主 → 失败
    assert(!mgr.StartGameAndNotify(room_id, "player_2", &broadcast));
    assert(records.empty());
    assert(mgr.GetRoom(room_id)->state() == game::ROOM_STATE_WAITING);  // 状态未变
}

// 13. 非 WAITING 状态开始游戏 → 失败
void TestStartGameAndNotifyWrongState() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_room_id = mgr.CreateRoom("owner", cfg); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    // 不改为 WAITING，保持 IDLE

    std::vector<SendRecord> records;
    auto send_mock = [&records](const std::string& pid,
                                 const std::vector<uint8_t>& data) {
        records.push_back({pid, data});
    };

    game::Broadcast broadcast(&mgr, send_mock);

    // IDLE 状态下不能开始
    assert(!mgr.StartGameAndNotify(room_id, "owner", &broadcast));
    assert(records.empty());
}

// 14. broadcast 为 nullptr → 操作成功但不广播（不崩溃）
void TestNotifyWithNullBroadcast() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    auto _r_room_id = mgr.CreateRoom("owner", cfg); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);

    // broadcast = nullptr, 不应崩溃
    assert(mgr.JoinRoomAndNotify(room_id, "player_2", nullptr));
    assert(mgr.GetRoom(room_id)->player_count() == 2);

    assert(mgr.LeaveRoomAndNotify(room_id, "player_2", nullptr));
    assert(mgr.GetRoom(room_id)->player_count() == 1);

    assert(mgr.StartGameAndNotify(room_id, "owner", nullptr));
    assert(mgr.GetRoom(room_id)->state() == game::ROOM_STATE_PLAYING);
}

// ============================================================
// 入口
// ============================================================

int main() {
    setbuf(stdout, NULL);  // 禁用缓冲，崩溃时能看到已执行测试
    printf("=== 房间事件通知自动广播测试 ===\n\n");

    printf("[Proto] 通知消息序列化往返\n");
    RunTest("PlayerJoinNtf 序列化往返",     TestPlayerJoinNtfRoundtrip);
    RunTest("PlayerLeaveNtf 序列化往返",    TestPlayerLeaveNtfRoundtrip);
    RunTest("GameStartNtf 序列化往返",      TestGameStartNtfRoundtrip);

    printf("\n[JoinRoomAndNotify] 加入 → 自动广播 PlayerJoinNtf\n");
    RunTest("加入者被排除，其他人收到通知",   TestJoinAndNotifyOthersReceive);
    RunTest("房主收到第一个加入者通知",       TestJoinAndNotifySoloOwner);
    RunTest("每加入一人，所有已有成员都收到", TestJoinAndNotifyMultipleJoins);
    RunTest("加入失败 → 不广播",              TestJoinAndNotifyFailure);

    printf("\n[LeaveRoomAndNotify] 离开 → 自动广播 PlayerLeaveNtf\n");
    RunTest("离开后剩余玩家收到通知",          TestLeaveAndNotifyRemainingReceive);
    RunTest("最后一人离开，房间销毁，不广播",  TestLeaveAndNotifyLastPlayer);
    RunTest("离开失败 → 不广播",              TestLeaveAndNotifyFailure);

    printf("\n[StartGameAndNotify] 开始 → 自动广播 GameStartNtf\n");
    RunTest("所有人收到 GameStartNtf（含房主）", TestStartGameAndNotifyAllReceive);
    RunTest("非房主开始 → 失败不广播",           TestStartGameAndNotifyNotOwner);
    RunTest("非 WAITING 状态 → 失败",            TestStartGameAndNotifyWrongState);

    printf("\n[健壮性]\n");
    RunTest("broadcast 为 nullptr 不崩溃",     TestNotifyWithNullBroadcast);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
