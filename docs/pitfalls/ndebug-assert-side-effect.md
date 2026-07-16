# NDEBUG 导致 assert 内函数调用被移除的 Bug

**发现时间**：2026-07-15  
**发现场景**：编写 RoomService RPC 端到端测试（`test_room_service.cpp`）  
**严重程度**：高（测试假通过，实际逻辑未执行）

## 问题描述

CMake `RelWithDebInfo` 构建类型默认定义 `NDEBUG` 宏，导致所有 `assert()` 宏展开为空。当 `assert()` 参数中包含**有副作用的函数调用**时，该函数调用也会被一并移除，导致代码静默跳过关键逻辑。

在本项目中表现为：`assert(client.Connect(port))` 中的 `Connect()` 从未执行，`fd` 保持初始值 `-1`，后续 `send(-1, ...)` 返回 `EBADF`，所有测试虚假通过。

## 触发条件

同时满足：
1. 构建类型为 `Release` / `RelWithDebInfo` / `MinSizeRel`（CMake 默认定义 `NDEBUG`）
2. 函数调用写在 `assert()` 参数中，且该调用对程序正确性必不可少
3. 函数有返回值（如 `bool`），返回值恰好适合作为 assert 的布尔判断

## 错误示例

```cpp
SimpleClient client;

// BUG: NDEBUG 时整行展开为空，Connect() 从未被调用！
assert(client.Connect(port));

// fd 仍为 -1，后续操作全部失败但不报错
ClientLogin(client, "player_a");  // send(-1, ...) → EBADF
```

**NDEBUG 展开后的实际代码**：

```cpp
SimpleClient client;

// assert(client.Connect(port));  ← 整行消失

ClientLogin(client, "player_a");  // fd == -1，静默失败
```

## 正确写法

```cpp
SimpleClient client;

// 将副作用调用与 assert 分离
bool ok = client.Connect(port);
assert(ok);          // NDEBUG 下仅此行为空，Connect() 始终执行

// 或者完全不依赖 assert，使用显式检查
if (!client.Connect(port)) {
    fprintf(stderr, "FATAL: Connect failed\n");
    abort();
}
```

## 全局修复建议

对于测试代码中需要**无条件执行**的校验，定义不依赖 `NDEBUG` 的断言宏：

```cpp
// 始终执行的断言（不受 NDEBUG 影响）
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        abort(); \
    } \
} while(0)
```

这样即使在其他构建类型下也能保证测试逻辑正确执行。

## 经验教训

1. **assert 内永远不要写有副作用的表达式**：`assert(func())` 是 C/C++ 的经典陷阱，所有教程都会强调，但实践中极易踩坑。
2. **测试代码不应依赖 assert**：测试断言应该始终执行，建议用 `CHECK` / `REQUIRE` 替代 `assert`，或使用 Google Test 等不受 `NDEBUG` 影响的测试框架。
3. **RelWithDebInfo 也定义 NDEBUG**：很多开发者以为只有 `Release` 才定义 `NDEBUG`，实际上 CMake 的 `RelWithDebInfo` 和 `MinSizeRel` 都会定义。
4. **怀疑"测试全部通过但没输出"时，优先检查 NDEBUG**：如果代码本该有大量副作用输出却静默通过，很可能 `assert` 吞掉了关键调用。
