# transport/ — 对象字典访问与传输后端

## 结构

```text
transport/
├── include/transport/
│   ├── object_access.h  对象访问契约
│   └── sdo_client.h     CANopen SDO 后端接口
└── src/sdo_client.c
```

`object_access` 为设备层提供阻塞式 `read/write`，以及 `abort/now_ms/last_error`。它不认识 DS402 对象语义，也不要求底层必须是 CANopen。

`sdo_client` 使用 CANopenNode 客户端实现该契约：

- 服务线程提交一笔读写并等待条件变量；
- I/O 线程调用 `sdo_client_io_step()` 步进 SDO 协议栈；
- `sdo_client_abort()` 唤醒等待方，由 I/O 线程关闭在途事务；
- 每条通道同时只执行一笔事务。

依赖方向：

```text
canopen_sdo → object_access + CANopenNode
cia402      → object_access
```

因此 CiA 402 核心不依赖 CANopenNode，替换传输后端无需修改设备流程。
