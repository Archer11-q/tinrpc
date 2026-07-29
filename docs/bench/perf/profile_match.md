# 匹配系统 CPU 热点分析 & 火焰图报告

> 生成时间: 2026-07-28 17:13:52
> 构建类型: RelWithDebInfo (-fno-omit-frame-pointer -g)

## 1. 测试场景

| # | 场景 | 客户端 | 模式 | 持续时间 |
|---|------|--------|------|---------|
| 1 | 100-match | 100 | EnterMatch→CancelMatch 循环 | 180s |
| 2 | 300-match | 300 | EnterMatch→CancelMatch 循环 | 180s |
| 3 | 500-match | 500 | EnterMatch→CancelMatch 循环 | 120s |

## 2. 压测链路

```
服务端:
  连接 → 登录
  → EnterMatch(随机ELO) → EnterQueue(二分插入排序)
  → TryMatch(批量配对: 遍历队列 + 分差比较 + 双方出队)
  → OnMatchFound(创建房间 + 通知双方 + 超时)
  → CancelMatch(清理队列) → 循环
```

## 3. 压力模块

| 模块 | 压力点 |
|------|--------|
| MatchQueue::EnterQueue | 二分插入 (std::lower_bound + insert) — 每次入队 |
| MatchQueue::TryMatch | 全队列遍历配对 (O(n)) — 每次 EnterMatch 触发 |
| MatchQueue::CancelMatch | 线性查找 + erase (O(n)) — 每次取消 |
| EloCalculator | ELO 分计算 — 每次配对 |
| MatchQueue::FindMatch | 单个匹配搜索 — 超时扫描 |
| TryMatch 中的 sort | 配对后重新排序 |

## 4. 各场景火焰图

| 场景 | 火焰图 |
|------|--------|
| 100-match | [flamegraph_match_100-match.svg](flamegraph_match_100-match.svg) |
| 300-match | [flamegraph_match_300-match.svg](flamegraph_match_300-match.svg) |
| 500-match | [flamegraph_match_500-match.svg](flamegraph_match_500-match.svg) |

## 5. 各场景 Top 热点

### 100-match

```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 724  of event 'cpu/cycles/P'
# Event count (approx.): 11292781491
#
# Overhead  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     2.24%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |
            ---srso_alias_safe_ret

     1.98%  [k] 0x00007fffc0004003                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               -      -            
            |
            ---0xffffffffc0000003
               __send_ipi_one
               hv_send_ipi
               native_send_call_func_single_ipi
               __smp_call_single_queue
               ttwu_queue_wakelist
               try_to_wake_up
               |          
                --1.65%--default_wake_function
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
# Samples: 724  of event 'cpu/cycles/P'
# Event count (approx.): 11292781491
#
#     Self  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     2.24%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |          
             --2.06%--_start
                       __libc_start_main_impl (inlined)
                       __libc_start_call_main
                       main
                       game::GameService::Run(unsigned short)
                       rpc::EventLoop::Run()
                       |          
                        --1.80%--OnRead (inlined)
                                  operator() (inlined)
                                  OnServerFrame (inlined)
                                  Send (inlined)
                                  __libc_send (inlined)
                                  __syscall_cancel
                                  __internal_syscall_cancel (inlined)
                                  entry_SYSCALL_64_after_hwframe
                                  do_syscall_64
                                  x64_sys_call
                                  __x64_sys_sendto
                                  |          
                                   --1.65%--__sys_sendto
                                             |          
                                              --1.50%--inet_sendmsg
                                                        tcp_sendmsg
                                                        |          
                                                         --1.10%--tcp_sendmsg_locked
                                                                   tcp_push
                                                                   __tcp_push_pending_frames
                                                                   tcp_write_xmit
                                                                   |          
                                                                    --0.92%--__tcp_transmit_skb
                                                                              ip_queue_xmit
                                                                              __ip_queue_xmit
                                                                              ip_local_out
                                                                              ip_output
                                                                              ip_finish_output
                                                                              __ip_finish_output
                                                                              ip_finish_output2
```

### 300-match

```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 1K of event 'cpu/cycles/P'
# Event count (approx.): 32260463241
#
# Overhead  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     3.17%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |
            ---srso_alias_safe_ret

     2.06%  [k] tcp_ack                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          -      -            
            |
            ---tcp_ack
               tcp_rcv_established
               tcp_v4_do_rcv
               |          
               |--1.04%--__release_sock
               |          release_sock
               |          tcp_sendmsg
               |          inet_sendmsg
               |          __sys_sendto
               |          __x64_sys_sendto
               |          x64_sys_call
               |          do_syscall_64
               |          entry_SYSCALL_64_after_hwframe
               |          __internal_syscall_cancel (inlined)
               |          __syscall_cancel
               |          __libc_send (inlined)
               |          Send (inlined)
               |          OnServerFrame (inlined)
               |          operator() (inlined)
               |          OnRead (inlined)
               |          rpc::EventLoop::Run()
               |          game::GameService::Run(unsigned short)
               |          main
               |          __libc_start_call_main
               |          __libc_start_main_impl (inlined)
               |          _start
               |          
                --1.03%--tcp_v4_rcv
                          ip_protocol_deliver_rcu
                          ip_local_deliver_finish
                          ip_local_deliver
                          ip_rcv
                          __netif_receive_skb_one_core
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 1K of event 'cpu/cycles/P'
# Event count (approx.): 32260463241
#
#     Self  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     3.17%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |          
             --3.17%--_start
                       __libc_start_main_impl (inlined)
                       __libc_start_call_main
                       main
                       game::GameService::Run(unsigned short)
                       rpc::EventLoop::Run()
                       |          
                       |--2.45%--OnRead (inlined)
                       |          |          
                       |           --2.08%--operator() (inlined)
                       |                     OnServerFrame (inlined)
                       |                     Send (inlined)
                       |                     __libc_send (inlined)
                       |                     __syscall_cancel
                       |                     __internal_syscall_cancel (inlined)
                       |                     |          
                       |                      --2.04%--entry_SYSCALL_64_after_hwframe
                       |                                do_syscall_64
                       |                                |          
                       |                                 --1.98%--x64_sys_call
                       |                                           __x64_sys_sendto
                       |                                           __sys_sendto
                       |                                           |          
                       |                                            --1.91%--inet_sendmsg
                       |                                                      |          
                       |                                                       --1.86%--tcp_sendmsg
                       |                                                                 |          
                       |                                                                  --1.50%--tcp_sendmsg_locked
                       |                                                                            tcp_push
                       |                                                                            __tcp_push_pending_frames
                       |                                                                            tcp_write_xmit
                       |                                                                            |          
                       |                                                                             --1.41%--__tcp_transmit_skb
                       |                                                                                       ip_queue_xmit
                       |                                                                                       |          
                       |                                                                                        --1.35%--__ip_queue_xmit
                       |                                                                                                  ip_local_out
```

### 500-match

```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 2K of event 'cpu/cycles/P'
# Event count (approx.): 33947842767
#
# Overhead  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     2.60%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |
            ---srso_alias_safe_ret

     2.13%  [k] tcp_ack                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          -      -            
            |
            ---tcp_ack
               tcp_rcv_established
               tcp_v4_do_rcv
               |          
               |--1.19%--__release_sock
               |          release_sock
               |          tcp_sendmsg
               |          inet_sendmsg
               |          __sys_sendto
               |          __x64_sys_sendto
               |          x64_sys_call
               |          do_syscall_64
               |          entry_SYSCALL_64_after_hwframe
               |          
                --0.94%--tcp_v4_rcv
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
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 2K of event 'cpu/cycles/P'
# Event count (approx.): 33947842767
#
#     Self  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     2.60%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |          
             --2.30%--entry_SYSCALL_64_after_hwframe
                       do_syscall_64
                       |          
                        --2.30%--x64_sys_call
                                  |          
                                   --1.70%--__x64_sys_sendto
                                             __sys_sendto
                                             inet_sendmsg
                                             tcp_sendmsg
                                             |          
                                              --1.54%--tcp_sendmsg_locked
                                                        |          
                                                         --1.50%--tcp_push
                                                                   __tcp_push_pending_frames
                                                                   |          
                                                                    --1.44%--tcp_write_xmit
                                                                              |          
                                                                               --1.31%--__tcp_transmit_skb
                                                                                         ip_queue_xmit
                                                                                         __ip_queue_xmit
                                                                                         ip_local_out
                                                                                         ip_output
                                                                                         ip_finish_output
                                                                                         __ip_finish_output
                                                                                         ip_finish_output2
                                                                                         __dev_queue_xmit
                                                                                         |          
                                                                                          --1.19%--__local_bh_enable_ip
                                                                                                    do_softirq.part.0
                                                                                                    __do_softirq
                                                                                                    |          
                                                                                                     --1.15%--handle_softirqs
                                                                                                               |          
                                                                                                                --1.10%--net_rx_action
                                                                                                                          |          
                                                                                                                           --1.05%--__napi_poll
                                                                                                                                     process_backlog
```

## 6. 关键观察

- **EnterQueue** 二分插入排序是否随队列长度成为瓶颈？
- **TryMatch** O(n) 遍历是否随并发恶化？
- **CancelMatch** 线性查找 erase 的占比？
- **FindMatch** 单独匹配搜索的成本？
- TryMatch 内部的 **sort/erase** 批量操作？
