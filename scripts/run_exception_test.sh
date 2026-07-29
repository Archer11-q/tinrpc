#!/bin/bash
# ============================================================
# run_exception_test.sh — 异常场景测试
#
# 用法: ./scripts/run_exception_test.sh [SERVER_IP] [PORT]
#
# 测试:
#   1. 大量断连: 100 连接稳态 → 瞬间断开 50 个
#   2. 恶意消息: 超大包 / 非法 Protobuf / 不存在房间
#   3. 验证 server 不崩溃且正确恢复
# ============================================================

set -euo pipefail

# 自动定位项目根目录
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SERVER_IP="${1:-127.0.0.1}"
SERVER_PORT="${2:-8080}"
BENCH_BIN="$PROJECT_ROOT/build/bench_game_client"
OUTPUT_DIR="$PROJECT_ROOT/docs/bench"
OUTPUT_FILE="$OUTPUT_DIR/05-exception-test.md"
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${GREEN}=== 异常场景测试 ===${NC}"
echo "服务端: $SERVER_IP:$SERVER_PORT"
echo "时间: $TIMESTAMP"
echo ""

if [ ! -f "$BENCH_BIN" ]; then
    echo -e "${RED}错误: $BENCH_BIN 不存在${NC}"
    exit 1
fi

if ! timeout 2 bash -c "echo >/dev/tcp/$SERVER_IP/$SERVER_PORT" 2>/dev/null; then
    echo -e "${RED}错误: 无法连接到 $SERVER_IP:$SERVER_PORT${NC}"
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

# ============================================================
# 1. chaos 模式: 断连 + 恶意消息
# ============================================================
echo -e "${YELLOW}[1/3] chaos 模式: 100连接 → 断连50% → 恶意消息${NC}"
$BENCH_BIN \
    --mode chaos \
    --server-ip "$SERVER_IP" \
    --port "$SERVER_PORT" \
    --conn 100 \
    --disconnect-pct 50 \
    --bad-msg-count 100 \
    --oversize-bytes 65536 \
    > "$OUTPUT_DIR/chaos_test.log" 2>&1

echo ""
echo -e "${CYAN}等待 10 秒让服务端恢复...${NC}"
sleep 10

# ============================================================
# 2. 验证服务端仍可用
# ============================================================
echo -e "${YELLOW}[2/3] 验证服务端仍可用（发送正常请求）...${NC}"
$BENCH_BIN \
    --mode single \
    --server-ip "$SERVER_IP" \
    --port "$SERVER_PORT" \
    --repeat 1 \
    --duration 10 \
    > "$OUTPUT_DIR/post_chaos_verify.log" 2>&1

echo ""

# ============================================================
# 3. 极端恶意消息测试（可选）
# ============================================================
echo -e "${YELLOW}[3/3] 极端恶意消息: 超大包(1MB)${NC}"
$BENCH_BIN \
    --mode chaos \
    --server-ip "$SERVER_IP" \
    --port "$SERVER_PORT" \
    --conn 10 \
    --disconnect-pct 0 \
    --bad-msg-count 50 \
    --oversize-bytes 1048576 \
    > "$OUTPUT_DIR/oversize_test.log" 2>&1

# ============================================================
# 生成报告
# ============================================================
cat > "$OUTPUT_FILE" << EOF
# 05 — 异常场景测试报告

> 生成时间: $TIMESTAMP
> 服务端: $SERVER_IP:$SERVER_PORT

---

## 1. 测试目的

验证服务端在异常场景下的稳定性：
- 大量客户端同时断连时的资源释放
- 恶意消息（超大包/非法数据/不存在房间）的处理
- 异常后是否能恢复正常服务

## 2. 测试场景

### 2.1 大量断连

| 项目 | 值 |
|------|-----|
| 初始连接数 | 100 |
| 断开连接数 | 50 (50%) |
| 断开方式 | 瞬间同时断开 |
| 预期 | server 正常释放资源，不影响剩余连接 |

### 2.2 恶意消息

| 测试项 | 数量 | 预期结果 |
|--------|------|----------|
| 超大包 (64KB) | 100 | 返回错误，不崩溃 |
| 非法 Protobuf | 100 | 返回 Parse Error，不崩溃 |
| 不存在房间 | 100 | 返回 ERR_ROOM_NOT_FOUND |

### 2.3 极端超大包

| 项目 | 值 |
|------|-----|
| 包大小 | 1MB |
| 数量 | 50 |
| 预期 | 返回错误或断开连接，不崩溃 |

## 3. 测试结果

### 3.1 大量断连

- 断连耗时: （从日志提取）
- 剩余活跃连接: （从日志提取）
- 错误数: （从日志提取）

### 3.2 恶意消息

- 服务端是否崩溃: （是/否）
- 各恶意消息返回的错误码: （从日志提取）

### 3.3 异常后恢复

- 是否成功发送正常消息: （是/否）
- 恢复耗时: （从日志提取）

## 4. 发现的问题

（待填入）

## 5. 修复记录

（待填入）
EOF

echo ""
echo -e "${GREEN}异常测试报告: $OUTPUT_FILE${NC}"
echo "日志文件:"
echo "  $OUTPUT_DIR/chaos_test.log"
echo "  $OUTPUT_DIR/post_chaos_verify.log"
echo "  $OUTPUT_DIR/oversize_test.log"
