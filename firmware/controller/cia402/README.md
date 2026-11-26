# cia402/ — 实例化 CiA 402 设备状态机

一个 `struct cia402_device` 对应一个从站的非阻塞控制上下文。每个实例独立保存请求、状态机、传输进度和超时，不使用跨设备共享的可写状态。一个实例同时最多一个操作流程、一笔对象事务；一个操作流程可以依次包含多笔事务。

`enum cia402_operation_result` 只表示一次操作的结果：`NONE` / `DONE` / `FAILED`。
`cia402_take_result()` 取走结局；取消不交付结局。
`cia402_failure_of()` 在 `FAILED` 后单独读取失败归类。

SDO abort 码属于 CiA 301 的协议知识，不往上层传：`dev_fail()` 收到对端 abort 时
直接打到 stderr（带对象号），上层只看得到归类。

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
    └── operations/      每个操作一个状态，内部按步骤号顺序推进
        ├── config.c     写 6060 并回读 6061（2 步）
        ├── enable.c     使能链（10 步）
        ├── disable.c    断使能（2 步）
        ├── move.c       PP 参数、触发、到位（7 步）
        ├── halt.c       减速停（3 步）
        ├── homing.c     回零主流程 4 步 + 中止链 4 步
        └── quick_stop.c 快速停
```

层次只有两层：`sm_top → st_operation_active → 具体操作`。`st_idle`
直接挂在 `sm_top` 下，不属于任何操作，因此不继承 `st_operation_active` 的"被抢占"语义。

**每个操作只占一个状态**，内部用 `operation.stage` 顺序推进一串事务：
`SM_SIG_ENTRY` 只记下第一步的 stage（不发起事务），`CIA402_SIG_TICK` 发起当前那一步，
`CIA402_SIG_SDO_DONE` 判结果并推进下一步，步骤枚举只存在于各自的 `operations` 文件里。
ENTRY 里发起事务是不行的：内核把 ENTRY 的返回值丢掉，发起失败时无法迁移。
为此每个操作只需要：

| 函数 | 职责 |
|---|---|
| `st_xxx` | 状态处理函数：转发 TICK / SDO_DONE 给 `xxx_step` |
| `xxx_step` | 判上一步结果：收尾返回目标状态，否则发起下一步并返回 NULL |
| `xxx_issue` | 按 stage 发起当前那笔事务 |
| `xxx_advance` / `xxx_judge` | 写成功后的下一步 / 读到值后的分叉判定 |

取消、超时看门狗、流程收尾仍由状态层次处理（顺序编程做不到"另一条控制流插进来"）。

回零是两个状态：`st_home` 跑主流程，`st_home_abort` 跑中止链（停电机 → 落 bit4 →
切回 PP）。`st_home` 自己拦下 `CIA402_SIG_CANCEL` 改道中止链，而不是像其他操作那样
直接被抢占到 `st_idle`——否则驱动器会带着 `6040` bit4 继续找原点。

## 对外接口

```c
void         cia402_init(struct cia402_device *d, struct sdo_client *client);
enum cia402_response cia402_request(struct cia402_device *d, const struct cia402_request *req);
void         cia402_poll(struct cia402_device *d, uint32_t now_ms);
void         cia402_failure_of(const struct cia402_device *d, enum cia402_fail *out);
enum cia402_operation_result cia402_take_result(struct cia402_device *d);
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
| `CIA402_SIG_TICK` | 每拍一次 | 当前状态：发起当前步骤的事务（`poll` 只在通道空闲、没有取消、没有待受理请求时送 TICK） |

单笔事务的在途状态和期限全部由 `sdo_client` 持有，`cia402` **不另外记账**。
`dev_read()` / `dev_write()` 直接调用 `sdo_client_start()` 并检查受理结果；
`cia402_poll()` 每拍无条件调用一次 `sdo_client_poll()`，用它的三种返回决定这一拍做什么：

| `sdo_client_poll()` | 含义 | 本拍动作 |
|---|---|---|
| `DONE` | 结果已取走、通道已释放 | 分发取消或 `SDO_DONE` |
| `WAITING` | 有一笔在途 | 什么都不做（要取消就先 `sdo_client_abort()`） |
| `IDLE` | 通道空闲 | 分发取消 / 取新请求 / 送 `TICK` |

`IDLE` 时 `sdo_client_poll()` 只读状态、不修改任何东西，因此每拍调用是安全的。

`BUSY / INVALID` 在发起的那一时刻就当场判失败：`TICK` 的返回值内核接受，可以直接迁回
`st_idle` 并交出一个真实结局。通道如果真的丢了已受理事务，`flow_deadline` 看门狗会把
它兜成真实的 `TIMEOUT` 结局，不需要本层自己再记一份在途标志。
`TIMEOUT` 是已受理事务的真实失败；`CANCELLED` 只用于取消旧事务和继续安全请求，
不是 402 故障或操作结局。

`cia402_poll()` 只在本设备没有在跑流程时才从邮箱取新请求：跑着的时候取出来也没有
状态会受理，只会把已经接受的 HALT / 急停悄悄丢掉。抢占靠 `CANCEL` 实现，请求留在
邮箱里，等流程真的回到空闲再取。

一次 `poll` 最多发起一笔对象事务，因此CANopen 工作线程能在多个设备之间公平轮转，
一个设备在等应答不会占住循环。

`outcome` 保持到下一次请求被受理，或被 `cia402_take_result()` 取走；取走后为 NONE。
`fail` 保留用于诊断。轴层不保存回执副本。

被抢占的旧流程不交付结局；事务超时属于真实失败，由 `st_idle` 收尾。
回零被抢占时仍完成停机与模式恢复，但不交付回零成功或失败。

急停是特例：`st_qstop_write` 只写一帧 `6040 = 0x0002`，收到写应答即进终态
`st_qstop_hold`；写失败只报失败，**不重读状态字、不重试、不写断使能**。
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
Fault              → 故障复位链（同一状态内的步骤）
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
  未受理当场收尾，归类为 `CIA402_FAIL_NOT_STARTED`，不冒充超时或 SDO 失败。
- **流程总期限**：请求受理时按类型设 `flow_deadline`（比该流程最长的单步期限再留余量），
  `st_operation_active` 在 TICK 里检查已受理流程是否超出业务期限。

失败归类只回答“这次为什么没成”，不等于“设备故障”：

| 归类 | 含义 |
|---|---|
| `CIA402_FAIL_NONE` | 未发生失败（零值，不冒充任何原因） |
| `CIA402_FAIL_TIMEOUT` | 期限内没等到预期结果 |
| `CIA402_FAIL_DRIVER` | 驱动器报 PDS FAULT——唯一的设备故障 |
| `CIA402_FAIL_HOMING` | 回零自身报错（6041 bit13） |
| `CIA402_FAIL_SDO` | 已受理事务以对端 abort 或协议错误结束 |
| `CIA402_FAIL_CHECK` | 回读值与请求不符 |
| `CIA402_FAIL_NOT_STARTED` | 流程内事务未受理：这一步没执行 |

是否锁存成整机故障由 controller 决定，402 不判等级。

## 依赖边界

```text
cia402/device → sdo_client（显式公共函数） ← canopen/sdo 实现 → CANopenNode
cia402/pds    → 无依赖
```

本模块不认识 CAN 帧、COB-ID、毫米单位或整机业务状态机。

## 自测

`apps/tools/cia402_selftest` 不需要 CAN 硬件，覆盖：

- PDS 状态字解析；
- 完整使能链；
- PP 参数、触发和到位；
- Quick Stop 抢占在途事务；
- 事务超时与失败发布；
- 未受理后回到空闲并可重新发起；
- 回零取消清理与旧失败抑制。
