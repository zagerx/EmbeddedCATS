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

- `start()`：立即发起一笔读写并返回受理结果；
- `take_result()`：有结果时取走，没有结果时立即返回；
- `abort()`：取消当前事务，取消结果仍通过 `take_result()` 收取。

`sdo_client` 将该契约接到 CANopenNode。请求发起、协议推进和结果收取全部在
同一个 CANopen 工作线程内执行，不再有跨线程请求槽或互斥锁：

```text
cia402_poll()       → start() / take_result()
sdo_client_step()   → CO_SDOclientUpload() / CO_SDOclientDownload()
```

每条通道同时最多一笔事务。`step()` 接收 epoll 提供的真实时间差和 `timerNext_us`，
在成功、失败和取消时生成结果，每笔事务结束关闭上游临时通信状态。

```text
canopen_sdo → sdo_access + CANopenNode
cia402      → sdo_access
```

CiA402 设备层不认识 CANopenNode，替换对象访问实现不需要修改设备状态机。
