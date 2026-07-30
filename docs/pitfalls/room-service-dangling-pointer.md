# RoomServiceImpl 局部变量悬空指针 — Segfault

**发现时间**：2026-07-25  
**发现场景**：压测 v0.11，客户端一发送 CreateRoom 请求，服务端立即 segfault  
**严重程度**：致命（100% 必现崩溃）

## 问题描述

`GameService` 构造函数中，`RoomServiceImpl room_svc` 声明为**栈上的局部变量**，构造函数结束后立即析构。而 `Dispatch` 中注册的 8 个 lambda 全部捕获了 `this` 指针指向这个局部变量，后续 RPC 调用时访问悬空指针 → segfault。

## 触发条件

任何 RPC 调用（CreateRoom / JoinRoom / LeaveRoom 等）都会触发。

## 错误代码

```cpp
// game_service.cpp
GameService::GameService() {
    game::RoomServiceImpl room_svc(&room_mgr_, &broadcast_);  // 栈上局部变量！

    dispatch_.Register("CreateRoom", [&room_svc](auto& body) {
        return room_svc.CreateRoom(body);  // room_svc 析构后访问 → 悬空指针
    });
    // ... 另外 7 个 lambda 同样捕获 &room_svc
}   // ← room_svc 在此析构，所有 lambda 中的引用全部悬空
```

## 正确代码

```cpp
// game_service.h
std::unique_ptr<game::RoomServiceImpl> room_svc_;  // 成员变量，生命周期与 GameService 一致

// game_service.cpp
GameService::GameService() {
    room_svc_ = std::make_unique<game::RoomServiceImpl>(&room_mgr_, &broadcast_);

    dispatch_.Register("CreateRoom", [this](auto& body) {
        return room_svc_->CreateRoom(body);  // room_svc_ 始终有效
    });
}
```

## 经验教训

1. **构造函数中创建的局部变量，生命周期仅限于构造函数体**：C++ 不会因为 lambda 捕获引用而延长对象的生命周期
2. **捕获 `this` 优于捕获局部变量引用**：`this` 指向对象本身，只要 `GameService` 存活就能用
3. **压测是暴露此类问题的最高效手段**：单连接测试不会区分"正确"和"碰巧没崩溃"，并发压测让悬空指针立即暴露
4. **这类 Bug 编译器不报错**：lambda 捕获引用是语法合法的，只有运行时才崩溃，地址随机时可能"偶尔正常"
