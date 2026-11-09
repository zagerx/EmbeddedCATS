# ds402/ — DS 402 标准知识（纯层）

**零依赖、零流程**：不 include `canopen`/`bus`，不碰时间/线程/总线/单位，
里面没有一个循环。谁都能链它，也能脱离硬件单测。

这里是"402 长什么样"的唯一定义处：状态枚举、控制字/状态字位、对象号、模式号，
以及**把"使能"写成几步**的那两张序列表。

## 结构

```text
ds402/
├── include/ds402/
│   ├── types.h   状态枚举（从站 PDS 状态的投影）+ 控制字/状态字位 + 模式号
│   ├── od.h      对象号（全工程唯一出处）
│   └── pds.h     parse / name / 序列表 / 起点快进
└── src/pds.c
```

## 三个函数

| 函数 | 回答什么问题 |
|---|---|
| `ds402_parse(sw)` | 这串状态字（6041）是哪个状态？ |
| `ds402_seq_enable(&n)` / `ds402_seq_disable(&n)` | 要走哪些步、每步写什么控制字、等什么状态字 |
| `ds402_seq_skip(sw, seq, n)` | 现在走到第几步了（起点快进） |

序列表就是标准写死的东西：

```c
/* enable：0x06 → 0x07 → 0x0F */
{DS402_CW_SHUTDOWN,  0x6F, 0x21},   /* → Ready to switch on */
{DS402_CW_SWITCH_ON, 0x6F, 0x23},   /* → Switched on */
{DS402_CW_ENABLE_OP, 0x6F, 0x27},   /* → Operation enabled */
```

**起点快进**让"上次停在半路"也能使能：读到的状态字已经满足某一步，就从它的下一步开始。

| 当前状态字 | `seq_skip` | 效果 |
|---|---|---|
| `0x0040` Switch on disabled | 0 | 从 06 开始 |
| `0x0021` Ready | 1 | 跳过 06，直接 07 |
| `0x0023` Switched on | 2 | 直接 0F |
| `0x0027` 已使能 | 3 | 一帧不发，直接成功 |

## 本层不知道

- 什么时候该发命令、等多久、失败了怎么办（`drive` 决定）；
- 命令怎么送出去（`drive/backends/od_sdo.c`：SDO 阻塞读写）；
- "轴"、"mm"、"使能"这些业务词（`axis` / `statemachine`）。

## 单测

`apps/tools/ds402_selftest`：状态字解析 + 四种起点的 `seq_skip`，纯逻辑不需要 CAN。
