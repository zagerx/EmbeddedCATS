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
- `poll()`：逐拍推进并交付结果，返回 `DONE` / `WAITING` / `IDLE`；
- `abort()`：取消当前事务，取消结果仍通过 `poll()` 收取。

事务的在途状态与期限全部由通道持有：调用方不记录任何事务字段，
只负责「发起之后反复 poll 到 DONE」。期限口径统一在这里定义：
从真正上总线起算，排队不计时，到期由通道 abort 并以 `SDO_STATUS_TIMEOUT` 交付。

`sdo_client` 将该契约接到 CANopenNode。请求发起、协议推进和结果收取全部在
同一个 CANopen 工作线程内执行，不再有跨线程请求槽或互斥锁：

```text
cia402_poll()       → start() / poll()
sdo_client_step()   → CO_SDOclientUpload() / CO_SDOclientDownload()
```

每条通道同时最多一笔事务。`step()` 接收 epoll 提供的真实时间差和 `timerNext_us`，
在成功、失败和取消时生成结果，每笔事务结束关闭上游临时通信状态。

```text
canopen_sdo → sdo_access + CANopenNode
cia402      → sdo_access
```

CiA402 设备层不认识 CANopenNode，替换对象访问实现不需要修改设备状态机。
