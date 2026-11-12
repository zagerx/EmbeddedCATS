# transport/ — 异步对象访问与 SDO 后端

## 结构

```text
transport/
├── include/transport/
│   ├── sdo_access.h  异步对象访问契约
│   └── sdo_client.h  CANopenNode 客户端封装
└── src/sdo_client.c
```

`sdo_access` 提供三个操作：

- `start()`：排队一笔读写，立即返回；
- `take_result()`：有结果时取走，没有结果时立即返回；
- `abort()`：请求取消当前事务，取消结果仍通过 `take_result()` 收取。

`sdo_client` 将该契约接到 CANopenNode：

```text
服务线程：start() / take_result()
I/O 线程：sdo_client_io_poll() → CO_SDOclient*()
```

每条通道同时最多一笔事务。互斥锁只保护通道字段，不用于等待；I/O 线程独占协议栈，并在成功、失败和取消时生成结果。每笔事务结束都会关闭 CANopenNode 的本次通信状态。

```text
canopen_sdo → sdo_access + CANopenNode
cia402      → sdo_access
```

CiA 402 设备层不认识 CANopenNode，替换对象访问实现不需要修改设备状态机。
