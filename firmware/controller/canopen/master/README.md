# master/ — CANopenLinux 主站生命周期与节点表

NMT、PDO 和 SDO 客户端来自 `controller/third_party/CANopenLinux/CANopenNode`。
SocketCAN、epoll、timerfd 与 eventfd 使用 CANopenLinux 的上游实现；不编译
`CO_main_basic.c`，也不运行 `canopend`。

本目录负责：

1. 按接口名创建并释放主站，检查所有初始化返回值并处理失败回滚；
2. 登记节点，第 i 个节点绑定对象字典第 i 条 SDO 客户端通道；
3. 提供 NMT（启动/复位节点）、eventfd 唤醒和通信复位重建接口。

对象字典 `0x1016` 的消费项均为 0，当前不启用从站心跳监控。

## 资源与调用约束

```c
master_init(&m, "can0");
master_add_node(&m, 1);
master_add_node(&m, 2);
/* 工作线程运行 controller_event_poll()，使用 m.ep 与 m.co */
master_notify(&m);              /* 其他线程唤醒 eventfd */
/* 工作线程退出并 join 后 */
master_deinit(&m);
```

| 约定 | 原因 |
|---|---|
| `CO_t` 动态分配 | 使用上游 Linux 动态对象模型；退出由 `CO_delete()` 释放 |
| 站号登记顺序与 OD 通道一致 | `0x1280` / `0x1281` 连续对应节点 1 / 2 |
| 本机站号为 `0x7F` | CANopenNode 要求有效的已配置站号 |
| 单工作线程 | `CO_SINGLE_THREAD=1`；协议栈、SDO 与 NMT 不跨线程调用 |
| 关闭 CAN 错误自动处理 | 防止上游错误处理自行执行 `ip link` 重启接口 |
| 通信复位显式重建 | 保留 `CO_t` 与 SDO 客户端地址，已装配对象不必重绑 |
| 广播命令也会打到自己 | 本机站号 `0x7F` 在总线上，广播的复位节点/复位通信都会让本机协议栈返回 `CO_RESET_APP` / `CO_RESET_COMM`，两者同样只需重建通信服务 |

本层不认识 CiA402 对象、轴或整机业务，也不创建线程。控制器负责在
`CO_epoll_processRT()` 与 `CO_epoll_processMain()` 之间推进各轴 SDO。
