cmake_minimum_required(VERSION 3.16)

set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -fno-omit-frame-pointer -g")

project(rpc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# Protobuf
# ============================================================
find_package(Protobuf REQUIRED)

# 用 protoc 从 .proto 生成 C++ 代码
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS proto/game.proto)

# ============================================================
# 公共 include 路径
# ============================================================
include_directories(${CMAKE_SOURCE_DIR}/include)
# Protobuf 生成的头文件在 build 目录下
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${Protobuf_INCLUDE_DIRS})

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
    src/thread_pool.cpp
    src/dispatch.cpp
    src/rpc_client.cpp
    src/game/timer_manager.cpp
    src/game/game_room.cpp
    src/game/room_manager.cpp
    src/game/broadcast.cpp
    src/game/room_service.cpp
    src/game/input_buffer.cpp
    src/game/frame_sync.cpp
    ${PROTO_SRCS}
)

target_link_libraries(rpc_lib ${Protobuf_LIBRARIES})

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

add_executable(test_thread_pool
    tests/test_thread_pool.cpp
)
target_link_libraries(test_thread_pool rpc_lib pthread)

add_executable(test_rpc
    tests/test_rpc.cpp
)
target_link_libraries(test_rpc rpc_lib pthread)

# ============================================================
# Benchmark — 性能对比
# ============================================================
add_executable(bench_server
    bench/bench_server.cpp
)
target_link_libraries(bench_server rpc_lib pthread)

add_executable(bench_client
    bench/bench_client.cpp
)
target_link_libraries(bench_client rpc_lib pthread)

add_executable(bench_serialize
    bench/bench_serialize.cpp
)
target_link_libraries(bench_serialize rpc_lib pthread)

add_executable(packet_frag_test
    bench/packet_frag_test.cpp
)
target_link_libraries(packet_frag_test rpc_lib pthread)

# ============================================================
# TLV vs Protobuf 序列化对比测试
# ============================================================
add_executable(test_proto_vs_tlv
    tests/test_proto_vs_tlv.cpp
)
target_link_libraries(test_proto_vs_tlv rpc_lib pthread)

# ============================================================
# 游戏协议消息正确性测试
# ============================================================
add_executable(test_game_proto
    tests/test_game_proto.cpp
)
target_link_libraries(test_game_proto rpc_lib pthread)

# ============================================================
# TimerManager 小顶堆定时器测试
# ============================================================
add_executable(test_timer_manager
    tests/test_timer_manager.cpp
)
target_link_libraries(test_timer_manager rpc_lib pthread)

# ============================================================
# GameRoom / RoomManager 状态机测试
# ============================================================
add_executable(test_game_room
    tests/test_game_room.cpp
)
target_link_libraries(test_game_room rpc_lib pthread)

# ============================================================
# Broadcast 房间广播测试
# ============================================================
add_executable(test_broadcast
    tests/test_broadcast.cpp
)
target_link_libraries(test_broadcast rpc_lib pthread)

# ============================================================
# 房间事件通知自动广播测试
# ============================================================
add_executable(test_room_events
    tests/test_room_events.cpp
)
target_link_libraries(test_room_events rpc_lib pthread)

# ============================================================
# 游戏房间端到端流程测试
# ============================================================
add_executable(test_game_e2e
    tests/test_game_e2e.cpp
)
target_link_libraries(test_game_e2e rpc_lib pthread)

# ============================================================
# RoomService RPC 端到端测试（Stub → Dispatch → RoomManager）
# ============================================================
add_executable(test_room_service
    tests/test_room_service.cpp
)
target_link_libraries(test_room_service rpc_lib pthread)

# ============================================================
# InputBuffer 帧同步输入缓冲测试
# ============================================================
add_executable(test_input_buffer
    tests/test_input_buffer.cpp
)
target_link_libraries(test_input_buffer rpc_lib pthread)

# ============================================================
# FrameSyncManager 帧同步测试
# ============================================================
add_executable(test_frame_sync
    tests/test_frame_sync.cpp
)
target_link_libraries(test_frame_sync rpc_lib pthread)
