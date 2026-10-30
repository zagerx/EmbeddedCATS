# canopen/ — 内核层

平台无关的 CANopen 协议核心（DS 301）。**这是要长期保留并移植的资产。**

## 结构

```text
canopen/
├── include/canopen/   公共头（上层只能 include 这里）
└── src/               实现（nmt / sdo / pdo / sync / emcy）
```

## 职责

- NMT 状态管理与心跳监控
- SDO 客户端（读写对象字典、超时、abort 码上抛）
- PDO 配置与过程数据域
- SYNC 发生器
- EMCY 上报

## 边界

- `include/canopen/can_io.h` 是本层**定义**的 CAN 收发能力契约，由 `src/backends/` 实现；
- 本层不 include 任何平台头文件（`linux/can.h`、socket、线程、系统时钟均不得出现）；
- 本层不认识"轴""机构""轨迹"，只认识节点、对象与过程数据。

## 已实现

| 能力 | 位置 |
|---|---|
| CAN 收发契约（边界层实现） | `include/canopen/can_io.h` |
| SDO 加速读 | `include/canopen/sdo_client.h` + `src/sdo_client.c` |

未实现：NMT 状态机与心跳、SDO 写与分段传输、PDO、SYNC、EMCY。
