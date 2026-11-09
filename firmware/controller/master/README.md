# master/ — 主站实体

**一条总线 + 若干从站。** 这个目录回答"这台机器上有哪几个站、它们还活着吗、
我要让谁上线"；帧长什么样、命令字是几，全在 `../canopen/`。

```text
master/
├── include/master/master.h   主站句柄：站号表 / SDO 客户端 / 心跳状态
└── src/master.c              登记、收帧分派、每拍推进、唯一收发出口
```

## 结构

| 成员 | 含义 |
|---|---|
| `struct master.bus` | 唯一的收发通道（壳创建、只借用） |
| `struct master.node[]` | 每个已登记从站一份：`node_id` + `struct co_sdo` + `struct co_hb` |
| `MASTER_HB_TIMEOUT_MS` | 750 ms —— "本机愿意等多久"，不是协议常量 |

每从站一份 SDO 客户端，所以多站访问是并行的：一站的传输在途，不影响另一站。

## 对外

```c
master_init(&m, bus);
master_add_node(&m, 1);            /* 双轴驱动器 = 2 个从站 */
master_set_state(&m, 0, CO_NMT_START);       /* 0 = 全站 */
master_poll(&m, now_ms);           /* 抽帧 → 分派 → 推进各站事务 */
master_node_alive(&m, 1);          /* 0 = 在位 */
master_sdo(&m, 1);                 /* 该从站的对象访问通道，交给 drive */
```

## 边界

- 只依赖 `canopen`（协议栈）与 `bus`（一帧收发）；
- **不认识** 402、对象语义、轴、单位、业务状态机（R10 检查）；
- 不创建线程：`master_poll()` 由宿主的 I/O 线程按拍调用。

## 与 canopen/ 的分工

| 问题 | 归谁 |
|---|---|
| `0x580 + id` 是多少？心跳帧怎么判？ | `canopen/cob.c` |
| NMT 命令字是 `0x01` 还是 `0x82`？帧长几字节？ | `canopen/nmt.c` |
| SDO 事务怎么走、超时怎么补 abort？ | `canopen/sdo_channel.c` |
| 掉线多久算死？ | **master**（`MASTER_HB_TIMEOUT_MS`） |
| 这台机器有哪几个站？`0x581` 属于谁？ | **master**（站号表 + 分派） |
| 谁真正 `write()` 到总线？ | **master**（唯一出口） |
