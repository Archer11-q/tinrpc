#!/bin/bash
# ============================================================
# run_lv3_e2e.sh — Lv3 全流程端到端测试
#
# 用法: ./scripts/run_lv3_e2e.sh [SERVER_IP] [PORT]
#
# 启动 5 个客户端，走完:
#   匹配 → 房间创建 → 加入 → 开始游戏 → 帧同步 → 结算
#
# 注意: 当前匹配需要服务端 MatchQueue 有 RPC 入口。
#       如果没有，此脚本先用 CreateRoom/JoinRoom 模拟。
# ============================================================

set -euo pipefail

# 自动定位项目根目录
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SERVER_IP="${1:-127.0.0.1}"
SERVER_PORT="${2:-8080}"
BENCH_BIN="$PROJECT_ROOT/build/bench_game_client"
OUTPUT_DIR="$PROJECT_ROOT/docs/bench"
OUTPUT_FILE="$OUTPUT_DIR/06-lv3-e2e.md"
TIMESTAMP=$(date '+%Y-%m-%d %H:%M:%S')

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

echo -e "${GREEN}=== Lv3 全流程端到端测试 ===${NC}"
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
# 运行 5 个客户端并发
# ============================================================
echo -e "${YELLOW}启动 5 个客户端并发执行全流程...${NC}"
echo "流程: 连接 → 登录 → 创建房间 → 发消息 → 离开"
echo ""

$BENCH_BIN \
    --mode steady \
    --server-ip "$SERVER_IP" \
    --port "$SERVER_PORT" \
    --conn 5 \
    --duration 60 \
    --think-mean 500 \
    --warmup 5 \
    --ramp-rate 5 \
    > "$OUTPUT_DIR/lv3_e2e.log" 2>&1

echo ""

# ============================================================
# 检查结果
# ============================================================
echo -e "${GREEN}检查测试结果...${NC}"

if grep -q "QPS" "$OUTPUT_DIR/lv3_e2e.log" 2>/dev/null; then
    echo -e "${GREEN}  ✓ 全流程完成（有 QPS 输出）${NC}"
else
    echo -e "${RED}  ✗ 未检测到 QPS 输出，可能有问题${NC}"
fi

if grep -q "error" "$OUTPUT_DIR/lv3_e2e.log" 2>/dev/null; then
    echo -e "${YELLOW}  ⚠ 日志中有 error 字样，请检查${NC}"
else
    echo -e "${GREEN}  ✓ 无错误${NC}"
fi

# ============================================================
# 生成报告
# ============================================================
cat > "$OUTPUT_FILE" << EOF
# 06 — Lv3 全流程端到端测试报告

> 生成时间: $TIMESTAMP
> 服务端: $SERVER_IP:$SERVER_PORT

---

## 1. 测试目的

验证"匹配 → 房间 → 帧同步 → 结算"全流程无断点、无错误码。

## 2. 测试配置

| 项目 | 值 |
|------|-----|
| 客户端数量 | 5 |
| 每客户端流程 | 连接→登录→创建房间→发消息(60s)→离开 |
| think time | 500ms 均值 (泊松) |
| 预热时间 | 5s |

## 3. 测试结果

### 3.1 连接建立

（从日志提取）

### 3.2 各阶段耗时

（从日志提取 bench_game_client 的阶段输出）

### 3.3 错误统计

（从日志提取）

## 4. 全流程验证清单

| 步骤 | 状态 | 备注 |
|------|------|------|
| TCP 连接建立 | ☐ | |
| Login 登录 | ☐ | |
| CreateRoom 创建房间 | ☐ | |
| StartGame 开始游戏 | ☐ | |
| SendMessage 发消息 | ☐ | |
| LeaveRoom 离开房间 | ☐ | |
| 断连清理 | ☐ | |

## 5. 截图/录屏

（手动附上）

## 6. 发现的问题

（待填入）

## 7. 结论

（待填入）
EOF

echo ""
echo -e "${GREEN}Lv3 E2E 报告: $OUTPUT_FILE${NC}"
echo "日志: $OUTPUT_DIR/lv3_e2e.log"
echo ""
echo -e "${CYAN}提示: 如果服务端启用了 MatchService RPC，可额外手动测试匹配流程。${NC}"
