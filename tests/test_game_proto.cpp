// ============================================================
// test_game_proto — 游戏协议消息的正确性测试
//
// 验证 proto/game.proto 中所有消息类型的序列化/反序列化往返。
// 不测性能（性能对比见 test_proto_vs_tlv.cpp），只测正确性。
// ============================================================

#include "game.pb.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <string>

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
// 辅助：构建一个测试用 PlayerInfo
// ============================================================

game::PlayerInfo MakePlayer(const std::string& id, const std::string& name, game::PlayerRank rank) {
    game::PlayerInfo p;
    p.set_player_id(id);
    p.set_player_name(name);
    p.set_rank(rank);
    return p;
}

// ============================================================
// 测试用例
// ============================================================

// 1. Login 往返
void TestLoginRoundtrip() {
    // 请求
    game::LoginReq req;
    req.set_token("test_token_abc123");

    std::string buf;
    assert(req.SerializeToString(&buf));

    game::LoginReq req2;
    assert(req2.ParseFromString(buf));
    assert(req2.token() == "test_token_abc123");

    // 响应（成功）
    game::LoginRes res;
    res.set_success(true);
    auto* info = res.mutable_player_info();
    info->set_player_id("p001");
    info->set_player_name("张三");
    info->set_rank(game::RANK_GOLD);

    std::string buf2;
    assert(res.SerializeToString(&buf2));

    game::LoginRes res2;
    assert(res2.ParseFromString(buf2));
    assert(res2.success() == true);
    assert(res2.player_info().player_id() == "p001");
    assert(res2.player_info().player_name() == "张三");
    assert(res2.player_info().rank() == game::RANK_GOLD);

    // 响应（失败）
    game::LoginRes res_fail;
    res_fail.set_success(false);
    res_fail.set_error_msg("token 已过期");

    std::string buf3;
    assert(res_fail.SerializeToString(&buf3));

    game::LoginRes res_fail2;
    assert(res_fail2.ParseFromString(buf3));
    assert(res_fail2.success() == false);
    assert(res_fail2.error_msg() == "token 已过期");
}

// 2. CreateRoom 往返
void TestCreateRoomRoundtrip() {
    game::CreateRoomReq req;
    req.set_player_id("p001");
    req.set_room_name("测试房间");
    req.set_max_players(4);

    std::string buf;
    assert(req.SerializeToString(&buf));

    game::CreateRoomReq req2;
    assert(req2.ParseFromString(buf));
    assert(req2.player_id() == "p001");
    assert(req2.room_name() == "测试房间");
    assert(req2.max_players() == 4);

    // 响应
    game::CreateRoomRes res;
    res.set_success(true);
    auto* room = res.mutable_room_info();
    room->set_room_id("room_001");
    room->set_room_name("测试房间");
    room->set_player_count(1);
    room->set_max_players(4);
    room->set_room_state(game::ROOM_STATE_WAITING);
    auto* p = room->add_players();
    p->set_player_id("p001");
    p->set_player_name("张三");

    std::string buf2;
    assert(res.SerializeToString(&buf2));

    game::CreateRoomRes res2;
    assert(res2.ParseFromString(buf2));
    assert(res2.success() == true);
    assert(res2.room_info().room_id() == "room_001");
    assert(res2.room_info().players_size() == 1);
}

// 3. JoinRoom 往返
void TestJoinRoomRoundtrip() {
    game::JoinRoomReq req;
    req.set_player_id("p002");
    req.set_room_id("room_001");

    std::string buf;
    assert(req.SerializeToString(&buf));

    game::JoinRoomReq req2;
    assert(req2.ParseFromString(buf));
    assert(req2.player_id() == "p002");
    assert(req2.room_id() == "room_001");

    // 响应（含两个玩家的完整房间信息）
    game::JoinRoomRes res;
    res.set_success(true);
    auto* room = res.mutable_room_info();
    room->set_room_id("room_001");
    room->set_room_name("测试房间");
    room->set_player_count(2);
    room->set_room_state(game::ROOM_STATE_WAITING);
    auto* p1 = room->add_players();
    p1->set_player_id("p001");
    p1->set_player_name("张三");
    auto* p2 = room->add_players();
    p2->set_player_id("p002");
    p2->set_player_name("李四");

    std::string buf2;
    assert(res.SerializeToString(&buf2));

    game::JoinRoomRes res2;
    assert(res2.ParseFromString(buf2));
    assert(res2.success() == true);
    assert(res2.room_info().players_size() == 2);
    assert(res2.room_info().players(1).player_name() == "李四");
}

// 4. LeaveRoom 往返
void TestLeaveRoomRoundtrip() {
    game::LeaveRoomReq req;
    req.set_player_id("p002");
    req.set_room_id("room_001");

    std::string buf;
    assert(req.SerializeToString(&buf));

    game::LeaveRoomReq req2;
    assert(req2.ParseFromString(buf));
    assert(req2.player_id() == "p002");
    assert(req2.room_id() == "room_001");

    // 响应
    game::LeaveRoomRes res;
    res.set_success(true);

    std::string buf2;
    assert(res.SerializeToString(&buf2));

    game::LeaveRoomRes res2;
    assert(res2.ParseFromString(buf2));
    assert(res2.success() == true);
}

// 5. StartGame 往返
void TestStartGameRoundtrip() {
    game::StartGameReq req;
    req.set_room_id("room_001");
    req.set_player_id("p001"); // 房主发起

    std::string buf;
    assert(req.SerializeToString(&buf));

    game::StartGameReq req2;
    assert(req2.ParseFromString(buf));
    assert(req2.room_id() == "room_001");
    assert(req2.player_id() == "p001");

    // 响应（失败：人数不足）
    game::StartGameRes res;
    res.set_success(false);
    res.set_error_msg("人数不足，至少需要 2 人才能开始游戏");

    std::string buf2;
    assert(res.SerializeToString(&buf2));

    game::StartGameRes res2;
    assert(res2.ParseFromString(buf2));
    assert(res2.success() == false);
    assert(!res2.error_msg().empty());
}

// 6. GameOverNtf 往返（含 repeated PlayerScore）
void TestGameOverNtfRoundtrip() {
    game::GameOverNtf ntf;
    ntf.set_room_id("room_001");
    ntf.set_game_duration_sec(120);

    auto* s1 = ntf.add_scores();
    s1->set_player_id("p001");
    s1->set_player_name("张三");
    s1->set_score(1500);
    s1->set_rank(1);

    auto* s2 = ntf.add_scores();
    s2->set_player_id("p002");
    s2->set_player_name("李四");
    s2->set_score(800);
    s2->set_rank(2);

    std::string buf;
    assert(ntf.SerializeToString(&buf));

    game::GameOverNtf ntf2;
    assert(ntf2.ParseFromString(buf));
    assert(ntf2.room_id() == "room_001");
    assert(ntf2.game_duration_sec() == 120);
    assert(ntf2.scores_size() == 2);
    assert(ntf2.scores(0).player_name() == "张三");
    assert(ntf2.scores(0).rank() == 1);
    assert(ntf2.scores(1).score() == 800);
}

// 7. RoomInfo 完整往返（含 repeated PlayerInfo）
void TestRoomInfoRoundtrip() {
    game::RoomInfo room;
    room.set_room_id("room_001");
    room.set_room_name("测试房间");
    room.set_player_count(2);
    room.set_max_players(4);
    room.set_room_state(game::ROOM_STATE_WAITING);

    auto* p1 = room.add_players();
    p1->set_player_id("p001");
    p1->set_player_name("张三");
    p1->set_rank(game::RANK_GOLD);

    auto* p2 = room.add_players();
    p2->set_player_id("p002");
    p2->set_player_name("李四");
    p2->set_rank(game::RANK_SILVER);

    std::string buf;
    assert(room.SerializeToString(&buf));
    assert(!buf.empty());

    game::RoomInfo room2;
    assert(room2.ParseFromString(buf));
    assert(room2.room_id() == "room_001");
    assert(room2.room_name() == "测试房间");
    assert(room2.player_count() == 2);
    assert(room2.max_players() == 4);
    assert(room2.room_state() == game::ROOM_STATE_WAITING);
    assert(room2.players_size() == 2);
    assert(room2.players(0).player_id() == "p001");
    assert(room2.players(0).player_name() == "张三");
    assert(room2.players(1).player_id() == "p002");
    assert(room2.players(1).player_name() == "李四");
}

// ============================================================
// 入口
// ============================================================

int main() {
    printf("=== 游戏协议消息正确性测试 ===\n\n");

    RunTest("LoginReq/Res 往返", TestLoginRoundtrip);
    RunTest("CreateRoomReq/Res 往返", TestCreateRoomRoundtrip);
    RunTest("JoinRoomReq/Res 往返", TestJoinRoomRoundtrip);
    RunTest("LeaveRoomReq/Res 往返", TestLeaveRoomRoundtrip);
    RunTest("StartGameReq/Res 往返", TestStartGameRoundtrip);
    RunTest("GameOverNtf 往返", TestGameOverNtfRoundtrip);
    RunTest("RoomInfo 完整往返", TestRoomInfoRoundtrip);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
