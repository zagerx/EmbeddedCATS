# controller_states/ — 控制器的业务状态图

本目录只包含状态函数、状态注册表和名称映射。控制器对象、输入事件队列、公共类型与推进入口均归控制层，不在本目录另设运行对象。

## 职责

- 输入事实：各轴配置/使能/故障与运动结论；
- 输出动作：通过主站 NMT 接口与轴接口下发通信与轴请求；
- `states.c` 注册状态函数，并提供状态与故障名称映射；
- `states.h` 集中声明状态函数，并提供从 HSM 成员找回 `struct controller` 的工具。

## 状态流

```text
PREPARING → CONFIGURING → IDLE ──CONTROLLER_SIG_ENABLE──> ENABLING → HOLDING ⇄ MOVING
                                                          ↑              │
                                                          └─ HALTING ────┘
FAULT / ESTOP 为终态。
```

`IDLE` 表示"已配置、未使能"：配置完成后停在这里，只有 `controller_enable()` 投递的
使能事件才继续。状态函数名是 `st_idle_ctrl`，因为 `st_idle` 已被 cia402 设备层占用。

## 线程与边界

- 状态图只由 CANopen 工作线程推进：对外入口是 `controller_event_poll()`，每轮内部
  调一次 `controller_poll()` 分发事件并投递节拍；
- 状态函数使用 `controller` 中平铺的状态字段；
- 时钟经 `controller_now_ms()` 取得，NMT 直接调 `master_request_nmt()`；状态图不
  依赖 CANopenNode 头、SDO 传输层或平台 socket；
- 不处理 402 对象号、SDO/PDO 或 CAN 帧。

## 加一个状态

1. 写 `state_xxx.c`；
2. 在 `states.h` 加声明；
3. 在 `states.c` 的 `states[]` / `NAMES[]` 各加一行，下标与 `enum controller_state` 对应。
