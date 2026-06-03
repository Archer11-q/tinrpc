#include "rpc/dispatch.h"

#include <cstdio>

namespace rpc {

void Dispatch::RegisterMethod(const std::string& method_name, Handler handler) {
    handlers_[method_name] = std::move(handler);
    printf("[Dispatch] Registered method: %s\n", method_name.c_str());
}

std::optional<std::vector<uint8_t>> Dispatch::Call(const std::string& method_name,
                                                    const std::vector<uint8_t>& body) {
    auto it = handlers_.find(method_name);
    if (it == handlers_.end()) {
        printf("[Dispatch] Method not found: %s\n", method_name.c_str());
        return std::nullopt;
    }
    return it->second(body);
}

} // namespace rpc