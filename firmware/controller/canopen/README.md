# canopen/ — 内核层

平台无关的 CANopen 主站（DS 301 子集）。**这是要长期保留并移植的资产。**

协议栈是**本层自己实现的**：只做工程真正用到的三件事 —— NMT 命令、心跳消费、
SDO 客户端（加速传输）。没有 SDO server / PDO / SYNC / EMCY / LSS。
`controller/third_party/CANopenNode` 保留作参考实现，**不再参与构建**。

## 结构

```text
canopen/
├── include/canopen/          公共头（上层只能 include 这里）
│   ├── master.h              主站：登记节点 / NMT / 心跳 / 每拍推进
│   └── node.h                节点：读/写一个对象（非阻塞）
└── src/
    ├── internal.h            层内契约（服务之间怎么调）
    ├── master.c              装配 + 收帧分派 + 每拍推进 + NMT 命令帧 + 唯一发帧出口
    ├── heartbeat.c           心跳/boot-up 判定（纯时间戳）
    ├── node.c                node.h 的实现（就绪 / 忙 / 交结果）
    ├── sdo_channel.h/.c      一节点一个事务槽：发起 / 收帧校验 / 超时 / abort
    └── sdo_frame.c           SDO 报文编解码（纯函数，可单测）
```

## 职责

- 每拍把总线抽干，按 COB-ID 分派给心跳或对应节点的 SDO 事务；
- 推进各节点的在途事务（超时判定）；
- 回答上层查询：某个节点还活着吗、某个对象读到了吗。

## 边界

- 收发契约 `<bus/can_io.h>` 归 `controller/bus/`（本层的**下层**），本层只使用不定义；
- 本层不 include 任何平台头文件（`linux/can.h`、socket、线程、系统时钟均不得出现）；
- 本层不认识"轴""机构""轨迹"，只认识节点、COB-ID 与对象号。

## 已实现

| 能力 | 位置 |
|---|---|
| CAN 收发契约（归 bus 层） | `controller/bus/include/bus/can_io.h` |
| NMT 状态切换（0x000） | `src/master.c` 的命令字映射 |
| 心跳 / boot-up（0x700+id，超时 `CO_MASTER_HB_TIMEOUT_MS`） | `src/heartbeat.c` |
| 节点对象访问（非阻塞，一节点一个事务槽，由主站每拍推进） | `include/canopen/node.h` + `src/node.c` + `src/sdo_channel.c` |
| SDO 报文编解码（加速读/写、abort、分段识别） | `src/sdo_frame.c` |
| 编解码单测（不需要 CAN） | `apps/tools/canopen_selftest/` |

## 对象访问归本层

DS 301 的 SDO 是 CANopen 自带的服务："一个节点只有一对 `0x600/0x580`、
同时只能有一个事务"是协议事实，所以**事务槽跟着节点走**
（`struct co_node`，由 `co_master_add_node()` 初始化），
**推进由 `co_master_poll()` 统一做**——上层不需要知道"传输是步进的"。

上层（`drive`）只说"读/写哪个对象"，拿到的也是字节数组；对象含义由 402 那套标准解释。

## 两条与上游不同的约定（都是刻意的）

1. **请求当场发出**：`co_node_read/write` 里直接组帧发送，不再"下一拍才发"，
   省掉一拍延迟。
2. **超时只判失败，不补发 abort 帧**：`co_node_abort_code()` 里能区分
   "从站回的 abort"（真实码）与"本层判的超时"（`0x05040000`）。
