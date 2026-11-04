# canopen/ — 内核层

平台无关的 CANopen 协议核心（DS 301）。**这是要长期保留并移植的资产。**

协议栈本身用 CANopenNode v4.1（子模块 `src/third_party/CANopenNode`，只读不改）；
本层只做平台适配与封装。

## 结构

```text
canopen/
├── include/canopen/   公共头（上层只能 include 这里）
├── src/master.c       主站对外接口（CANopenNode 的薄封装）
└── src/node/          驱动适配：用 can_io 实现 CO_CANmodule
```

## 职责

- 按上游要求初始化协议栈，并把心跳回调翻译成"见过 boot-up / 被重启过"两个事实
- 每拍把总线帧分派给协议对象，推进协议栈
- 回答应用层查询：boot-up 是否到齐、是否都活着、有无重启

## 边界

- `include/canopen/can_io.h` 是本层**定义**的 CAN 收发能力契约，由 `src/backends/` 实现；
- 本层不 include 任何平台头文件（`linux/can.h`、socket、线程、系统时钟均不得出现）；
- 本层不认识"轴""机构""轨迹"，只认识节点、对象与过程数据。

## 已实现

| 能力 | 位置 |
|---|---|
| CAN 收发契约（边界层实现） | `include/canopen/can_io.h` |
| NMT 主站命令、心跳监控（超时由 OD 0x1016 决定） | `include/canopen/master.h` + `src/master.c` |
| SDO 客户端对象出口（只交出对象，不封装动作；按站号取，一节点一条通道） | `include/canopen/sdo_client.h` |
| 驱动适配（CO_CANmodule） | `src/node/co_driver_cats.c` |
| 对象字典（照抄上游 example，已配 0x1016 与 0x1280/0x1281 两个 SDO 客户端） | `config/canopen_node/` |

SDO 的发起/推进/取结果用上游原语，由 `cia402` 驱动；PDO、SYNC、EMCY 尚未接入。
