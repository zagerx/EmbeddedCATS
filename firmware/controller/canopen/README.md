# canopen/ — 协议栈（DS 301 子集）

**只管协议，不知道这台机器上有几个从站。** 对标 CANopenNode 里 `CO_SDOclient` /
`CO_HBconsumer` / NMT 那一层：给定"站号 + 一帧字节 + 当前时刻"，它告诉你该发什么、
这帧是什么意思。

站号表、谁发帧、掉线多久算死，都在 `../master/`。

## 结构

```text
canopen/
├── include/canopen/
│   ├── cob.h        COB-ID 算式与反判（0x600/0x580/0x700 + id）
│   ├── nmt.h        NMT 命令 → 0x000 帧
│   ├── hb.h         心跳状态（纯时间戳；超时时长由调用方给）
│   └── sdo.h        SDO 客户端：站号 + 事务槽 + 注入的 tx 回调
└── src/
    ├── cob.c        COB-ID 算式（纯函数）
    ├── nmt.c        NMT 组帧（纯函数）
    ├── hb.c         心跳：记时刻、判"超时没"
    ├── sdo_client.c 受理事务 / 每拍把状态机要的帧交给 tx
    ├── sdo_channel.h/.c  一次事务的纯状态机（发起 / 收帧校验 / 超时 / abort）
    ├── sdo_frame.c  SDO 报文编解码（纯函数，可单测）
    └── internal.h   层内契约（上面两者的约定）
```

## 为什么 SDO 槽要带一个 tx 回调

"一个从站只有一对 `0x600/0x580`、同时只能有一个事务"是协议事实，所以事务槽跟站号
绑定（`struct co_sdo`）。但**帧往哪条总线走不是协议问题**，所以本层不 include `bus`：
主站在装配时注入 `co_sdo_tx_fn`，本层只把"该发的 8 个字节"交给它。

## 已实现

| 能力 | 位置 |
|---|---|
| COB-ID 映射（0x000 / 0x600+id / 0x580+id / 0x700+id） | `src/cob.c` |
| NMT 命令帧（`CO_NMT_START` / `CO_NMT_RESET_COMMUNICATION`） | `src/nmt.c` |
| 心跳 / boot-up 状态（0x700+id） | `src/hb.c` |
| SDO 客户端（非阻塞，一从站一个事务槽） | `include/canopen/sdo.h` + `src/sdo_client.c` |
| SDO 报文编解码（加速读/写、abort、分段识别） | `src/sdo_frame.c` |
| 编解码与事务槽单测（不需要 CAN） | `apps/tools/canopen_selftest/` |

没有 SDO server / 分段 / 块传输 / PDO / SYNC / EMCY / LSS。
`controller/third_party/CANopenNode` 保留作参考实现，**不参与构建**。

## 边界

- 不认识"有几个从站"、没有站号表（R9 检查）；
- 不认识 402 对象号、轴、业务状态（R2 检查）；
- 不 include 任何平台头文件（`linux/can.h`、socket、线程、系统时钟均不得出现），
  只经注入的 tx 发帧、只在参数里收时刻。

## 两条刻意的约定

1. **受理后下一拍就组帧发出**：`co_sdo_read/write` 只进事务槽，
   `co_sdo_advance()` 把状态机算出的帧立即交给 tx，不再多攒一拍。
2. **超时只判失败，不重试**：`co_sdo_abort_code()` 能区分"从站回的 abort"（真实码）
   与"本层判的超时"（`0x05040000`）。
