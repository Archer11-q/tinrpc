# 帧同步 CPU 热点分析 & 火焰图报告

> 生成时间: 2026-07-28 14:31:45
> 构建类型: RelWithDebInfo (-fno-omit-frame-pointer -g)

## 1. 测试场景

| # | 场景 | 客户端 | 房间(2人) | 帧率 | 持续时间 |
|---|------|--------|----------|------|---------|
| 1 | 100-fs | 100 | 50 | 20fps | 180s |
| 2 | 300-fs | 300 | 150 | 20fps | 180s |
| 3 | 500-fs | 500 | 250 | 20fps | 120s |

## 2. 压测链路

```
连接 → 登录 → 偶数号创建房间 → 奇数号加入房间
→ StartGame (启动 FrameSyncManager, 20fps Timer)
→ SendInput 循环 (每 50ms 发送 PlayerInputReq)
→ LeaveRoom → Disconnect
```

## 3. 各场景火焰图

| 场景 | 火焰图 |
|------|--------|
| 100-fs | [flamegraph_fs_100-fs.svg](flamegraph_fs_100-fs.svg) |
| 300-fs | [flamegraph_fs_300-fs.svg](flamegraph_fs_300-fs.svg) |
| 500-fs | [flamegraph_fs_500-fs.svg](flamegraph_fs_500-fs.svg) |

## 4. 各场景 Top 热点

### 100-fs

```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 1K of event 'cpu/cycles/P'
# Event count (approx.): 32150342002
#
# Overhead  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     4.00%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |
            ---srso_alias_safe_ret

     2.01%  [k] _raw_spin_lock_irqsave                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           -      -            
            |
            ---_raw_spin_lock_irqsave
               |          
               |--0.82%--ep_poll_callback
               |          __wake_up_common
               |          __wake_up_sync_key
               |          sock_def_readable
               |          tcp_data_ready
               |          tcp_rcv_established
               |          tcp_v4_do_rcv
               |          tcp_v4_rcv
               |          ip_protocol_deliver_rcu
               |          ip_local_deliver_finish
               |          ip_local_deliver
               |          ip_rcv
               |          __netif_receive_skb_one_core
               |          __netif_receive_skb
               |          process_backlog
               |          __napi_poll
               |          net_rx_action
               |          handle_softirqs
               |          __do_softirq
               |          do_softirq.part.0
               |          __local_bh_enable_ip
               |          __dev_queue_xmit
               |          ip_finish_output2
               |          __ip_finish_output
               |          ip_finish_output
               |          ip_output
               |          ip_local_out
               |          __ip_queue_xmit
               |          ip_queue_xmit
               |          __tcp_transmit_skb
               |          tcp_write_xmit
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 1K of event 'cpu/cycles/P'
# Event count (approx.): 32150342002
#
#     Self  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  ...................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     4.00%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              -      -            
            |          
             --4.00%--_start
                       __libc_start_main_impl (inlined)
                       __libc_start_call_main
                       main
                       game::GameService::Run(unsigned short)
                       rpc::EventLoop::Run()
                       |          
                        --3.89%--OnRead (inlined)
                                  |          
                                  |--3.12%--operator() (inlined)
                                  |          OnServerFrame (inlined)
                                  |          Send (inlined)
                                  |          __libc_send (inlined)
                                  |          __syscall_cancel
                                  |          __internal_syscall_cancel (inlined)
                                  |          |          
                                  |           --3.05%--entry_SYSCALL_64_after_hwframe
                                  |                     do_syscall_64
                                  |                     |          
                                  |                      --3.05%--x64_sys_call
                                  |                                __x64_sys_sendto
                                  |                                __sys_sendto
                                  |                                |          
                                  |                                 --2.91%--inet_sendmsg
                                  |                                           |          
                                  |                                            --2.70%--tcp_sendmsg
                                  |                                                      |          
                                  |                                                       --2.63%--tcp_sendmsg_locked
                                  |                                                                 |          
                                  |                                                                  --2.49%--tcp_push
                                  |                                                                            __tcp_push_pending_frames
                                  |                                                                            tcp_write_xmit
                                  |                                                                            |          
                                  |                                                                             --2.24%--__tcp_transmit_skb
                                  |                                                                                       ip_queue_xmit
                                  |                                                                                       __ip_queue_xmit
                                  |                                                                                       |          
```

### 300-fs

```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 4K of event 'cpu/cycles/P'
# Event count (approx.): 98467615682
#
# Overhead  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  .................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     4.05%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            -      -            
            |
            ---srso_alias_safe_ret

     2.28%  [.] game::InputBuffer::AddInput(unsigned int, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, std::vector<unsigned char, std::allocator<unsigned char> > const&)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           -      -            
            |          
            |--1.05%--__lower_bound<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, unsigned int, __gnu_cxx::__ops::_Iter_comp_val<game::InputBuffer::AddInput(uint32_t, const std::string&, const std::vector<unsigned char>&)::<lambda(const game::InputBuffer::FrameInput&, uint32_t)> > > (inlined)
            |          lower_bound<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, unsigned int, game::InputBuffer::AddInput(uint32_t, const std::string&, const std::vector<unsigned char>&)::<lambda(const game::InputBuffer::FrameInput&, uint32_t)> > (inlined)
            |          AddInput (inlined)
            |          SendInput (inlined)
            |          operator() (inlined)
            |          __invoke_impl<std::optional<std::vector<unsigned char> >, game::RegisterRoomService(rpc::Dispatch*, RoomService*)::<lambda(const std::vector<unsigned char>&)>&, const std::vector<unsigned char, std::allocator<unsigned char> >&> (inlined)
            |          __invoke_r<std::optional<std::vector<unsigned char> >, game::RegisterRoomService(rpc::Dispatch*, RoomService*)::<lambda(const std::vector<unsigned char>&)>&, const std::vector<unsigned char, std::allocator<unsigned char> >&> (inlined)
            |          _M_invoke (inlined)
            |          operator() (inlined)
            |          Call (inlined)
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
            |--0.52%--_Deque_iterator (inlined)
            |          
             --0.50%--operator+= (inlined)
                       __advance<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, long int> (inlined)
                       advance<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, long int> (inlined)
                       __lower_bound<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, unsigned int, __gnu_cxx::__ops::_Iter_comp_val<game::InputBuffer::AddInput(uint32_t, const std::string&, const std::vector<unsigned char>&)::<lambda(const game::InputBuffer::FrameInput&, uint32_t)> > > (inlined)
                       lower_bound<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, unsigned int, game::InputBuffer::AddInput(uint32_t, const std::string&, const std::vector<unsigned char>&)::<lambda(const game::InputBuffer::FrameInput&, uint32_t)> > (inlined)
                       AddInput (inlined)
                       SendInput (inlined)
                       operator() (inlined)
                       __invoke_impl<std::optional<std::vector<unsigned char> >, game::RegisterRoomService(rpc::Dispatch*, RoomService*)::<lambda(const std::vector<unsigned char>&)>&, const std::vector<unsigned char, std::allocator<unsigned char> >&> (inlined)
                       __invoke_r<std::optional<std::vector<unsigned char> >, game::RegisterRoomService(rpc::Dispatch*, RoomService*)::<lambda(const std::vector<unsigned char>&)>&, const std::vector<unsigned char, std::allocator<unsigned char> >&> (inlined)
                       _M_invoke (inlined)
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 4K of event 'cpu/cycles/P'
# Event count (approx.): 98467615682
#
#     Self  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  .................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     4.05%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            -      -            
            |
            ---_start
               __libc_start_main_impl (inlined)
               __libc_start_call_main
               main
               game::GameService::Run(unsigned short)
               rpc::EventLoop::Run()
               |          
                --3.87%--OnRead (inlined)
                          |          
                          |--3.01%--operator() (inlined)
                          |          OnServerFrame (inlined)
                          |          Send (inlined)
                          |          __libc_send (inlined)
                          |          __syscall_cancel
                          |          __internal_syscall_cancel (inlined)
                          |          |          
                          |           --2.93%--entry_SYSCALL_64_after_hwframe
                          |                     |          
                          |                      --2.91%--do_syscall_64
                          |                                |          
                          |                                 --2.89%--x64_sys_call
                          |                                           __x64_sys_sendto
                          |                                           |          
                          |                                            --2.87%--__sys_sendto
                          |                                                      |          
                          |                                                       --2.78%--inet_sendmsg
                          |                                                                 |          
                          |                                                                  --2.75%--tcp_sendmsg
                          |                                                                            |          
                          |                                                                             --2.61%--tcp_sendmsg_locked
                          |                                                                                       |          
                          |                                                                                        --2.46%--tcp_push
                          |                                                                                                  __tcp_push_pending_frames
                          |                                                                                                  |          
                          |                                                                                                   --2.43%--tcp_write_xmit
                          |                                                                                                             |          
                          |                                                                                                              --2.19%--__tcp_transmit_skb
```

### 500-fs

```
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 5K of event 'cpu/cycles/P'
# Event count (approx.): 119312838464
#
# Overhead  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  .................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     3.70%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            -      -            
            |
            ---srso_alias_safe_ret

     2.26%  [.] game::InputBuffer::AddInput(unsigned int, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, std::vector<unsigned char, std::allocator<unsigned char> > const&)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           -      -            
            |          
            |--1.31%--__lower_bound<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, unsigned int, __gnu_cxx::__ops::_Iter_comp_val<game::InputBuffer::AddInput(uint32_t, const std::string&, const std::vector<unsigned char>&)::<lambda(const game::InputBuffer::FrameInput&, uint32_t)> > > (inlined)
            |          lower_bound<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, unsigned int, game::InputBuffer::AddInput(uint32_t, const std::string&, const std::vector<unsigned char>&)::<lambda(const game::InputBuffer::FrameInput&, uint32_t)> > (inlined)
            |          AddInput (inlined)
            |          SendInput (inlined)
            |          operator() (inlined)
            |          __invoke_impl<std::optional<std::vector<unsigned char> >, game::RegisterRoomService(rpc::Dispatch*, RoomService*)::<lambda(const std::vector<unsigned char>&)>&, const std::vector<unsigned char, std::allocator<unsigned char> >&> (inlined)
            |          __invoke_r<std::optional<std::vector<unsigned char> >, game::RegisterRoomService(rpc::Dispatch*, RoomService*)::<lambda(const std::vector<unsigned char>&)>&, const std::vector<unsigned char, std::allocator<unsigned char> >&> (inlined)
            |          _M_invoke (inlined)
            |          operator() (inlined)
            |          Call (inlined)
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
             --0.57%--operator+= (inlined)
                       __advance<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, long int> (inlined)
                       advance<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, long int> (inlined)
                       __lower_bound<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, unsigned int, __gnu_cxx::__ops::_Iter_comp_val<game::InputBuffer::AddInput(uint32_t, const std::string&, const std::vector<unsigned char>&)::<lambda(const game::InputBuffer::FrameInput&, uint32_t)> > > (inlined)
                       lower_bound<std::_Deque_iterator<game::InputBuffer::FrameInput, game::InputBuffer::FrameInput&, game::InputBuffer::FrameInput*>, unsigned int, game::InputBuffer::AddInput(uint32_t, const std::string&, const std::vector<unsigned char>&)::<lambda(const game::InputBuffer::FrameInput&, uint32_t)> > (inlined)
                       AddInput (inlined)
                       SendInput (inlined)
                       operator() (inlined)
                       __invoke_impl<std::optional<std::vector<unsigned char> >, game::RegisterRoomService(rpc::Dispatch*, RoomService*)::<lambda(const std::vector<unsigned char>&)>&, const std::vector<unsigned char, std::allocator<unsigned char> >&> (inlined)
                       __invoke_r<std::optional<std::vector<unsigned char> >, game::RegisterRoomService(rpc::Dispatch*, RoomService*)::<lambda(const std::vector<unsigned char>&)>&, const std::vector<unsigned char, std::allocator<unsigned char> >&> (inlined)
                       _M_invoke (inlined)
                       operator() (inlined)
                       Call (inlined)
# To display the perf.data header info, please use --header/--header-only options.
#
#
# Total Lost Samples: 0
#
# Samples: 5K of event 'cpu/cycles/P'
# Event count (approx.): 119312838464
#
#     Self  Symbol                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           IPC   [IPC Coverage]
# ........  .................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................................  ....................
#
     3.70%  [k] srso_alias_safe_ret                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            -      -            
            |          
             --3.70%--_start
                       __libc_start_main_impl (inlined)
                       __libc_start_call_main
                       main
                       game::GameService::Run(unsigned short)
                       rpc::EventLoop::Run()
                       |          
                        --3.54%--OnRead (inlined)
                                  |          
                                  |--2.73%--operator() (inlined)
                                  |          OnServerFrame (inlined)
                                  |          |          
                                  |           --2.71%--Send (inlined)
                                  |                     __libc_send (inlined)
                                  |                     __syscall_cancel
                                  |                     __internal_syscall_cancel (inlined)
                                  |                     |          
                                  |                      --2.64%--entry_SYSCALL_64_after_hwframe
                                  |                                |          
                                  |                                 --2.62%--do_syscall_64
                                  |                                           |          
                                  |                                            --2.60%--x64_sys_call
                                  |                                                      __x64_sys_sendto
                                  |                                                      |          
                                  |                                                       --2.59%--__sys_sendto
                                  |                                                                 |          
                                  |                                                                  --2.53%--inet_sendmsg
                                  |                                                                            |          
                                  |                                                                             --2.51%--tcp_sendmsg
                                  |                                                                                       |          
                                  |                                                                                        --2.43%--tcp_sendmsg_locked
                                  |                                                                                                  |          
                                  |                                                                                                   --2.16%--tcp_push
                                  |                                                                                                             __tcp_push_pending_frames
                                  |                                                                                                             |          
                                  |                                                                                                              --2.11%--tcp_write_xmit
                                  |                                                                                                                        |          
```

## 5. 关键观察

- **FrameSyncManager::Tick** 是否成为热点？
- **InputBuffer** 乱序插入/弹出是否随客户端增长？
- **SnapshotManager** 快照创建开销？
- **GameState::tickLogic** 确定性更新成本？
- 帧数据广播 **BroadcastToRoom** 占比？
