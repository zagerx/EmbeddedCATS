# drive/ — 一个 402 从站的运行期

**一个 `drive` = 一对 `0x600/0x580` = 一个轴。**
物理双轴驱动器（2LD2-CAN7020B）= 2 个从站 = **2 个 `drive` 实例**。

它把三样东西装配成一个能用的设备对象：DS 402 标准知识（`ds402`）、
对象访问抽象（`od_access`）、模式插件（`mode_ops`）。

## 结构

```text
drive/
├── include/drive/
│   ├── drive.h       请求 / 事实 / service
│   ├── od_access.h   对象访问抽象（drive 唯一认识的"传输"）
│   ├── mode.h        struct mode_ops
│   └── od_sdo.h      od_access 的 SDO 实现（服务线程 ↔ I/O 线程）
└── src/
    ├── drive.c       请求受理、服务执行、反馈轮转
    ├── drive_sm.c    run_to(目标状态) + 对象读写
    ├── mode_pp.c     PP 模式（参数下发 + 到位判定）
    └── backends/od_sdo.c
```

## 三条规则

1. **请求不阻塞调用者**：`drive_request_*` / `drive_pp_start` 只往请求槽里放一件事；
2. **一次只执行一件事**，槽里已有请求或正在执行 → 后来者得到 `1`（忙）；
   例外是"停"类动作：`drive_halt` / `drive_quick_stop` **抢占**在途运动；
3. **反馈轮转只在空闲时进行**，避免和业务事务抢同一对 COB-ID。

## 线程归属

| 线程 | 调什么 |
|---|---|
| I/O 线程 | `od_sdo_io_step()`（推进事务、收帧分派） |
| 服务线程 | `drive_service()`、`od_access.read/write`（阻塞） |
| 任意线程 | `drive_request_*` / 事实查询 |

## 本层不知道

- CAN 帧、COB-ID（`canopen`）；
- 传输是 SDO 还是 PDO（`od_access` 后面）；
- 业务状态机、单位（`statemachine` / `axis`）。
