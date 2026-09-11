#!/usr/bin/env bash
# ============================================================
# 全量测试：19 个 target，统计通过/失败 + 断言总数
#
# 用法：
#   ./scripts/run_all_tests.sh                # 默认用 build/
#   BUILD_DIR=/tmp/mybuild ./scripts/run_all_tests.sh
#
# 注意：跑之前请先 make，陈旧二进制会给出误导性的"通过"结果。
# ============================================================
set -u

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${BUILD_DIR:-"$SCRIPT_DIR/../build"}

cd "$BUILD_DIR" || {
    echo "构建目录不存在: $BUILD_DIR（先 cmake .. && make）"
    exit 1
}

TESTS="test_serializer test_protocol test_network test_thread_pool test_rpc \
test_proto_vs_tlv test_game_proto test_timer_manager test_game_room test_broadcast \
test_room_events test_game_e2e test_room_service test_input_buffer test_frame_sync \
test_game_state test_snapshot_manager test_frame_sync_flow test_match_queue"

pass=0
fail=0
asserts=0
failed=""

for t in $TESTS; do
    out=$(timeout 120 "./$t" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        pass=$((pass + 1))
        n=$(printf '%s\n' "$out" | grep -Eo '[0-9]+ passed' | tail -1 | grep -Eo '[0-9]+')
        [ -z "$n" ] && n=0
        asserts=$((asserts + n))
        printf 'PASS  %-26s %s\n' "$t" "$n"
    else
        fail=$((fail + 1))
        failed="$failed $t"
        printf 'FAIL  %-26s (exit %s)\n' "$t" "$rc"
    fi
done

echo "-----------------------------------------------"
echo "TARGETS  pass=$pass fail=$fail   ASSERTIONS=$asserts"
[ -n "$failed" ] && echo "FAILED:$failed"
exit 0
