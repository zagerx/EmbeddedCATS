# statemachine/ — 本机的业务状态图

**只做一件事：拿事实、做决定、下请求。**

- 输入事实：心跳是否正常、各轴是否已配置/已使能/有故障/运动结论
- 输出动作：NMT 启停（组合根注入的回调）、轴的配置/使能/运动/停

## 结构

```text
statemachine/
├── include/statemachine/
│   ├── state_types.h     业务枚举（状态 / 故障码 / 结果）
│   └── statemachine.h    stm_init / stm_poll / stm_command / 事实查询
└── src/
    ├── statemachine.c            事件入队、每拍推进、事实读取
    ├── statemachine_internal.h   struct stm（不对外）
    ├── evt_queue/                业务事件队列（多生产者 / 单消费者）
    └── states/                   一状态一文件 + 唯一注册点 states.c
```

状态机**机制**不在本目录：通用内核在 `../hsm/`（`cats_hsm`，零业务依赖）。

## 线程与边界

- **只在控制线程里被推进**：`stm_poll()` 每拍一次；
- `stm_command()` 任意线程可调（入队，队列自带锁）；
- **不知道** 402、对象号、SDO/PDO、CAN 帧。

## 加一个状态

1. 写 `states/state_xxx.c`；
2. `states/states.h` 加声明；
3. `states.c` 的 `states[]` / `NAMES[]` 各加一行（下标 = `enum cats_state`）。
