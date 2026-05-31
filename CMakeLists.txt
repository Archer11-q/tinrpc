cmake_minimum_required(VERSION 3.16)
project(rpc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# 公共 include 路径
# ============================================================
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================
# RPC 核心库（静态库，后续所有模块逐步加入）
# ============================================================
add_library(rpc_lib STATIC
    src/serializer.cpp
    src/protocol.cpp
    src/buffer.cpp
    src/socket.cpp
    src/event_loop.cpp
    src/acceptor.cpp
    src/connection.cpp
)

# ============================================================
# 主程序（暂时保留空壳，后续填充）
# ============================================================
add_executable(rpc main.cpp)
target_link_libraries(rpc rpc_lib)

# ============================================================
# 序列化层单元测试
# ============================================================
add_executable(test_serializer
    tests/test_serializer.cpp
)
target_link_libraries(test_serializer rpc_lib)

add_executable(test_protocol
    tests/test_protocol.cpp
)
target_link_libraries(test_protocol rpc_lib)

add_executable(test_network
    tests/test_network.cpp
)
target_link_libraries(test_network rpc_lib pthread)
