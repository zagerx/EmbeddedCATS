# cia402/ — 实例化 CiA 402 设备状态机

一个 `struct cia402_device` 对应一个从站的非阻塞控制上下文。每个实例独立保存请求、状态机、传输进度和超时，不使用跨设备共享的可写状态。一个实例同时最多一个操作流程、一笔对象事务；一个操作流程可以依次包含多笔事务。

`struct cia402_status` 是主站依据通信流程作出的确认与操作回执，不是驱动器实时状态的完整镜像。外部反馈读取与缓存由控制器负责，402 只提供对象索引；详见 `边界.md`。

`mode_confirmed` 表示已写 `6060` 并回读 `6061` 确认 PP；`enable_confirmed` 表示使能流程曾确认成功且此后没有确认断使能。它们不会因为一次普通反馈读取而无条件改变。`operation_failed` 与 `last` 描述主站操作，不等同于驱动器的 Fault 状态。

普通调用方只包含 `device.h`，其中实例是不透明类型。轴层作为对象所有者包含 `device_instance.h`，仅用于提供静态存储；`operation`、`sdo`、`mailbox` 等字段由模块内部推进流程，所有者也不得直接读写。

## 结构

```text
cia402/
├── include/cia402/
│   ├── types.h    PDS 状态、控制字/状态字位、模式号
│   ├── od.h       DS402 对象号
│   ├── pds.h      状态字解析
│   ├── device.h   对外接口 + 请求/状态结构 + 不透明实例声明
│   └── device_instance.h  实例存储定义，仅供对象所有者与模块内部包含
└── src/
    ├── pds.c              纯标准知识
    ├── device.c           装配、service 推进、事件构造、读写原语、查询
    ├── device_internal.h  事件、超时常量、状态函数声明
    └── states/
        ├── states.h       状态函数公用工具
        ├── states.c       顶层初始转移
        ├── state_idle.c   st_idle：无流程与流程收尾
        ├── state_running.c  所有流程的父状态（取消在此统一退出）
        ├── state_config.c   写 6060 并回读 6061
        ├── state_enable.c   使能链（10 个状态）
        ├── state_disable.c  断使能
        ├── state_pp.c       PP 参数、触发、到位
        └── state_halt.c / state_quick_stop.c
```

层次只有三层：`sm_top → st_running → 具体流程状态`。`st_idle`
直接挂在 `sm_top` 下，不属于任何流程，因此不继承 `st_running` 的"被抢占"语义。

## 对外接口

```c
void         cia402_init(struct cia402_device *d, const struct sdo_access *access);
enum cia402_response cia402_request(struct cia402_device *d, const struct cia402_request *req);
void         cia402_poll(struct cia402_device *d, uint32_t now_ms,
                         struct cia402_status *out);
void         cia402_get_status(const struct cia402_device *d,
                               struct cia402_status *out);
enum cia402_operation_result cia402_take_result(struct cia402_device *d);
```

- **request 只说"收不收"**：`ACCEPTED` / `BUSY` / `INVALID` / `REFUSED`，不阻塞、不承诺成功；
- **poll 每拍推进一次并输出主站确认结果**：`phase` 是当前执行的操作，`mode_confirmed` / `enable_confirmed`
  是已确认的流程事实，`operation_failed` 是操作失败记录；`last.*` 是最近一次操作回执；
- **反馈不属于本模块的公共输出**：控制器通过对象索引自行发起 SDO，并管理读取结果；
- **等待不是本层的职责**：想同步的调用方自己轮询 `last.result`。

"成功/失败"必须从 poll 出——request 返回的那一刻流程还没跑，它不可能知道结果。

状态机用 `hsm` 内核（与业务状态机同一套机制）。`cia402_poll()` 把
「邮箱、事务结果、取消」翻成事件分发：

| 事件 | 触发 | 谁处理 |
|---|---|---|
| `CIA402_SIG_REQUEST` | 邮箱里取出新请求（载荷是 `struct cia402_request`）| `st_idle`：按 kind 迁到该流程的第一个状态 |
| `CIA402_SIG_SDO_DONE` | 一笔事务有了结果 | 当前状态：判结果、决定下一步或收尾 |
| `CIA402_SIG_CANCEL` | 被 HALT / QUICK STOP 抢占，或单笔事务超时 | `st_running`：统一退出回 `st_idle` |
| `CIA402_SIG_TICK` | 每拍一次 | 空闲时消费；本模块不主动读取反馈 |

一次 `poll` 最多发起一笔对象事务，因此CANopen 工作线程能在多个设备之间公平轮转，
一个设备在等应答不会占住循环。

`last.result` 保持到下一次请求被受理，或被 `cia402_take_result()` 取走；取走后为 NONE。
`last.fail` 与 `last.abort_code` 保留用于诊断。轴层不再保存 `status` 副本，而是通过
`cia402_get_status()` 读取主站确认结果、通过 `cia402_take_result()` 消费操作回执。

被打断时结论作废还是报失败，由 `st_idle` 的 ENTRY 按取消原因决定
（`st_running` 只负责退出，不碰结论文档）。正常收尾流程不进这条路径。

安全链是例外：`st_qstop_*` 三个状态自己处理 `CIA402_SIG_CANCEL`——写超时仍去读状态、
读超时仍去断使能，只有最后一步失败才报失败。安全动作不允许因为单笔事务超时
而停在"还在使能"的状态，所以它不继承 `st_running` 的统一退出。

配置、使能、PP 和停止均不阻塞。状态机通过异步 `sdo_access` 发起对象读写，不使用循环等待或条件变量。

使能按实际 6041 状态决定路径：

```text
Switch on disabled → 0x06 → Ready
Ready              → 0x07 → Switched on
Switched on        → 0x0F → Operation enabled
Fault              → 故障复位状态链
```

PP 依次写速度、加速度、减速度和目标位置，以 6040 bit4 边沿触发，再通过 6041 bit10 和必要的 6064 读数判断到位。

Halt 与 Quick Stop 会抢占当前流程。CANopen 工作线程先取消在途事务并丢弃旧结果，再启动停止状态链；Quick Stop 即使前序访问失败也继续尝试断使能。

单次 SDO 超时与整个使能步骤、停车、PP 到位的业务期限分别管理。外部反馈读取由控制器调度，经每轴仲裁器与控制流程共享通道；控制优先，结果不会跨会话交付。

## 依赖边界

```text
cia402/device → sdo_access ← transport/sdo_arbiter → sdo_client → CANopenNode
cia402/pds    → 无依赖
```

本模块不认识 CAN 帧、COB-ID、毫米单位或整机业务状态机。

## 自测

`apps/tools/cia402_selftest` 不需要 CAN 硬件，覆盖：

- PDS 状态字解析；
- 完整使能状态链；
- PP 参数、触发和到位；
- Quick Stop 抢占在途事务；
- 事务超时与失败发布。
