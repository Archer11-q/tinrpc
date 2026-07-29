# CPU 热点分析 & 火焰图报告

> 生成时间: 2026-07-27 14:49:29
> 服务端: `/mnt/d/CLion/rpc/build/rpc`
> 构建类型: RelWithDebInfo (-fno-omit-frame-pointer -g)
> 工具: perf record -F 99 -g --call-graph dwarf -m 8M

---

## 1. 测试场景

| # | 场景 | 连接数 | 模式 | 持续时间 | ramp速率 | 火焰图 |
|---|------|--------|------|----------|----------|--------|
| 1 | 100-steady | 100 | steady | 180s | 20/s | [flamegraph_100-steady.svg](flamegraph_100-steady.svg) |
| 2 | 300-steady | 300 | steady | 180s | 30/s | [flamegraph_300-steady.svg](flamegraph_300-steady.svg) |
| 3 | 500-ramp | 500 | ramp | 120s | 50/s | [flamegraph_500-ramp.svg](flamegraph_500-ramp.svg) |

## 2. 各场景 Top 热点

### 2.1 100 连接稳态

```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 723  of event 'cpu/cycles/P'
# Event count (approx.): 13265245940
#
# Overhead  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     5.14%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |
            ---srso_alias_safe_ret
               |          
                --2.87%--tcp_rcv_established
                          tcp_v4_do_rcv
                          |          
                           --2.65%--tcp_v4_rcv
                                     ip_protocol_deliver_rcu
                                     ip_local_deliver_finish
                                     ip_local_deliver
                                     ip_rcv
                                     __netif_receive_skb_one_core
                                     __netif_receive_skb
                                     process_backlog
                                     __napi_poll
                                     net_rx_action
                                     handle_softirqs
                                     __do_softirq
                                     do_softirq.part.0
                                     __local_bh_enable_ip
                                     __dev_queue_xmit
                                     ip_finish_output2
                                     __ip_finish_output
                                     ip_finish_output
                                     ip_output
                                     ip_local_out
                                     __ip_queue_xmit
                                     ip_queue_xmit
                                     __tcp_transmit_skb
                                     tcp_write_xmit
                                     __tcp_push_pending_frames
                                     tcp_push
                                     tcp_sendmsg_locked
                                     tcp_sendmsg
                                     inet_sendmsg
                                     __sys_sendto
                                     __x64_sys_sendto
                                     x64_sys_call
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 723  of event 'cpu/cycles/P'
# Event count (approx.): 13265245940
#
#     Self  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     5.14%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |
            ---_start
               __libc_start_main_impl (inlined)
               __libc_start_call_main
               main
               game::GameService::Run(unsigned short)
               rpc::EventLoop::Run()
               |          
                --4.71%--OnRead (inlined)
                          |          
                           --4.23%--operator() (inlined)
                                     OnServerFrame (inlined)
                                     Send (inlined)
                                     __libc_send (inlined)
                                     __syscall_cancel
                                     __internal_syscall_cancel (inlined)
                                     |          
                                      --4.09%--entry_SYSCALL_64_after_hwframe
                                                do_syscall_64
                                                |          
                                                 --4.09%--x64_sys_call
                                                           __x64_sys_sendto
                                                           __sys_sendto
                                                           inet_sendmsg
                                                           tcp_sendmsg
                                                           |          
                                                            --3.87%--tcp_sendmsg_locked
                                                                      tcp_push
                                                                      __tcp_push_pending_frames
                                                                      tcp_write_xmit
                                                                      |          
                                                                       --3.69%--__tcp_transmit_skb
                                                                                 ip_queue_xmit
                                                                                 __ip_queue_xmit
                                                                                 ip_local_out
                                                                                 ip_output
                                                                                 |          
                                                                                  --3.49%--ip_finish_output
```

### 2.2 300 连接稳态

```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 2K of event 'cpu/cycles/P'
# Event count (approx.): 37152284298
#
# Overhead  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     4.17%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |
            ---srso_alias_safe_ret
               |          
                --0.57%--tcp_v4_rcv
                          ip_protocol_deliver_rcu
                          ip_local_deliver_finish
                          ip_local_deliver
                          ip_rcv
                          __netif_receive_skb_one_core
                          __netif_receive_skb
                          process_backlog
                          __napi_poll
                          net_rx_action
                          handle_softirqs
                          __do_softirq
                          do_softirq.part.0
                          __local_bh_enable_ip
                          __dev_queue_xmit
                          ip_finish_output2
                          __ip_finish_output
                          ip_finish_output
                          ip_output
                          ip_local_out
                          __ip_queue_xmit
                          ip_queue_xmit
                          __tcp_transmit_skb
                          tcp_write_xmit
                          __tcp_push_pending_frames
                          tcp_push
                          tcp_sendmsg_locked
                          tcp_sendmsg
                          inet_sendmsg
                          __sys_sendto
                          __x64_sys_sendto
                          x64_sys_call
                          do_syscall_64
                          entry_SYSCALL_64_after_hwframe
                          __internal_syscall_cancel (inlined)
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 2K of event 'cpu/cycles/P'
# Event count (approx.): 37152284298
#
#     Self  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     4.17%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |
            ---_start
               __libc_start_main_impl (inlined)
               __libc_start_call_main
               main
               game::GameService::Run(unsigned short)
               rpc::EventLoop::Run()
               |          
               |--3.59%--OnRead (inlined)
               |          |          
               |           --3.35%--operator() (inlined)
               |                     OnServerFrame (inlined)
               |                     Send (inlined)
               |                     __libc_send (inlined)
               |                     __syscall_cancel
               |                     __internal_syscall_cancel (inlined)
               |                     |          
               |                      --3.28%--entry_SYSCALL_64_after_hwframe
               |                                |          
               |                                 --3.23%--do_syscall_64
               |                                           x64_sys_call
               |                                           __x64_sys_sendto
               |                                           |          
               |                                            --3.15%--__sys_sendto
               |                                                      |          
               |                                                       --3.10%--inet_sendmsg
               |                                                                 tcp_sendmsg
               |                                                                 |          
               |                                                                  --2.74%--tcp_sendmsg_locked
               |                                                                            |          
               |                                                                             --2.59%--tcp_push
               |                                                                                       __tcp_push_pending_frames
               |                                                                                       |          
               |                                                                                        --2.54%--tcp_write_xmit
               |                                                                                                  |          
               |                                                                                                   --2.35%--__tcp_transmit_skb
               |                                                                                                             ip_queue_xmit
               |                                                                                                             __ip_queue_xmit
```

### 2.3 500 连接渐进

```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 2K of event 'cpu/cycles/P'
# Event count (approx.): 39375977645
#
# Overhead  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     3.89%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |
            ---srso_alias_safe_ret

     2.14%  [k] 0x00007fffc0004003                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               -      -            
            |
            ---0xffffffffc0000003
               __send_ipi_one
               hv_send_ipi
               native_send_call_func_single_ipi
               __smp_call_single_queue
               ttwu_queue_wakelist
               try_to_wake_up
               |          
                --2.08%--default_wake_function
                          ep_autoremove_wake_function
                          __wake_up_common
                          __wake_up_sync
                          ep_poll_callback
                          __wake_up_common
                          __wake_up_sync_key
                          sock_def_readable
                          tcp_data_ready
                          tcp_rcv_established
                          tcp_v4_do_rcv
                          tcp_v4_rcv
                          ip_protocol_deliver_rcu
                          ip_local_deliver_finish
                          ip_local_deliver
                          ip_rcv
                          __netif_receive_skb_one_core
                          __netif_receive_skb
                          process_backlog
                          __napi_poll
                          net_rx_action
                          handle_softirqs
                          __do_softirq
                          do_softirq.part.0
                          __local_bh_enable_ip
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 2K of event 'cpu/cycles/P'
# Event count (approx.): 39375977645
#
#     Self  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     3.89%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |          
             --3.89%--_start
                       __libc_start_main_impl (inlined)
                       __libc_start_call_main
                       main
                       game::GameService::Run(unsigned short)
                       rpc::EventLoop::Run()
                       |          
                       |--3.12%--OnRead (inlined)
                       |          |          
                       |          |--2.51%--operator() (inlined)
                       |          |          OnServerFrame (inlined)
                       |          |          Send (inlined)
                       |          |          __libc_send (inlined)
                       |          |          __syscall_cancel
                       |          |          __internal_syscall_cancel (inlined)
                       |          |          entry_SYSCALL_64_after_hwframe
                       |          |          do_syscall_64
                       |          |          x64_sys_call
                       |          |          __x64_sys_sendto
                       |          |          __sys_sendto
                       |          |          |          
                       |          |           --2.45%--inet_sendmsg
                       |          |                     tcp_sendmsg
                       |          |                     |          
                       |          |                      --1.73%--tcp_sendmsg_locked
                       |          |                                |          
                       |          |                                 --1.65%--tcp_push
                       |          |                                           __tcp_push_pending_frames
                       |          |                                           |          
                       |          |                                            --1.60%--tcp_write_xmit
                       |          |                                                      |          
                       |          |                                                       --1.49%--__tcp_transmit_skb
                       |          |                                                                 ip_queue_xmit
                       |          |                                                                 |          
                       |          |                                                                  --1.41%--__ip_queue_xmit
                       |          |                                                                            ip_local_out
                       |          |                                                                            ip_output
```

## 3. 火焰图解读指南

### 怎么看火焰图
- **X 轴宽度** = CPU 占比，越宽越热（按函数名字母排序，不是时间轴）
- **Y 轴高度** = 调用栈深度，从下到上是 caller → callee
- **颜色** = 随机，仅用于区分不同栈帧，无特殊含义
- **点击** = 在浏览器中可放大到某个函数子树
- **搜索框** = 右上角可高亮特定函数

### 常见瓶颈模式
| 火焰图形状 | 含义 |
|-----------|------|
| 平顶山 (plateau) | 某个函数自身耗时很宽 → 优化该函数本体 |
| 塔楼 (tower) | 深层调用链 → 考虑减少调用深度或内联 |
| 多塔并立 | 多个独立热点路径 → 逐个优化 |
| 细碎锯齿 | 大量小函数分散 → 可能是虚函数/间接调用开销 |

## 4. 对比分析

### 随负载变化的趋势

| 指标 | 100-steady | 300-steady | 500-ramp | 趋势 |
|------|-----------|-----------|---------|------|
| QPS (峰值) | - | - | - | |
| p99 延迟 (us) | - | - | - | |
| 热点函数 #1 | - | - | - | |
| 热点函数 #2 | - | - | - | |
| 热点函数 #3 | - | - | - | |

> 从对应的 bench_*.log 中提取 QPS 和延迟数据填入上表。

## 5. 优化建议

（根据火焰图分析结果填入）

---

## 附录: 生成火焰图的命令

```bash
# 单场景快速运行
perf record -F 99 -g --call-graph dwarf -m 8M -o /tmp/perf_rpc.data -- ./build/rpc &
# ... 运行压测客户端 ...
kill -INT $!  # SIGINT 让 perf 优雅结束

# 生成火焰图
perf script -i /tmp/perf_rpc.data | \
  ~/FlameGraph/stackcollapse-perf.pl | \
  ~/FlameGraph/flamegraph.pl --width 1600 > flamegraph.svg
```
