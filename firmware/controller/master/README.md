# master/ — 主站实体（CANopenNode 的薄壳）

**协议栈不再是我们自己写的**：NMT、心跳消费、SDO 客户端全部来自
`controller/third_party/CANopenNode`（v4，Apache-2.0，原样参与构建）。

本目录只做三件"本机才有的事"：

1. 把总线接到 CO_driver 的移植层（`../canopen_node/co_driver_cats.c`）；
2. **站号表**：这台机器登记了哪几个从站，第 i 个登记 ⇒ 第 i 条 SDO 客户端通道
   （对应对象字典 `0x1280` / `0x1281`）；
3. 每拍两段：`master_recv_step()` 抽帧交给栈，`master_send_step()` 调 `CO_process()`。

它**不认识** 402、对象语义、"轴"这些业务词（R10 检查）。

## 对外

```c
master_init(&m, bus);                                  /* CO_new / CO_CANinit / CO_CANopenInit */
master_add_node(&m, 1);                                /* 第 1 个 ⇒ OD 0x1280 那条通道 */
master_add_node(&m, 2);                                /* 第 2 个 ⇒ OD 0x1281 那条通道 */
master_set_state(&m, 0, MASTER_NMT_START);             /* 0 = 全站 */
master_recv_step(&m, now_ms);                          /* ① 收 */
   /* ② 宿主在这两段之间搬 SDO 请求/结果 */
master_send_step(&m, now_ms);                          /* ③ CO_process：超时 + 发帧 */
master_node_alive(&m, 1);                              /* 0 = 心跳 ACTIVE */
master_sdo(&m, 1);                                     /* 该从站的 CO_SDOclient_t* */
```

## 关键约定

| 约定 | 原因 |
|---|---|
| `CO_USE_GLOBALS=1` | 栈用静态实例，零 malloc，和项目风格一致 |
| 站号 0 登记顺序必须与 OD 通道顺序一致 | 栈按 `ENTRY_H1280` **连续取** `CNT_SDO_CLI` 条 |
| 主站自己的站号 `MASTER_NODE_ID=0x7F` | CANopenNode 在节点号未配置时 `CO_process()` 直接返回 |
| **栈只在 I/O 线程里被调用** | 栈是单线程设计：收发 + SDO 步进都在 `master_*_step` 和 `od_sdo_io_step` 里 |
| 心跳超时来自 OD `0x1016`（本项目 750 ms） | CANopenNode 的模型：策略写在对象字典里 |

## 本层不知道

- 402 对象号、控制字/状态字、PDS 状态（`cia402`）；
- "轴"、"mm"、业务状态机（`axis` / `statemachine`）；
- 线程与节拍（宿主决定）。
