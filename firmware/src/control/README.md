# control/ — 控制层

整机控制主体 `cats_control`：状态机 + 指令槽 + 故障策略。

## 结构

```text
control/
├── include/control/   公共头（对外契约：启动、每拍推进、指令入口）
└── src/
    ├── control.c              装配与对外接口（不含状态逻辑）
    ├── control_internal.h     内部对象（struct cats_control）
    ├── hsm/                   状态机机制（状态即函数指针，与业务无关）
    └── states/                业务状态：一状态一文件，states.c 唯一注册点
```

## 职责

- 启动 / 停止顺序与失败回退
- 周期调度（唯一的节拍来源）
- 配置加载与校验
- 故障策略（通信超时、驱动器报警 → 系统动作）
- **唯一装配点**：所有对象的创建与注入只在这里发生

## 边界

- 只依赖 `axis`、`cia402`、`canopen` 的公共头，以及 `can_io.h` 接口；
- 不直接操作 PDO 字节、不直接调用平台 CAN 接口；
- 具体 backend 由上层（集成层）传入，本层不 `#include` 任何 backend 实现。

## 对外契约（`include/control/control.h`）

集成层只 include 这一个头，接口为：
`cats_control_init` / `cats_control_poll` / `cats_control_state` / `cats_control_state_name` /
`cats_control_move_to` / `cats_control_stop` / `cats_control_estop` /
`cats_control_fault_code` / `cats_control_fault_name` / `cats_control_axis_abort`。

- **单线程轮询**：由壳按 1 ms 调 `cats_control_poll()`，本层内部不用线程、不用回调；
- **启动不靠外部事件**：`cats_control_init()` 建好状态机即为 PREPARING，之后由 `poll` 自行推进；
- **单例**：一个进程一条总线，静态实例，不动态分配内存；
- **跨线程只共享 5 个字段**（指令槽 / 急停请求 / 两个发布值），由一把 `pthread_mutex_t` 保护；
- 时序、超时、状态全在本层及以下，壳只负责“什么时候调”。

> 状态机：PREPARING → ENABLING → HOLDING ⇄ MOVING，HALTING 回 HOLDING，
> FAULT / ESTOP 为终态（ESTOP 只能重启程序退出）。
>
> 层次：ROOT（统一故障出口）├─ PREPARING（不在 ONLINE 下：此时心跳还没有是正常的）
> └─ ONLINE（心跳丢失 → FAULT）├─ ENABLING / HOLDING / MOVING / HALTING。
