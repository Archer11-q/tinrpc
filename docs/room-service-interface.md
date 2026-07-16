# 房间服务器 RPC 接口清单

> 版本 v0.8 完整接口审计  
> 日期 2026-07-15

---

## 一、注册到 Dispatch 的 RPC 方法（6 个）

| 方法名 | 请求类型 | 响应类型 | 可返回 ErrorCode | 状态 |
|--------|---------|---------|-----------------|:--:|
| `CreateRoom` | `CreateRoomReq` | `CreateRoomRes` | `ERR_PLAYER_ALREADY_IN_ROOM` | ✅ |
| `JoinRoom` | `JoinRoomReq` | `JoinRoomRes` | `ERR_ROOM_NOT_FOUND` / `ERR_ROOM_FULL` / `ERR_ROOM_NOT_JOINABLE` / `ERR_PLAYER_ALREADY_IN_ROOM` | ✅ |
| `LeaveRoom` | `LeaveRoomReq` | `LeaveRoomRes` | `ERR_ROOM_NOT_FOUND` / `ERR_PLAYER_NOT_IN_ROOM` | ✅ |
| `SendMessage` | `SendMessageReq` | `SendMessageRes` | `ERR_ROOM_NOT_FOUND` | ✅ |
| `GetRoomList` | `GetRoomListReq` | `GetRoomListRes` | 无错误场景 | ✅ |
| `StartGame` | `StartGameReq` | `StartGameRes` | `ERR_ROOM_NOT_FOUND` / `ERR_NOT_OWNER` / `ERR_WRONG_ROOM_STATE` | ✅ |

---

## 二、未注册到 Dispatch 的操作

无。全部 6 个 RoomManager 操作方法均已通过 RoomService 暴露为 RPC。

---

## 三、各方法详细接口

### 1. CreateRoom

```
请求：CreateRoomReq { player_id, room_name?, max_players }
响应：CreateRoomRes { success, room_info?, error_msg, error_code }

服务端流程：
  RoomServiceImpl::CreateRoom
    → RoomManager::CreateRoom(player_id, config, timeout)
    → 成功后 SetState(ROOM_STATE_WAITING)
    → 失败透传 ErrorCode

错误码：
  ERR_NONE (0)                  — 成功
  ERR_PLAYER_ALREADY_IN_ROOM (4)— 房主已在其他房间
```

### 2. JoinRoom

```
请求：JoinRoomReq { player_id, room_id }
响应：JoinRoomRes { success, room_info?, error_msg, error_code }

服务端流程：
  RoomServiceImpl::JoinRoom
    → RoomManager::JoinRoomAndNotify() → Broadcast PlayerJoinNtf
    → 或 RoomManager::JoinRoom()（无广播）

错误码：
  ERR_NONE (0)                  — 成功
  ERR_ROOM_NOT_FOUND (1)        — 房间不存在
  ERR_ROOM_FULL (2)             — 房间已满
  ERR_ROOM_NOT_JOINABLE (3)     — 房间状态不允许（IDLE 或 PLAYING 等）
  ERR_PLAYER_ALREADY_IN_ROOM (4)— 玩家已在其他房间
```

### 3. LeaveRoom

```
请求：LeaveRoomReq { player_id, room_id }
响应：LeaveRoomRes { success, error_msg, error_code }

服务端流程：
  RoomServiceImpl::LeaveRoom
    → RoomManager::LeaveRoomAndNotify() → Broadcast PlayerLeaveNtf
    → 或 RoomManager::LeaveRoom()（无广播）
    → 房间空时自动标记 DESTROYED

错误码：
  ERR_NONE (0)                  — 成功
  ERR_ROOM_NOT_FOUND (1)        — 房间不存在
  ERR_PLAYER_NOT_IN_ROOM (5)    — 玩家不在该房间中
```

### 4. SendMessage

```
请求：SendMessageReq { room_id, sender_id, content }
响应：SendMessageRes { success, error_msg, error_code }

服务端流程：
  RoomServiceImpl::SendMessage
    → 检查房间是否存在
    → BroadcastToRoomExcept（排除发送者）— 广播 RoomBroadcastMsg
    → 发送者通过响应获知成功

错误码：
  ERR_NONE (0)                  — 成功
  ERR_ROOM_NOT_FOUND (1)        — 房间不存在
```

### 5. GetRoomList

```
请求：GetRoomListReq { }（无字段，保留扩展）
响应：GetRoomListRes { rooms: repeated RoomInfo }

服务端流程：
  RoomServiceImpl::GetRoomList
    → RoomManager::GetAllRoomIds()
    → 遍历，排除 DESTROYED 状态的房间
    → 返回所有可加入房间的列表

错误码：无错误场景（空列表 = 无房间）
```

### 6. StartGame

```
请求：StartGameReq { room_id, player_id }
响应：StartGameRes { success, error_msg, error_code }

服务端流程：
  RoomServiceImpl::StartGame
    → RoomManager::StartGameAndNotify() → Broadcast GameStartNtf
    → 或 RoomManager::StartGame()（无广播）
    → 仅房主可调用，且房间必须在 WAITING 状态

错误码：
  ERR_NONE (0)                  — 成功
  ERR_ROOM_NOT_FOUND (1)        — 房间不存在
  ERR_NOT_OWNER (6)             — 不是房主
  ERR_WRONG_ROOM_STATE (7)      — 房间状态不允许（非 WAITING）
```

---

## 四、ErrorCode 枚举全集（7 个有效码）

| 值 | 枚举名 | 使用方 | 端到端测试 |
|:--:|--------|--------|:--:|
| 0 | `ERR_NONE` | 所有方法（成功默认值） | — |
| 1 | `ERR_ROOM_NOT_FOUND` | JoinRoom, LeaveRoom, SendMessage | ✅ |
| 2 | `ERR_ROOM_FULL` | JoinRoom | ✅ |
| 3 | `ERR_ROOM_NOT_JOINABLE` | JoinRoom | ✅ |
| 4 | `ERR_PLAYER_ALREADY_IN_ROOM` | CreateRoom, JoinRoom | ✅ |
| 5 | `ERR_PLAYER_NOT_IN_ROOM` | LeaveRoom | ✅ |
| 6 | `ERR_NOT_OWNER` | StartGame | ✅ (test_game_room) |
| 7 | `ERR_WRONG_ROOM_STATE` | StartGame | ✅ (test_game_room) |

---

## 五、服务端推送通知（无请求-响应，不计入 RPC 方法）

| 通知类型 | 触发时机 | 广播方式 |
|---------|---------|---------|
| `PlayerJoinNtf` | JoinRoomAndNotify 成功后 | BroadcastToRoomExcept（排除加入者） |
| `PlayerLeaveNtf` | LeaveRoomAndNotify 成功后 | BroadcastToRoom（发给剩余玩家） |
| `GameStartNtf` | StartGameAndNotify 成功后 | BroadcastToRoom（发给所有人） |
| `RoomBroadcastMsg` | SendMessage 时 | BroadcastToRoomExcept（排除发送者） |

---

## 六、测试覆盖矩阵

| RPC 方法 | 成功路径 | 每项 ErrorCode | 文件 |
|---------|:--:|:--:|------|
| CreateRoom | ✅ | ✅ ERR_PLAYER_ALREADY_IN_ROOM | test_room_service.cpp |
| JoinRoom | ✅ | ✅ 全部 4 种错误码 | test_room_service.cpp |
| LeaveRoom | ✅ | ✅ 全部 2 种错误码 | test_room_service.cpp |
| SendMessage | ✅ | ✅ ERR_ROOM_NOT_FOUND | test_room_service.cpp |
| GetRoomList | ✅ | N/A | test_room_service.cpp |
| StartGame | ✅ | ✅ (test_game_room: ERR_NOT_OWNER + ERR_WRONG_ROOM_STATE) | test_game_room.cpp |

---

## 七、接口状态

全部 6 个 RoomManager 操作方法均已通过 RoomService 暴露为 RPC，完整覆盖 7 个 ErrorCode。无已知缺口。
