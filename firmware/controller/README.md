# controller/ — 整机控制主体

## 构建入口

本目录是**自包含的 CMake 子工程**：`controller/CMakeLists.txt` 定义全部库目标
（`canopen_node`、`cats_hsm`、`cats_master`、`sdo_access`、`canopen_sdo`、`cia402`、
`cats_axis`、`cats_control`），并可被任何工程直接引入，不依赖父工程的 C 标准、警告选项
或平台配置。

```cmake
add_subdirectory(<path>/firmware/controller)
target_link_libraries(自己的目标 PRIVATE cats_control)
```

- 对外只暴露 `include/`（`control/control.h`、`control/state_types.h`）；其余各层的
  include 目录不会传给宿主，宿主也不应该 include 它们。
- `find_package(Threads)` 由本目录自己声明；Qt 等宿主侧依赖不在这里。

## 结构

```text
controller/
├── canopen/
│   ├── master/     主站生命周期、节点表、NMT 与通信复位
│   ├── pdo/        在空置接收槽上接住从站 TPDO 并解包
│   └── transport/  对象访问契约与同线程 CANopen SDO 后端
├── cia402/         DS402 纯知识与单从站运行控制
├── third_party/    CANopenLinux 与 CANopenNode，只读
├── controller_axis/ 单轴请求、单位换算与整机轴集合
├── controller_states/ 整机业务状态图与状态注册表
├── controller_evtqueue/ 带锁的控制输入事件队列
├── hsm/            通用层次状态机内核
├── config/         机械参数与 CANopen 对象字典
├── include/control/ 公共状态、故障与控制接口
├── controller.c       组合根、生命周期与事件循环
├── controller_actions.c 时间基准与状态图装配
├── controller_feedback.c 把 TPDO 快照换算并写入上报结构
├── control_api.c      指令、故障锁存与信息查询
└── controller_internal.h 控制器完整上下文与内部契约
```

## 职责

- `struct controller` 持有主站指针、状态图和轴集合；每轴 SDO 通道随 `axis` 持有。
- 宿主只提供接口名并创建一个 CANopen 工作线程，不持有 fd、主站或设备对象。
- `controller_init(ifname)` 装配资源；`controller_event_poll()` 处理一轮事件；
  `controller_deinit()` 在线程退出后释放资源，支持安全重复调用。
- 提供使能、位置、暂停、急停、状态、故障和轴样本接口。
- 反馈改由 TPDO 提供：`canopen/pdo` 在空置接收槽上解包，SDO 通道由 402 流程独占。
- `controller_feedback.c` 只把最新 TPDO 快照搬进上报结构，不发任何总线帧。
- 不创建线程，不直接操作 SocketCAN；PDO 字节只由 `canopen/pdo` 解。

## 状态机

```text
PREPARING → CONFIGURING → IDLE ──controller_enable()──> ENABLING → HOLDING ⇄ MOVING
                                                         ↑               │
                                                         └── HALTING ────┘
FAULT / ESTOP 为终态。
```

`IDLE` 是“已配置、未使能”的空闲态：配置完成后停在这里，直到外部下发使能请求。

`ROOT` 统一处理故障与急停。当前不根据心跳监控结果迁移状态。

## 宿主调用约束

| 接口 | 调用方 | 是否阻塞 |
|---|---|---|
| `controller_init()` | CANopen 工作线程 | 仅初始化系统调用 |
| `controller_event_poll()` | CANopen 工作线程 | 等待 CAN / timerfd / eventfd |
| `controller_poll()` | 同一工作线程内部 | 否，只推进整机状态图 |
| `controller_deinit()` | 工作线程 join 后的宿主 | 仅资源释放 |
| 命令与查询 | UI 等其他线程 | 否；入队后 eventfd 唤醒 |

跨线程只保留命令队列和 `controller_info` 的锁。状态机锁存故障、反馈采集保存样本时都
直接写入 `controller_info`；每轮末尾 `controller_info_update()` 统一更新状态与 abort。
位置与速度在反馈保存时已转换为 mm / mm·s⁻¹，查询直接拷贝转换后的值。CANopen、SDO、
CiA402 与整机状态机均在同一工作线程内推进。参考实现：
`apps/qt/bindings/can_thread.cpp`、`apps/tools/master_probe/main.c`。
