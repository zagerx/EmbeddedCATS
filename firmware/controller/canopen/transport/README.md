# transport/ — 显式函数接口的异步 SDO 客户端

## 结构

```text
transport/
├── include/transport/
│   ├── sdo_types.h           公共事务类型与结果
│   ├── sdo_client.h          显式读写、轮询、取消、推进接口
│   └── sdo_client_instance.h 实例存储；仅供所有者与真实实现包含
└── src/sdo_client.c
```

公开接口没有函数指针表，也没有跨线程请求队列：

- `sdo_client_read()` / `sdo_client_write()`：立即发起，返回 `ACCEPTED` / `BUSY` / `INVALID`；
- `sdo_client_step()`：由 CANopen 工作循环推进 CANopenNode 协议；
- `sdo_client_poll()`：取结果，返回 `DONE` / `WAITING` / `IDLE`；
- `sdo_client_abort()`：登记取消，由后续 `step()` 推进协议 abort 或执行有界本地收尾。

核心不变量：

- 只有 `ACCEPTED` 表示本笔事务已受理，之后必有一笔结果；`BUSY / INVALID` 未受理，不生成结果。
- 每条通道同时最多一笔事务。内部为 `IDLE → RUNNING → DONE → IDLE`，取走 `DONE` 才释放通道。
- 受理即开始计时，到期由通道尝试 abort，交付 `TIMEOUT`。
- 普通取消交付 `CANCELLED`，这是事务收尾状态，不是 402 操作失败，也不生成 402 成功。
  若正常完成与取消请求在同一拍发生，尊重已完成的协议事务并交付 `OK`。
- 取消收尾最多再等一笔事务期限；发送缓冲持续满时调用 `CO_SDOclientClose()` 释放本地通道。
  这个回退**不保证协议 abort 帧已发送成功**，只保证本地事务不会永久占用通道。
- `CO_SDOclientClose()` 只清理上游本地通信状态，不代替协议 abort；正常情况下仍由 `step()` 收尾。

所有接口在同一个 CANopen 工作线程内调用，不使用互斥锁：

```text
cia402 状态入口      → sdo_client_read() / sdo_client_write()
cia402_poll()        → sdo_client_poll()（只轮询自己已受理的事务）
controller 工作循环  → sdo_client_step() → CO_SDOclientUpload() / Download()
```

`sdo_client.h` 对普通调用方隐藏 `struct sdo_client` 的定义，不包含 `CANopen.h`。
轴作为所有者包含 `sdo_client_instance.h` 提供静态存储，组合根调用 `sdo_client_init()`。

```text
cia402      → sdo_transport（公共函数与类型）
canopen_sdo → sdo_transport + CANopenNode
axis        → sdo_client_instance（仅实例存储）
```

自测分两种：402/控制器测试提供同名 `sdo_client_*` 假实现；
`apps/tools/sdo_client_selftest` 直接编译真实 `sdo_client.c`，用测试专用 CANopenNode
协议桩覆盖正常完成、未受理、取消、超时及发送缓冲持续满的有界本地收尾。
两者都不需要 CAN 硬件。
