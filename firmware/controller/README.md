# controller/ — 整机控制主体

`cats_control`：业务状态机 + 各轴运行期 + 组合根。

## 结构

```text
controller/
├── bus/            帧契约 + SocketCAN 后端（不认协议）
├── canopen/        协议栈：DS 301 子集（COB-ID / NMT / 心跳 / SDO 客户端）
├── master/         主站实体：一条总线 + 登记的从站（站号表 / 收发分派）
├── ds402/          CiA 402 状态知识（零依赖）
├── drive/          从站运行期：请求槽、状态迁移、模式插件、对象访问后端
├── axis/           单位换算与指令流
├── statemachine/   本机业务状态图（一状态一文件，states.c 唯一注册点）
├── hsm/            通用层次状态机内核（零业务依赖）
├── config/         机械参数与 PDO 配置
├── include/control/control.h    对外契约
├── root.c                       组合根（唯一装配点）
└── root_internal.h              组合根私有上下文
```

## 职责

- 装配：所有对象的创建与注入只在 `root.c` 一处
- 对外只给三个"推进一步"的接口、指令入口、故障读值
- 故障策略（通信超时、驱动器报警 → 系统动作）
- 不创建线程、不决定节拍（见下节）

## 边界

- 只依赖各层的公共头：`axis`、`drive`、`master`、`canopen`、`bus`；
- 不直接操作 PDO 字节、不直接调用平台 CAN 接口；
- 具体 backend 由集成层传入，本层不 `#include` 任何 backend 实现。

## 对外契约（`include/control/control.h`）

集成层只 include 这一个头，接口为：
`cats_control_init` / `cats_control_io_poll` / `cats_control_poll` /
`cats_control_svc_poll` / `cats_control_state` / `cats_control_state_name` /
`cats_control_move_to` / `cats_control_stop` / `cats_control_estop` /
`cats_control_fault_code` / `cats_control_fault_name` /
`cats_control_axis_abort` / `cats_control_axis_sample`。

- **分步推进**：`io_poll` / `poll` / `svc_poll` 三个入口各管一件事，由宿主编排（见下节）；
- **启动不靠外部事件**：`cats_control_init()` 建好状态机即为 PREPARING，之后由 `poll` 自行推进；
- **单例**：一个进程一条总线，静态实例，不动态分配内存；
- **跨线程只共享少数字段**（指令槽 / 急停请求 / 发布的状态与样本），由一把
  `pthread_mutex_t` 保护；
- 时序、超时、状态全在本层及以下，壳只负责“什么时候调”。

> 状态机：PREPARING → CONFIGURING → ENABLING → HOLDING ⇄ MOVING，HALTING 回 HOLDING，
> FAULT / ESTOP 为终态（ESTOP 只能重启程序退出）。
>
> 层次：ROOT（统一故障出口）├─ PREPARING（不在 ONLINE 下：此时心跳还没有是正常的）
> └─ ONLINE（心跳丢失 → FAULT）├─ CONFIGURING / ENABLING / HOLDING / MOVING / HALTING。

## 如何驱动（宿主职责）

本目录**不创建任何线程**，只提供三个推进步骤，节拍与线程由 `apps/` 下的宿主编排：

| 步骤 | 线程要求 | 节拍 | 说明 |
|---|---|---|---|
| `cats_control_io_poll()` | 单线程、独占总线 | 1 ms | 收帧分派 + 推进各节点 SDO 通道 |
| `cats_control_poll()` | 可与 io_poll 同线程（先 io 后控制） | 1 ms | 业务状态机走一拍，不得阻塞 |
| `cats_control_svc_poll()` | **必须独占一个线程** | ~1 ms | 会阻塞（最长一个 SDO 超时） |

参考实现（照抄即可）：

- 产品宿主：`apps/qt/bindings/can_thread.cpp`（io+ctrl 一条线程，svc 一条）
- 最小宿主：`apps/tools/master_probe/main.c`

边界检查 `apps/tools/layering_check.sh` 的 R8 会验证本目录里没有 `pthread_create`。
