# states/ — 控制器的业务状态图

本目录只包含状态函数、状态注册表和名称映射。控制器对象、输入事件队列、公共类型与推进入口均归控制层，不在本目录另设运行对象。

## 职责

- 输入事实：心跳、各轴配置/使能/故障与运动结论；
- 输出动作：通过 `controller_*` 动作函数与轴接口下发 NMT 与轴请求；
- `states.c` 注册状态函数，并提供状态与故障名称映射；
- `states.h` 集中声明状态函数，并提供从 HSM 成员找回 `struct controller` 的工具。

## 线程与边界

- 状态图只由控制线程通过 `controller_poll()` 推进；
- 状态函数使用 `controller` 中平铺的状态字段；
- 时钟与 NMT 只经 `controller_now_ms()` / `controller_nmt_*()` 请求，状态图不
  直接依赖主站或平台时钟；
- 不处理 402 对象号、SDO/PDO 或 CAN 帧。

## 加一个状态

1. 写 `state_xxx.c`；
2. 在 `states.h` 加声明；
3. 在 `states.c` 的 `states[]` / `NAMES[]` 各加一行，下标与 `enum controller_state` 对应。
