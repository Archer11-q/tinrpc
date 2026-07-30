# Protobuf 消息缺少字段 — 模板编译错误

**发现时间**：2026-07-25  
**发现场景**：压测 v0.11，编译阶段报错 `'class game::GetMetricsRes' has no member named 'success'`  
**严重程度**：中（编译失败，但不涉及运行时逻辑）

## 问题描述

压测工具中有一个通用的 `TimedCall<TReq, TRes>` 模板函数，它假设**所有 Response 消息都有一个 `bool success` 字段**用于结果校验。但 `GetMetricsRes` 在设计时遗漏了这个字段，导致模板实例化时编译错误。

```cpp
template<typename TReq, typename TRes>
std::optional<TRes> TimedCall(GameClient& client, const std::string& method, const TReq& req) {
    auto result = client.Call(method, req);
    if (!result || !result->success()) {  // ← GetMetricsRes 没有 success()！
        return std::nullopt;
    }
    return *result;
}
```

## 根因

Proto 消息定义时只考虑了数据字段（QPS、延迟等），未考虑 RPC 框架的统一错误处理约定。这是一个**跨协议层的设计约束**：框架层要求 `success` 字段，协议设计时未遵守。

## 修复

在 `game.proto` 的 `GetMetricsRes` 消息中添加 `bool success = 1;`，并将后续字段编号依次 +1（避免编号冲突）。

```protobuf
message GetMetricsRes {
    bool success = 1;           // ← 新增，RPC 框架要求
    double qps = 2;             // 原编号 1 → 2
    uint64 total_requests = 3;  // 原编号 2 → 3
    // ...
}
```

## 经验教训

1. **框架层的约定要在 Proto 设计规范中明确**：如果框架要求所有 Response 含 `success` 字段，应该在 proto style guide 中写明，而不是靠模板报错才发现
2. **Protobuf 字段编号一旦分配就不要改**：虽然这是开发阶段，但如果是上线后改字段编号，客户端和服务端将无法通信（二进制不兼容）
3. **模板编译错误的信息通常很长但根因简单**：`has no member named 'success'` 本身足以定位问题，不需要读完整 200 行模板展开
