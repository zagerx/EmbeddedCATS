# transport/ — 对象字典访问与传输后端

## 结构

```text
transport/
├── include/transport/
│   ├── od_access.h  对象访问契约
│   └── od_sdo.h     CANopen SDO 后端接口
└── src/od_sdo.c
```

`od_access` 为设备层提供阻塞式 `read/write`，以及 `abort/now_ms/last_error`。它不认识 DS402 对象语义，也不要求底层必须是 CANopen。

`od_sdo` 使用 CANopenNode 客户端实现该契约：

- 服务线程提交一笔读写并等待条件变量；
- I/O 线程调用 `od_sdo_io_step()` 步进 SDO 协议栈；
- `od_sdo_abort()` 唤醒等待方，由 I/O 线程关闭在途事务；
- 每条通道同时只执行一笔事务。

依赖方向：

```text
canopen_sdo → od_access + CANopenNode
cia402      → od_access
```

因此 CiA 402 核心不依赖 CANopenNode，替换传输后端无需修改设备流程。
