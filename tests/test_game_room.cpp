// ============================================================
// test_game_room — GameRoom / RoomManager 单元测试
//
// 覆盖：CreateRoom / JoinRoom / LeaveRoom / 房间空时自动销毁
// ============================================================

#include "game/game_room.h"
#include "game/room_manager.h"
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

    std::string id1 = mgr.CreateRoom("player_a", cfg);
    std::string id2 = mgr.CreateRoom("player_b", cfg);

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

    std::string id = mgr.CreateRoom("owner", cfg);
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

    std::string id = mgr.CreateRoom("owner", cfg);

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

    std::string id = mgr.CreateRoom("owner", cfg);
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

    std::string id = mgr.CreateRoom("owner", cfg);
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

    std::string id = mgr.CreateRoom("owner", cfg);
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

    std::string id = mgr.CreateRoom("owner", cfg);
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

    std::string id = mgr.CreateRoom("owner", cfg);
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

    std::string id = mgr.CreateRoom("owner", cfg);
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

    std::string id = mgr.CreateRoom("owner", cfg);
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

    std::string id1 = mgr.CreateRoom("player_a", cfg);
    std::string id2 = mgr.CreateRoom("player_b", cfg);
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

    std::string id = mgr.CreateRoom("owner", cfg);
    assert(mgr.room_count() == 1);

    assert(mgr.RemoveRoom(id));
    assert(mgr.room_count() == 0);
    assert(mgr.GetRoom(id) == nullptr);

    // 重复移除返回 false
    assert(!mgr.RemoveRoom(id));
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

    printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
