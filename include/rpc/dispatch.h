#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <functional>
#include <optional>

namespace rpc {

// ============================================================
/// @brief Dispatch — 服务端方法分发器
///
/// 职责：
/// - 维护方法名 → 处理函数的映射表
/// - 收到 Frame 后根据 method_name 查表，调用对应的处理函数
/// - 处理函数接收 body 字节，返回 response body 字节
///
/// 替代 v0.3/v0.4 中手写 if-else 方法名判断的方式
// ============================================================
class Dispatch {
public:
    /// @brief 服务端处理函数类型
    /// @details 接收请求 body → 返回响应 body，返回 nullopt 表示方法不存在或参数错误
    using Handler =
        std::function<std::optional<std::vector<uint8_t>>(const std::vector<uint8_t>& body)>;

    Dispatch() = default;

    /// @brief 注册方法
    /// @param method_name 方法名（用于 Dispatch 分发）
    /// @param handler 处理函数（Handler 类型）
    void RegisterMethod(const std::string& method_name, Handler handler);

    /// @brief 根据方法名查找并调用处理函数
    /// @param method_name 要调用的方法名
    /// @param body 请求 body 字节
    /// @return 响应 body 字节；方法未注册时返回 nullopt
    std::optional<std::vector<uint8_t>> Call(const std::string& method_name,
                                             const std::vector<uint8_t>& body);

private:
    std::unordered_map<std::string, Handler> handlers_;
};

} // namespace rpc