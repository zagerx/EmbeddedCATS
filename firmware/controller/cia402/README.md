# cia402/ — 实例化 CiA 402 设备状态机

一个 `struct cia402_device` 对应一个从站的非阻塞控制上下文。每个实例独立保存请求、状态机、传输进度和超时，不使用跨设备共享的可写状态。一个实例同时最多一个操作流程、一笔对象事务；一个操作流程可以依次包含多笔事务。

`enum cia402_outcome` 只表示一次操作的结局：`NONE` / `DONE` / `FAILED`。
`cia402_take_result()` 取走结局；取消不交付结局。
`struct cia402_failure_info` 独立保存失败诊断，只在 `FAILED` 后通过
`cia402_get_failure()` 读取 `fail` 与 `abort_code`。

设备不缓存驱动器实时状态。当前模式、PDS 状态、位置速度由控制器自行读取，
402 只提供对象索引；详见 `边界.md`。

没有 `mode_confirmed` / `enable_confirmed` 这类标志位：它们只是「上一次 CONFIG/ENABLE
成功了」的派生值，驱动器掉使能、换模式或掉电后都会与实际不符，判断依据必须来自对象读取。
同样地，`admit()` 不校验流程前提（先配置、先使能），只回答忙不忙，顺序责任在调用方。

普通调用方只包含 `device.h`，其中实例是不透明类型。轴层作为对象所有者包含 `device_instance.h`，仅用于提供静态存储；`operation`、`sdo`、`mailbox` 等字段由模块内部推进流程，所有者也不得直接读写。

## 结构

```text
cia402/
├── include/cia402/
│   ├── types.h    PDS 状态、控制字/状态字位、模式号
│   ├── od.h       DS402 对象号
│   ├── pds.h      状态字解析
│   ├── device.h   对外接口 + 请求/回执结构 + 不透明实例声明
│   └── device_instance.h  实例存储定义，仅供对象所有者与模块内部包含
└── src/
    ├── pds.c              纯标准知识
    ├── device.c           装配、service 推进、事件构造、读写原语、查询
    ├── device_internal.h  事件、超时常量、状态函数声明
    ├── states/          状态机骨架
    │   ├── states.h     状态函数公用工具
    │   ├── states.c     顶层初始转移
    │   ├── idle.c       无流程与流程收尾
    │   └── operation_active.c  操作进行中的公共父状态（取消在此统一退出）
    └── operations/      各操作的一串状态实现
        ├── config.c     写 6060 并回读 6061
        ├── enable.c     使能链（10 个状态）
        ├── disable.c    断使能
        ├── move.c       PP 参数、触发、到位
        ├── halt.c       减速停
        ├── homing.c     回零（HM 模式，9 个状态）
        └── quick_stop.c 快速停
```

层次只有三层：`sm_top → st_operation_active → 具体操作状态链`。`st_idle`
直接挂在 `sm_top` 下，不属于任何操作，因此不继承 `st_operation_active` 的"被抢占"语义。

回零多一层：`sm_top → st_operation_active → st_home_active → st_home_*`。
`st_home_active` 拦下 `CIA402_SIG_CANCEL` 改走中止链（停电机 → 落 bit4 →
切回 PP），而不是像其他操作那样直接被抢占到 `st_idle`——否则驱动器会带着
`6040` bit4 继续找原点。

## 对外接口

```c
void         cia402_init(struct cia402_device *d, struct sdo_client *client);
enum cia402_response cia402_request(struct cia402_device *d, const struct cia402_request *req);
void         cia402_poll(struct cia402_device *d, uint32_t now_ms);
void         cia402_get_failure(const struct cia402_device *d,
                                struct cia402_failure_info *out);
enum cia402_outcome cia402_take_result(struct cia402_device *d);
```

- **request 只说“收不收”**：`ACCEPTED` / `BUSY` / `INVALID`，不阻塞、不承诺成功，也不校验流程前提；
- **poll 只推进流程**：结论由 `cia402_take_result()` 消费，失败时再读取诊断；
- **反馈不属于本模块的公共输出**：控制器通过 TPDO 读取反馈，不占用 SDO 通道；
- **等待不是本层的职责**：想同步的调用方自己轮询 `outcome`。

"成功/失败"必须从 poll 出——request 返回的那一刻流程还没跑，它不可能知道结果。

状态机用 `hsm` 内核（与业务状态机同一套机制）。`cia402_poll()` 把
「邮箱、事务结果、取消」翻成事件分发：

| 事件 | 触发 | 谁处理 |
|---|---|---|
| `CIA402_SIG_REQUEST` | 邮箱里取出新请求（载荷是 `struct cia402_request`）| `st_idle`：按 kind 迁到该流程的第一个状态 |
| `CIA402_SIG_SDO_DONE` | 一笔事务有了结果 | 当前状态：判结果、决定下一步或收尾 |
| `CIA402_SIG_CANCEL` | 被 HALT / QUICK STOP 抢占，或通道交付超时 | `st_operation_active`：统一退出回 `st_idle` |
| `CIA402_SIG_TICK` | 每拍一次 | 空闲时消费；本模块不主动读取反馈 |

单笔事务的在途状态和期限由 `sdo_client` 持有。`dev_read()` / `dev_write()` 直接调用
`sdo_client_start()`，并检查受理结果；`cia402_poll()` 只在本设备
拥有已受理事务时调用 `sdo_client_poll()`，不会取走其他使用者的结果。

只有 `ACCEPTED` 才设置 `sdo_pending` 并等待结果；`BUSY / INVALID` 在状态 ENTRY 中记录
`sdo_unstarted`，下一拍由实际活动状态处理取消并回到空闲。HSM 内核不执行 ENTRY 返回的
二次转移，因此不能直接在 ENTRY 中要求回空闲。未受理不产生 `DONE / FAILED`，也不等待
虚假通信超时。
`TIMEOUT` 是已受理事务的真实失败；`CANCELLED` 只用于取消旧事务和继续安全请求，
不是 402 故障或操作结局。

一次 `poll` 最多发起一笔对象事务，因此CANopen 工作线程能在多个设备之间公平轮转，
一个设备在等应答不会占住循环。

`outcome` 保持到下一次请求被受理，或被 `cia402_take_result()` 取走；取走后为 NONE。
`fail` 与 `abort_code` 保留用于诊断。轴层不保存回执副本。

被抢占的旧流程不交付结局；事务超时属于真实失败，由 `st_idle` 收尾。
回零被抢占时仍完成停机与模式恢复，但不交付回零成功或失败。

急停是特例：`st_qstop_write` 只写一帧 `6040 = 0x0002`，收到写应答即进终态
`st_qstop_hold`；写失败只记 abort 码并报失败，**不重读状态字、不重试、不写断使能**。
驱动器停在哪个状态由它自己的 `605A` 决定（本项目驱动器出厂 6 = quick stop 状态，
仍保持使能），本层不推断也不跟踪。

终态只接受 `CIA402_REQ_ESTOP_CLEAR`。解除急停不碰驱动器，只把设备流程释放回空闲，
不写任何控制字。再使能必须由操作员刻意发起，复位不得导致重启。

配置、使能、PP 和停止均不阻塞。状态机通过异步 `sdo_client_*` 函数发起对象读写，不使用循环等待或条件变量。

使能按实际 6041 状态决定路径：

```text
Switch on disabled → 0x06 → Ready
Ready              → 0x07 → Switched on
Switched on        → 0x0F → Operation enabled
Fault              → 故障复位状态链
```

PP 依次写速度、加速度、减速度和目标位置，以 6040 bit4 边沿触发，再通过 6041 bit10 判断到位——到位只信驱动器的状态字，不再拿 6064 与目标比对。

Halt 与 Quick Stop 会抢占当前流程。CANopen 工作线程先取消在途事务并丢弃旧结果，
再启动暂停或急停状态。Halt 等 `6041` bit10 到位后清 bit8；Quick Stop 只写一帧
`6040 = 0x0002`，减速、断使能与抱闸全由驱动器（`605A = 2`）完成，之后进终态。
解除急停不碰驱动器，只把流程释放回空闲。

单次 SDO 超时与整个使能步骤、停车、PP 到位的业务期限分别管理。反馈不再占用
SDO 通道（改由 `canopen/pdo` 收 TPDO），通道由 402 流程独占。

两道保护，保证已受理事务一定有结果，未受理不会误报执行故障：

- **诚实受理**：`dev_read` / `dev_write` 显式检查 `ACCEPTED / BUSY / INVALID`。只有受理才等待结果；
  未受理立即清理当前流程，保留 `NONE` 结局，不生成虚假失败。
- **流程总期限**：请求受理时按类型设 `flow_deadline`（比该流程最长的单步期限再留余量），
  `st_operation_active` 在 TICK 里检查已受理流程是否超出业务期限。

## 依赖边界

```text
cia402/device → sdo_client（显式公共函数） ← canopen/sdo 实现 → CANopenNode
cia402/pds    → 无依赖
```

本模块不认识 CAN 帧、COB-ID、毫米单位或整机业务状态机。

## 自测

`apps/tools/cia402_selftest` 不需要 CAN 硬件，覆盖：

- PDS 状态字解析；
- 完整使能状态链；
- PP 参数、触发和到位；
- Quick Stop 抢占在途事务；
- 事务超时与失败发布；
- 未受理后回到空闲并可重新发起；
- 回零取消清理与旧失败抑制。
