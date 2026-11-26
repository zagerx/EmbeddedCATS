# controller_states/ — 控制器的业务状态图

本目录只包含状态函数与状态注册表。控制器对象、输入事件队列、公共类型与推进入口均归控制层，不在本目录另设运行对象。

## 职责

- 输入事实：各轴配置/使能/故障与运动结论；
- 输出动作：通过主站 NMT 接口与轴接口下发复位节点与轴请求；
- `states.c` 只负责注册状态函数；状态与故障一律以枚举值对外发布，本层不产出字符串；
- `states.h` 集中声明状态函数，并提供从 HSM 成员找回 `struct controller` 的工具。

## 状态流

```text
PREPARING → CONFIGURING → IDLE ──CONTROLLER_SIG_ENABLE──> ENABLING → HOLDING ⇄ MOVING
                                                          ↑              │    ⇅
                                                          └─ HALTING ────┘  HOMING
FAULT 为终态；ESTOP 只能解除回 IDLE。
```

`IDLE` 表示"已配置、未使能"：配置完成后停在这里，只有 `controller_enable()` 投递的
使能事件才继续。状态函数名是 `st_idle_ctrl`，因为 `st_idle` 已被 cia402 设备层占用。

`PREPARING` 只发一条**复位节点**命令（`0x81` 广播）并驻留 250 ms。驱动器整机重启，
实测（2026-11-20，5 次样本）从收到命令到发出 boot-up 帧需 138~168 ms；窗口内发出的
SDO 不会被拒绝，而是排队后延迟应答，所以必须等够时间再配置，不能靠"发早了自然失败" 兜底。
复位后驱动器停在 Pre-operational，`CONFIGURING` 的 NMT Start 仍然必须发。
复位不改位置（`6064h` 前后一致），但会按存储值重载应用参数。

`ESTOP` 的入口只做两件事：给各轴写一次 quick stop 控制字、清空事件队列。它不等减速过程、
不写断使能，也不让 `ROOT` 的"有故障 → FAULT"规则把它拉走；那一笔写没送到时，以
`stage = ESTOP` 锁存故障供 UI 显示。

`ESTOP` 只接受一条命令：`CONTROLLER_SIG_ESTOP_CLEAR`（解除急停）。它让各轴写一次
`6040 = 0x00` 断使能、清掉旧运动批次，**仍留在 ESTOP 等回执**，收齐后才迁 `IDLE`。
回执必须在本状态收完：那条回执不带流程身份，交给 `IDLE` 收会被随后的 `ENABLING`
误当成"使能成功"。

**急停是覆盖层，故障独立锁存**：解除时不碰 `controller_info.fault`，所以若急停前已有故障
（或急停/解除那笔没送到而锁存了新故障），等迁到 `IDLE` 后由 `ROOT` 的"有故障 → FAULT"
规则把整机送进 `FAULT`。想恢复必须重启程序，不能用"解除急停"把故障顶掉。

`HOMING` 与 `MOVING` 同级，且只能从 `HOLDING` 进：回零要求驱动器已使能。
进入时对参与轴下发 `axis_home()`（方法号与三档速度是 `cats_mech.h` 里的
机械常量，业务层不传参数）。

**回零期间是原子的**：只响应急停（由 `ROOT` 迁 `ESTOP`）与故障（由 `ROOT` 迁
`FAULT`），`MOVE` / `HALT` 一律在状态里丢弃 —— 丢在这里而不是留在队列里，避免
回零结束后被意外执行。半个回零流程比走完更危险：原点没标定，上层却以为标了；
要中途停只能按急停。

`HOMING` 只等待本次实际受理的轴。`controller_axes_collect()` 只扫描这个等待集合，
不会取未参与轴的旧回执；集合清空后回 `HOLDING`，不代表被取消的回零成功。
设备忙只记未执行日志；取消也不生成故障或伪成功回执。

急停抢占回零时，402 层由 `st_home` 拦下 `CIA402_SIG_CANCEL` 走中止链
（停电机 → 落 bit4 → 切回 PP）：`halt.c` 清 bit8 用的是 `confirmed` 镜像，若直接
被抢占跳 `st_idle`，随后那笔 halt 会把 bit4 一起带回来，驱动器会**重启回零**。

故障以 `struct controller_fault` 上报：失败阶段、真实执行失败原因、首个失败轴、
失败轴位图。未受理、未执行与取消只记日志，不生成故障。后续故障
只累积轴位图，不覆盖首个快照。

## 线程与边界

- 状态图只由 CANopen 工作线程推进：对外入口是 `controller_event_poll()`，每轮内部
  调一次 `controller_poll()` 分发事件并投递节拍；
- 状态函数使用 `controller` 中平铺的状态字段；
- 时钟经 `controller_now_ms()` 取得，NMT 直接调 `master_request_nmt()`；状态图不
  依赖 CANopenNode 头、SDO 传输层或平台 socket；
- 不处理 402 对象号、SDO/PDO 或 CAN 帧。

## 加一个状态

1. 写 `state_xxx.c`；
2. 在 `states.h` 加声明；
3. 在 `states.c` 的 `states[]` / `NAMES[]` 各加一行，下标与 `enum controller_state` 对应。
