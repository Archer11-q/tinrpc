// ============================================================
// test_game_room — GameRoom / RoomManager 单元测试
//
// 覆盖：CreateRoom / JoinRoom / LeaveRoom / 房间空时自动销毁
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
#include <thread>
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
// 任务1：GameRoom 构造 + 成员变量
// ============================================================

// 1. 创建房间后房主自动加入
void TestCreateRoomOwnerJoined() {
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    game::GameRoom room("room_001", "player_1", cfg);

    assert(room.room_id() == "room_001");
    assert(room.state() == game::ROOM_STATE_IDLE);
    assert(room.player_count() == 1);        // 房主自动加入
    assert(room.max_players() == 4);
    assert(room.owner_id() == "player_1");
    assert(room.HasPlayer("player_1"));
    assert(!room.is_full());
    assert(!room.is_empty());
}

// 2. 构造后 timer 可用
void TestGameRoomTimerAccess() {
    game::GameRoom::Config cfg;
    game::GameRoom room("room_001", "player_1", cfg);

    // timer 成员变量存在且可用
    assert(room.timer().PendingCount() == 0);
}

// ============================================================
// 任务2：RoomManager::CreateRoom(playerId, config) 返回 roomId
// ============================================================

// 3. CreateRoom 返回不同 room_id
void TestCreateRoomReturnsId() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_id1 = mgr.CreateRoom("player_a", cfg); assert(_r_id1); std::string id1 = _r_id1.room_id;
    auto _r_id2 = mgr.CreateRoom("player_b", cfg); assert(_r_id2); std::string id2 = _r_id2.room_id;

    assert(id1 != id2);                  // 每次创建返回不同 ID
    assert(id1 == "room_001");
    assert(id2 == "room_002");
    assert(mgr.room_count() == 2);
}

// 4. CreateRoom 后可通过 GetRoom 取到房间
void TestCreateRoomThenGetRoom() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 3;

    auto _r_id = mgr.CreateRoom("owner", cfg); assert(_r_id); std::string id = _r_id.room_id;
    game::GameRoom* room = mgr.GetRoom(id);

    assert(room != nullptr);
    assert(room->room_id() == id);
    assert(room->max_players() == 3);
    assert(room->player_count() == 1);    // 房主已加入
    assert(room->HasPlayer("owner"));
}

// ============================================================
// 任务3：JoinRoom(roomId, playerId) 检查人数上限
// ============================================================

// 5. JoinRoom 成功
void TestJoinRoomSuccess() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    auto _r_id = mgr.CreateRoom("owner", cfg); assert(_r_id); std::string id = _r_id.room_id;

    // 加入前把房间状态改为 WAITING（模拟开放）
    mgr.GetRoom(id)->SetState(game::ROOM_STATE_WAITING);

    assert(mgr.JoinRoom(id, "player_2"));
    assert(mgr.JoinRoom(id, "player_3"));
    assert(mgr.GetRoom(id)->player_count() == 3);
    assert(mgr.GetRoom(id)->HasPlayer("player_2"));
    assert(mgr.GetRoom(id)->HasPlayer("player_3"));
}

// 6. JoinRoom 人数满时拒绝
void TestJoinRoomFull() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 2;  // 仅 2 人

    auto _r_id = mgr.CreateRoom("owner", cfg); assert(_r_id); std::string id = _r_id.room_id;
    mgr.GetRoom(id)->SetState(game::ROOM_STATE_WAITING);

    assert(mgr.JoinRoom(id, "player_2"));   // 第2人，满
    assert(mgr.GetRoom(id)->is_full());

    assert(!mgr.JoinRoom(id, "player_3"));  // 第3人，拒绝
    assert(mgr.GetRoom(id)->player_count() == 2);
}

// 7. JoinRoom 重复加入拒绝
void TestJoinRoomDuplicate() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_id = mgr.CreateRoom("owner", cfg); assert(_r_id); std::string id = _r_id.room_id;
    mgr.GetRoom(id)->SetState(game::ROOM_STATE_WAITING);

    assert(mgr.JoinRoom(id, "player_2"));
    assert(!mgr.JoinRoom(id, "player_2"));  // 重复，拒绝
    assert(mgr.GetRoom(id)->player_count() == 2);  // owner + 一次有效加入
}

// 8. JoinRoom 房间不存在返回 false
void TestJoinRoomNonExistent() {
    game::RoomManager mgr;
    assert(!mgr.JoinRoom("room_999", "player_x"));
}

// 9. JoinRoom 状态不允许时拒绝（PLAYING 状态不可加入）
void TestJoinRoomWrongState() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    auto _r_id = mgr.CreateRoom("owner", cfg); assert(_r_id); std::string id = _r_id.room_id;
    // 不改为 WAITING，保持 IDLE 状态
    // IDLE 允许加入
    assert(mgr.JoinRoom(id, "player_2"));

    // 改为 PLAYING
    mgr.GetRoom(id)->SetState(game::ROOM_STATE_PLAYING);
    assert(!mgr.JoinRoom(id, "player_3"));  // PLAYING 状态拒绝加入
}

// ============================================================
// 任务4：LeaveRoom(roomId, playerId) 更新状态
// ============================================================

// 10. LeaveRoom 成功
void TestLeaveRoomSuccess() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    auto _r_id = mgr.CreateRoom("owner", cfg); assert(_r_id); std::string id = _r_id.room_id;
    mgr.GetRoom(id)->SetState(game::ROOM_STATE_WAITING);
    mgr.JoinRoom(id, "player_2");

    assert(mgr.GetRoom(id)->player_count() == 2);

    assert(mgr.LeaveRoom(id, "player_2"));   // 离开成功
    assert(mgr.GetRoom(id)->player_count() == 1);
    assert(!mgr.GetRoom(id)->HasPlayer("player_2"));
    assert(mgr.GetRoom(id)->HasPlayer("owner"));  // 房主还在
}

// 11. LeaveRoom 房间不存在返回 false
void TestLeaveRoomNonExistent() {
    game::RoomManager mgr;
    assert(!mgr.LeaveRoom("room_999", "player_x"));
}

// 12. LeaveRoom 玩家不在房间返回 false
void TestLeaveRoomPlayerNotInRoom() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_id = mgr.CreateRoom("owner", cfg); assert(_r_id); std::string id = _r_id.room_id;
    assert(!mgr.LeaveRoom(id, "stranger"));  // stranger 不在房间
    assert(mgr.GetRoom(id)->player_count() == 1);  // 房主不受影响
}

// ============================================================
// 任务5：当房间空时标记为销毁
// ============================================================

// 13. 最后一个玩家离开 → 房间标记 DESTROYED
void TestAutoDestroyWhenEmpty() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_id = mgr.CreateRoom("owner", cfg); assert(_r_id); std::string id = _r_id.room_id;
    // 房主离开
    assert(mgr.LeaveRoom(id, "owner"));

    // 房间为空，自动标记销毁
    assert(mgr.GetRoom(id)->is_empty());
    assert(mgr.GetRoom(id)->state() == game::ROOM_STATE_DESTROYED);
}

// 14. 多人房间，逐个离开，最后一个离开才销毁
void TestAutoDestroyLastPlayer() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    auto _r_id = mgr.CreateRoom("owner", cfg); assert(_r_id); std::string id = _r_id.room_id;
    mgr.GetRoom(id)->SetState(game::ROOM_STATE_WAITING);
    mgr.JoinRoom(id, "player_2");
    mgr.JoinRoom(id, "player_3");

    // player_2 离开，房间还有 2 人
    assert(mgr.LeaveRoom(id, "player_2"));
    assert(mgr.GetRoom(id)->player_count() == 2);
    assert(mgr.GetRoom(id)->state() != game::ROOM_STATE_DESTROYED);

    // player_3 离开，房间还有 1 人
    assert(mgr.LeaveRoom(id, "player_3"));
    assert(mgr.GetRoom(id)->player_count() == 1);
    assert(mgr.GetRoom(id)->state() != game::ROOM_STATE_DESTROYED);

    // owner 离开，房间空 → DESTROYED
    assert(mgr.LeaveRoom(id, "owner"));
    assert(mgr.GetRoom(id)->player_count() == 0);
    assert(mgr.GetRoom(id)->state() == game::ROOM_STATE_DESTROYED);
}

// 15. RoomManager::CleanupDestroyed 清理已销毁房间
void TestCleanupDestroyed() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_id1 = mgr.CreateRoom("player_a", cfg); assert(_r_id1); std::string id1 = _r_id1.room_id;
    auto _r_id2 = mgr.CreateRoom("player_b", cfg); assert(_r_id2); std::string id2 = _r_id2.room_id;
    assert(mgr.room_count() == 2);

    // 让 room1 的玩家离开 → 自动销毁
    mgr.LeaveRoom(id1, "player_a");
    assert(mgr.GetRoom(id1)->state() == game::ROOM_STATE_DESTROYED);

    // CleanupDestroyed 清理已销毁的房间
    size_t removed = mgr.CleanupDestroyed();
    assert(removed == 1);
    assert(mgr.room_count() == 1);
    assert(mgr.GetRoom(id1) == nullptr);   // room1 已移除
    assert(mgr.GetRoom(id2) != nullptr);   // room2 还在
}

// 16. RoomManager::RemoveRoom 手动移除
void TestRemoveRoom() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_id = mgr.CreateRoom("owner", cfg); assert(_r_id); std::string id = _r_id.room_id;
    assert(mgr.room_count() == 1);

    assert(mgr.RemoveRoom(id));
    assert(mgr.room_count() == 0);
    assert(mgr.GetRoom(id) == nullptr);

    // 重复移除返回 false
    assert(!mgr.RemoveRoom(id));
}

// ============================================================
// 任务6：房间超时定时器 — CheckRoomTimeout
// ============================================================

// 17. 创建房间设置短超时 → 到期后 CheckRoomTimeout → 房间被销毁
void TestRoomTimeoutDestroyed() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    // 超时 10ms
    auto _r_room_id = mgr.CreateRoom("owner", cfg, 10); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    assert(mgr.room_count() == 1);

    // 等到超时
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    size_t removed = mgr.CheckRoomTimeout();
    assert(removed == 1);
    assert(mgr.room_count() == 0);
    assert(mgr.GetRoom(room_id) == nullptr);
}

// 18. 超时后，房间状态为 DESTROYED 但仍在 map 中（CleanupDestroyed 之前）
void TestRoomTimeoutStateBeforeCleanup() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_room_id = mgr.CreateRoom("owner", cfg, 10); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // 手动 Tick 该房间的定时器，触发到期回调 → 标记 DESTROYED
    mgr.GetRoom(room_id)->timer().Tick();

    // 状态已变为 DESTROYED，但还在 map 里
    assert(mgr.GetRoom(room_id)->state() == game::ROOM_STATE_DESTROYED);
    assert(mgr.room_count() == 1);

    // CleanupDestroyed 后移除
    size_t removed = mgr.CleanupDestroyed();
    assert(removed == 1);
    assert(mgr.room_count() == 0);
}

// 19. timeout_ms=0 时不注册超时，房间不会被自动销毁
void TestRoomTimeoutZeroDisables() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto _r_room_id = mgr.CreateRoom("owner", cfg, 0); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Tick 所有房间的定时器
    mgr.CheckRoomTimeout();

    // 房间不应被销毁（没有注册超时定时器）
    assert(mgr.GetRoom(room_id) != nullptr);
    assert(mgr.GetRoom(room_id)->state() != game::ROOM_STATE_DESTROYED);
}

// 20. PLAYING 状态的房间不会被超时销毁
void TestPlayingRoomNotDestroyedByTimeout() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 4;

    auto _r_room_id = mgr.CreateRoom("owner", cfg, 10); assert(_r_room_id); std::string room_id = _r_room_id.room_id;
    mgr.GetRoom(room_id)->SetState(game::ROOM_STATE_WAITING);
    mgr.JoinRoom(room_id, "player_2");

    // 开始游戏 → PLAYING
    mgr.StartGame(room_id, "owner");
    assert(mgr.GetRoom(room_id)->state() == game::ROOM_STATE_PLAYING);

    // 等到超时
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // 定时器触发，但 PLAYING 状态不应被销毁
    mgr.CheckRoomTimeout();

    assert(mgr.GetRoom(room_id) != nullptr);
    assert(mgr.GetRoom(room_id)->state() == game::ROOM_STATE_PLAYING);
}

// ============================================================
// 任务7：ErrorCode 错误码验证
// ============================================================

// 21. CreateRoom 失败 — 玩家已在其他房间
void TestCreateRoomPlayerAlreadyInRoom() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto r1 = mgr.CreateRoom("owner", cfg);
    assert(r1);

    // 同一玩家再创建房间 → 失败
    auto r2 = mgr.CreateRoom("owner", cfg);
    assert(!r2);
    assert(r2.code == game::ERR_PLAYER_ALREADY_IN_ROOM);
}

// 22. JoinRoom 错误码 — 房间不存在
void TestJoinRoomErrorNotFound() {
    game::RoomManager mgr;
    auto r = mgr.JoinRoom("room_999", "player_x");
    assert(!r);
    assert(r.code == game::ERR_ROOM_NOT_FOUND);
}

// 23. JoinRoom 错误码 — 已满员
void TestJoinRoomErrorFull() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;
    cfg.max_players = 2;

    auto cr = mgr.CreateRoom("owner", cfg); assert(cr);
    mgr.GetRoom(cr.room_id)->SetState(game::ROOM_STATE_WAITING);
    assert(mgr.JoinRoom(cr.room_id, "player_2"));

    auto r = mgr.JoinRoom(cr.room_id, "player_3");
    assert(!r);
    assert(r.code == game::ERR_ROOM_FULL);
}

// 24. JoinRoom 错误码 — 状态不允许
void TestJoinRoomErrorNotJoinable() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto cr = mgr.CreateRoom("owner", cfg); assert(cr);
    mgr.GetRoom(cr.room_id)->SetState(game::ROOM_STATE_PLAYING);

    auto r = mgr.JoinRoom(cr.room_id, "player_2");
    assert(!r);
    assert(r.code == game::ERR_ROOM_NOT_JOINABLE);
}

// 25. JoinRoom 错误码 — 已在其他房间
void TestJoinRoomErrorAlreadyInOtherRoom() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto cr1 = mgr.CreateRoom("owner", cfg); assert(cr1);
    auto cr2 = mgr.CreateRoom("player_2", cfg); assert(cr2);
    mgr.GetRoom(cr2.room_id)->SetState(game::ROOM_STATE_WAITING);

    // player_2 已在 room2 中，尝试加入 room1 → 失败
    auto r = mgr.JoinRoom(cr1.room_id, "player_2");
    assert(!r);
    assert(r.code == game::ERR_PLAYER_ALREADY_IN_ROOM);
}

// 26. LeaveRoom 错误码 — 房间不存在
void TestLeaveRoomErrorNotFound() {
    game::RoomManager mgr;
    auto r = mgr.LeaveRoom("room_999", "player_x");
    assert(!r);
    assert(r.code == game::ERR_ROOM_NOT_FOUND);
}

// 27. LeaveRoom 错误码 — 玩家不在房间
void TestLeaveRoomErrorPlayerNotInRoom() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto cr = mgr.CreateRoom("owner", cfg); assert(cr);
    auto r = mgr.LeaveRoom(cr.room_id, "stranger");
    assert(!r);
    assert(r.code == game::ERR_PLAYER_NOT_IN_ROOM);
}

// 28. StartGame 错误码 — 房间不存在
void TestStartGameErrorNotFound() {
    game::RoomManager mgr;
    auto r = mgr.StartGame("room_999", "owner");
    assert(!r);
    assert(r.code == game::ERR_ROOM_NOT_FOUND);
}

// 29. StartGame 错误码 — 不是房主
void TestStartGameErrorNotOwner() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto cr = mgr.CreateRoom("owner", cfg); assert(cr);
    mgr.GetRoom(cr.room_id)->SetState(game::ROOM_STATE_WAITING);

    auto r = mgr.StartGame(cr.room_id, "player_2");
    assert(!r);
    assert(r.code == game::ERR_NOT_OWNER);
}

// 30. StartGame 错误码 — 状态不允许
void TestStartGameErrorWrongState() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto cr = mgr.CreateRoom("owner", cfg); assert(cr);
    // 不改为 WAITING，保持 IDLE

    auto r = mgr.StartGame(cr.room_id, "owner");
    assert(!r);
    assert(r.code == game::ERR_WRONG_ROOM_STATE);
}

// 31. GetPlayerRoom — 正确返回玩家所在房间
void TestGetPlayerRoom() {
    game::RoomManager mgr;
    game::GameRoom::Config cfg;

    auto cr1 = mgr.CreateRoom("owner", cfg); assert(cr1);
    auto cr2 = mgr.CreateRoom("player_2", cfg); assert(cr2);

    assert(mgr.GetPlayerRoom("owner") == cr1.room_id);
    assert(mgr.GetPlayerRoom("player_2") == cr2.room_id);
    assert(mgr.GetPlayerRoom("stranger") == "");  // 不在任何房间
}

// ============================================================
// 入口
// ============================================================

int main() {
    printf("=== GameRoom / RoomManager 单元测试 ===\n\n");

    printf("[任务1] GameRoom 构造 + 成员变量\n");
    RunTest("创建房间后房主自动加入",            TestCreateRoomOwnerJoined);
    RunTest("timer 成员变量可用",                 TestGameRoomTimerAccess);

    printf("\n[任务2] CreateRoom(playerId, config)\n");
    RunTest("CreateRoom 返回不同 room_id",       TestCreateRoomReturnsId);
    RunTest("CreateRoom 后 GetRoom 可取到房间",   TestCreateRoomThenGetRoom);

    printf("\n[任务3] JoinRoom(roomId, playerId) 检查人数上限\n");
    RunTest("JoinRoom 成功",                     TestJoinRoomSuccess);
    RunTest("JoinRoom 人数满时拒绝",              TestJoinRoomFull);
    RunTest("JoinRoom 重复加入拒绝",              TestJoinRoomDuplicate);
    RunTest("JoinRoom 房间不存在返回 false",       TestJoinRoomNonExistent);
    RunTest("JoinRoom 状态不允许时拒绝",           TestJoinRoomWrongState);

    printf("\n[任务4] LeaveRoom(roomId, playerId) 更新状态\n");
    RunTest("LeaveRoom 成功",                    TestLeaveRoomSuccess);
    RunTest("LeaveRoom 房间不存在返回 false",      TestLeaveRoomNonExistent);
    RunTest("LeaveRoom 玩家不在房间返回 false",    TestLeaveRoomPlayerNotInRoom);

    printf("\n[任务5] 房间空时标记为销毁\n");
    RunTest("最后一个玩家离开 → DESTROYED",       TestAutoDestroyWhenEmpty);
    RunTest("多人逐个离开，最后一个才销毁",         TestAutoDestroyLastPlayer);
    RunTest("CleanupDestroyed 清理已销毁房间",     TestCleanupDestroyed);
    RunTest("RemoveRoom 手动移除",                TestRemoveRoom);

    printf("\n[任务6] 房间超时定时器\n");
    RunTest("超时后 CheckRoomTimeout 销毁房间",    TestRoomTimeoutDestroyed);
    RunTest("超时后状态 DESTROYED 但仍在 map",     TestRoomTimeoutStateBeforeCleanup);
    RunTest("timeout=0 不注册超时",               TestRoomTimeoutZeroDisables);
    RunTest("PLAYING 状态不被超时销毁",            TestPlayingRoomNotDestroyedByTimeout);

    printf("\n[任务7] ErrorCode 错误码验证\n");
    RunTest("CreateRoom 玩家已在其他房间",          TestCreateRoomPlayerAlreadyInRoom);
    RunTest("JoinRoom ROOM_NOT_FOUND",              TestJoinRoomErrorNotFound);
    RunTest("JoinRoom ROOM_FULL",                   TestJoinRoomErrorFull);
    RunTest("JoinRoom ROOM_NOT_JOINABLE",           TestJoinRoomErrorNotJoinable);
    RunTest("JoinRoom 已在其他房间",                 TestJoinRoomErrorAlreadyInOtherRoom);
    RunTest("LeaveRoom ROOM_NOT_FOUND",             TestLeaveRoomErrorNotFound);
    RunTest("LeaveRoom PLAYER_NOT_IN_ROOM",         TestLeaveRoomErrorPlayerNotInRoom);
    RunTest("StartGame ROOM_NOT_FOUND",             TestStartGameErrorNotFound);
    RunTest("StartGame ERR_NOT_OWNER",              TestStartGameErrorNotOwner);
    RunTest("StartGame ERR_WRONG_ROOM_STATE",       TestStartGameErrorWrongState);
    RunTest("GetPlayerRoom 正确返回所在房间",        TestGetPlayerRoom);

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
