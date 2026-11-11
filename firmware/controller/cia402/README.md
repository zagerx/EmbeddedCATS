# cia402/ — 实例化 CiA 402 设备状态机

一个 `struct cia402_device` 对应一个从站。每个实例独立保存请求、状态机、传输进度、超时和反馈，不使用跨设备共享的可写状态。

## 结构

```text
cia402/
├── include/cia402/
│   ├── types.h    PDS 状态、控制字/状态字位、模式号
│   ├── od.h       DS402 对象号
│   ├── pds.h      状态解析与序列表
│   └── device.h   对外接口 + 请求/状态/反馈结构 + 内部分组的设备实例
└── src/
    ├── pds.c              纯标准知识
    ├── device.c           装配、service 推进、事件构造、读写原语、查询
    ├── device_internal.h  事件、超时常量、状态函数声明
    └── states/
        ├── states.h       状态函数公用工具
        ├── states.c       顶层初始转移
        ├── state_idle.c   st_idle / st_monitor
        ├── state_running.c  所有流程的父状态（取消在此统一退出）
        ├── state_config.c   写 6060 并回读 6061
        ├── state_enable.c   使能链（10 个状态）
        ├── state_disable.c  断使能
        ├── state_pp.c       PP 参数、触发、到位
        └── state_halt.c / state_quick_stop.c
```

层次只有三层：`sm_top → st_running → 具体流程状态`。`st_idle` 与 `st_monitor`
直接挂在 `sm_top` 下，不属于任何流程，因此不继承 `st_running` 的"被抢占"语义。

## 对外接口

```c
void         cia402_init(struct cia402_device *d, const struct sdo_access *access,
                         uint8_t node_id);
enum cia402_ret cia402_request(struct cia402_device *d, const struct cia402_request *req);
void         cia402_poll(struct cia402_device *d, uint32_t now_ms,
                         struct cia402_status *out);
void         cia402_get_feedback(const struct cia402_device *d,
                                 struct cia402_feedback *out);
```

- **request 只说"收不收"**：`ACCEPTED` / `BUSY` / `INVALID` / `REFUSED`，不阻塞、不承诺成功；
- **poll 每拍推进一次并输出流程状态**：`phase`、`result`、`fail`、`abort_code` 与能力事实；
- **get_feedback 独立读取原始反馈**：状态字、位置、速度和驱动器错误码；
- **等待不是本层的职责**：想同步的调用方自己轮询 `result`。

"成功/失败"必须从 poll 出——request 返回的那一刻流程还没跑，它不可能知道结果。

状态机用 `hsm` 内核（与业务状态机同一套机制）。`cia402_poll()` 把
「请求槽、事务结果、取消」翻成事件分发：

| 事件 | 触发 | 谁处理 |
|---|---|---|
| `CIA402_SIG_REQUEST` | 请求槽里取出新请求（载荷是 `struct cia402_request`）| `st_idle`：按 kind 迁到该流程的第一个状态 |
| `CIA402_SIG_SDO_DONE` | 一笔事务有了结果 | 当前状态：判结果、决定下一步或收尾 |
| `CIA402_SIG_CANCEL` | 抢占 / 超时 / 手动取消 | `st_running`：统一退出回 `st_idle` |
| `CIA402_SIG_TICK` | 每拍一次 | `st_idle`：到间隔才进 `st_monitor` 读反馈 |

一次 `poll` 最多发起一笔对象事务，因此服务线程能在多个设备之间公平轮转，
一个设备在等应答不会占住循环。

`result` 与 `fail` **保持到下一次请求被受理**：受理新请求即作废旧结论。
`axis` 层把 poll 的结果缓存在 `struct cats_axis` 里，并且只在 `result != NONE` 时
覆盖——控制线程 20 ms 一拍、服务线程 1 ms 一拍，否则会错过那一次结论。

被抢占或超时时结论作废还是报失败，由 `st_idle` 的 ENTRY 按取消原因决定
（`st_running` 只负责退出，不碰结论文档）。

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

Halt 与 Quick Stop 会抢占当前流程。服务线程先取消在途事务并丢弃旧结果，再启动停止状态链；Quick Stop 即使前序访问失败也继续尝试断使能。

单次 SDO 超时与整个使能步骤、停车、PP 到位的业务期限分别管理。反馈只在设备空闲时轮转读取。

## 依赖边界

```text
cia402/device → sdo_access ← transport/sdo_client → CANopenNode
cia402/pds    → 无依赖
```

本模块不认识 CAN 帧、COB-ID、毫米单位或整机业务状态机。

## 自测

`apps/tools/cia402_selftest` 不需要 CAN 硬件，覆盖：

- PDS 状态字解析与序列起点快进；
- 完整使能状态链；
- PP 参数、触发和到位；
- Quick Stop 抢占在途事务；
- 事务超时与失败发布；
- 独立反馈读取。
