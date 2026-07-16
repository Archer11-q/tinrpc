#!/bin/bash
# ============================================================
# 从 perf.data 生成火焰图 SVG
# 用法: bash gen_flame.sh [perf_data_path]
# ============================================================
set -e

PERF_DATA="${1:-/tmp/perf_v2.data}"
OUTPUT_DIR="$(dirname "$0")"
FOLDED_FILE="/tmp/perf_folded_fmt.txt"
FLAMEGRAPH_DIR="/tmp/FlameGraph"

# 1. 导出折叠格式调用栈（perf report -g folded 能正确解析 C++ 符号）
echo "[1/3] Extracting folded callgraph..."
timeout 15 perf report -i "$PERF_DATA" --stdio -g folded 2>&1 | grep ';' | \
    awk '{
        pct = $1
        gsub(/%/, "", pct)
        $1 = ""
        stack = $0
        sub(/^[[:space:]]+/, "", stack)
        count = int(pct * 100 + 0.5)
        if (count > 0) {
            print stack " " count
        }
    }' > "$FOLDED_FILE"

lines=$(wc -l < "$FOLDED_FILE")
echo "  -> $lines unique callchains"

# 2. 生成火焰图 SVG
echo "[2/3] Generating flame graph SVG..."
"$FLAMEGRAPH_DIR/flamegraph.pl" "$FOLDED_FILE" \
    --title="TinyRPC RPC Client CPU Flame Graph" \
    --width=1600 \
    --hash \
    --colors=java \
    > "$OUTPUT_DIR/flamegraph_rpc.svg"

svg_size=$(ls -lh "$OUTPUT_DIR/flamegraph_rpc.svg" | awk '{print $5}')
echo "  -> $OUTPUT_DIR/flamegraph_rpc.svg ($svg_size)"

# 3. 同时输出扁平热点排名
echo "[3/3] Top 20 functions (flat profile):"
perf report -i "$PERF_DATA" --stdio -g none --no-children 2>&1 | \
    grep -E '^\s+[0-9]+\.[0-9]+%' | head -20

echo ""
echo "Done! Open $OUTPUT_DIR/flamegraph_rpc.svg in browser."
