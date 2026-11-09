# drive/ — 一个 402 从站的运行期

**一个 `drive` = 一个从站 = 一对 `0x600/0x580` = 一个轴。**
物理双轴驱动器（2LD2-CAN7020B）= 2 个从站 = **2 个 `drive` 实例**。

## 模型：意图投递 + 服务线程同步执行

```text
任意线程    drive_request_enable() / drive_pp_start() / drive_halt()
                └─ 只写请求槽，立刻返回（不阻塞调用者）
服务线程    drive_service()
                └─ 取出请求，同步跑完整条流程：写控制字 → 读状态字 → 下一步
                   期间阻塞在对象读写上（condvar 睡眠，让出 CPU，不空转）
任意线程    drive_is_enabled() / drive_feedback_*() / drive_take_result()
安全动作    drive_cancel() 打断在途流程：等待点立刻返回
```

## 结构

```text
drive/
├── include/drive/
│   ├── drive.h       意图 / 事实 / 结论
│   ├── od_access.h   对象访问抽象（read / write / abort / now_ms / last_error）
│   └── od_sdo.h      od_access 的 SDO 实现（服务线程 ↔ I/O 线程）
└── src/
    ├── drive.c       请求槽、drive_service()、反馈轮转
    ├── drive_seq.c   402 流程：唯一一处"写控制字 + 等状态字"
    ├── drive_pp.c    PP：参数下发 + bit4 沿 + 等 bit10
    └── backends/od_sdo.c
```

## 三条规则

1. **意图只入槽**：`drive_request_*` 不阻塞调用者，服务线程取走执行；
2. **一次只做一件事**，槽里已有东西或正在执行 → 后来者得 `1`（忙）；
   例外是安全动作（`drive_halt` / `drive_quick_stop`）：先 `cancel` 在途，再占槽；
3. **反馈轮转只在空闲时进行**，避免和业务事务抢同一对 COB-ID。

## 402 流程怎么走

`drive_seq.c` 的 `drive_seq_run()`：

```text
读 6041 → ds402_parse
若 FAULT → 复位（先写 0x00 造下沿，再写 0x80 造上沿，等 0x40）
i = ds402_seq_skip(sw, seq, n)          ← 起点快进，已走过的不再走
for (; i < n; i++)
        写 6040 = seq[i].cw
        读 6041 直到 (sw & exp_mask) == exp_value
        ——每步独立超时（DRIVE_STEP_TIMEOUT_MS），卡在哪一步看得见
        ——读到 FAULT / 被 cancel / 超时 → 失败
```

序列本身（06→07→0F 与期望状态字）在 `ds402/`，这里的代码不认识这些数。

## 线程归属

| 线程 | 调什么 |
|---|---|
| 服务线程 | `drive_service()`、`od_access.read/write`（阻塞） |
| I/O 线程 | `od_sdo_io_step()`（推进事务、收帧分派） |
| 任意线程 | `drive_request_*` / `drive_halt` / `drive_quick_stop` / `drive_cancel` / 事实查询 |

## 本层不知道

- CAN 帧、COB-ID（`master` / `canopen`）；
- 传输是 SDO 还是 PDO（`od_access` 后面）；
- 业务状态机、单位（`statemachine` / `axis`）。
