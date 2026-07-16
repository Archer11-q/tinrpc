#!/bin/bash
set -e

# 清除并重启动服务端
pkill bench_server 2>/dev/null || true
sleep 0.5

/tmp/rpc_bench/bench_server --mode rpc --port 8081 &
sleep 1

# 验证服务存活
if ! ps aux | grep -q '[b]ench_server'; then
    echo "ERROR: Server failed to start"
    exit 1
fi

cd /tmp

# 长时间采样
echo "=== Sampling (4 threads x 2000 req) ==="
rm -f /tmp/perf_final.data
timeout 45 perf record -o /tmp/perf_final.data --call-graph fp,10 -F 999 \
    /tmp/rpc_bench/bench_client --mode rpc --port 8081 --threads 4 --requests 2000 --warmup 200 2>&1 | grep -E 'QPS|成功|失败'

echo ""
echo "Samples:"
perf report -i /tmp/perf_final.data --stdio -g none --no-children 2>&1 | grep -m1 'Samples:'
ls -lh /tmp/perf_final.data

# 导出折叠格式
echo ""
echo "=== Exporting folded callgraph ==="
timeout 15 perf report -i /tmp/perf_final.data --stdio -g folded 2>&1 | grep ';' | \
    awk '{
        pct = $1
        gsub(/%/, "", pct)
        $1 = ""
        stack = $0
        sub(/^[[:space:]]+/, "", stack)
        count = int(pct * 100 + 0.5)
        if (count > 0) print stack " " count
    }' > /tmp/perf_folded_final.txt

echo "Unique callchains: $(wc -l < /tmp/perf_folded_final.txt)"

# 生成火焰图
echo "=== Generating flame graph ==="
/tmp/FlameGraph/flamegraph.pl /tmp/perf_folded_final.txt \
    --title="TinyRPC Client CPU (fp unwind) - 4Tx2000req" \
    --width=1600 --hash --colors=java \
    > /mnt/d/CLion/rpc/perf/flamegraph_rpc.svg

echo "SVG: $(ls -lh /mnt/d/CLion/rpc/perf/flamegraph_rpc.svg | awk '{print $5}')"

# 扁平热点 Top 25
echo ""
echo "=== Top 25 functions (flat, self time) ==="
perf report -i /tmp/perf_final.data --stdio -g none --no-children 2>&1 | \
    grep -E '^\s+[0-9]+\.[0-9]+%' | head -25

echo ""
echo "=== Framework hotspots (self time) ==="
perf report -i /tmp/perf_final.data --stdio -g none --no-children 2>&1 | \
    grep -E 'rpc::|bench_server|bench_client.*[\.!\[]' | grep -v 'std::' | head -20

echo ""
echo "DONE: /mnt/d/CLion/rpc/perf/flamegraph_rpc.svg"
