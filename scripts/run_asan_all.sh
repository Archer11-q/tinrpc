#!/usr/bin/env bash
# ============================================================
# ASan 全量测试
#
# 背景：CMake 里的 ENABLE_ASAN 选项自 v0.8 起就存在，但此前只用它排查过
#       单个 bug，从未跑过全量。本脚本补上这道验证。
#
# 用法：
#   ./scripts/run_asan_all.sh
#   BUILD_DIR=/tmp/my_asan OUT_DIR=/tmp/my_logs ./scripts/run_asan_all.sh
# ============================================================
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${BUILD_DIR:-/tmp/rpc_asan_build}
OUT_DIR=${OUT_DIR:-/tmp/rpc_asan_logs}
JOBS=$(nproc 2>/dev/null || echo 4)

rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR" "$OUT_DIR"
cd "$BUILD_DIR" || exit 1

echo "源码:   $SRC"
echo "构建:   $BUILD_DIR"
echo "日志:   $OUT_DIR"
echo ""

cmake "$SRC" -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON > "$OUT_DIR/cmake.log" 2>&1
if [ $? -ne 0 ]; then
    echo "CONFIGURE FAILED"
    tail -20 "$OUT_DIR/cmake.log"
    exit 1
fi
echo "CONFIGURE OK"

make -j"$JOBS" > "$OUT_DIR/build.log" 2>&1
if [ $? -ne 0 ]; then
    echo "BUILD FAILED"
    grep -E "error:" "$OUT_DIR/build.log" | head -20
    exit 1
fi
echo "BUILD OK"

TESTS="test_serializer test_protocol test_network test_thread_pool test_rpc \
test_proto_vs_tlv test_game_proto test_timer_manager test_game_room test_broadcast \
test_room_events test_game_e2e test_room_service test_input_buffer test_frame_sync \
test_game_state test_snapshot_manager test_frame_sync_flow test_match_queue"

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:log_path="$OUT_DIR/asan_"
pass=0
fail=0
failed=""
for t in $TESTS; do
    if timeout 300 "./$t" > "$OUT_DIR/out_$t.txt" 2>&1; then
        echo "OK        $t"
        pass=$((pass + 1))
    else
        echo "ASAN-FAIL $t (exit $?)"
        fail=$((fail + 1))
        failed="$failed $t"
    fi
done

echo ""
echo "ASAN TOTAL  pass=$pass fail=$fail"
[ -n "$failed" ] && echo "FAILED:$failed"
exit 0
