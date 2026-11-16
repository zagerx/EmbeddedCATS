# controller/ — 整机控制主体

## 结构

```text
controller/
├── canopen/
│   ├── master/     主站生命周期、节点表、NMT 与心跳查询
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
├── controller_feedback.c 反馈读取调度与逐轴缓存
├── control_api.c      指令、查询与发布
└── controller_internal.h 控制器完整上下文与内部契约
```

## 职责

- `struct controller` 持有主站指针、状态图和轴集合；每轴 SDO 通道随 `axis` 持有。
- 宿主只提供接口名并创建一个 CANopen 工作线程，不持有 fd、主站或设备对象。
- `controller_init(ifname)` 装配资源；`controller_event_poll()` 处理一轮事件；
  `controller_deinit()` 在线程退出后释放资源，支持安全重复调用。
- 提供使能、位置、停止、急停、状态、故障和轴样本接口。
- 使用 402 对象索引调度反馈 SDO；每轴仲裁器隔离控制与反馈结果，控制优先。
- 反馈忙时跳过，成功才更新缓存；控制请求登记后自动取消反馈并等待通道释放。
- 不创建线程，不直接操作 SocketCAN 或 CANopen PDO 字节。

## 状态机

```text
PREPARING → CONFIGURING → IDLE ──controller_enable()──> ENABLING → HOLDING ⇄ MOVING
                                                         ↑               │
                                                         └── HALTING ────┘
FAULT / ESTOP 为终态。
```

`IDLE` 是“已配置、未使能”的空闲态：配置完成后停在这里，直到外部下发使能请求。

`ROOT` 统一处理故障与急停，`ONLINE` 为运行期子状态统一检查心跳。

## 宿主调用约束

| 接口 | 调用方 | 是否阻塞 |
|---|---|---|
| `controller_init()` | CANopen 工作线程 | 仅初始化系统调用 |
| `controller_event_poll()` | CANopen 工作线程 | 等待 CAN / timerfd / eventfd |
| `controller_poll()` | 同一工作线程内部 | 否，只推进整机状态图 |
| `controller_deinit()` | 工作线程 join 后的宿主 | 仅资源释放 |
| 命令与查询 | UI 等其他线程 | 否；入队后 eventfd 唤醒 |

跨线程只保留命令队列和发布快照的锁。CANopen、SDO、CiA402 与整机状态机均在同一
工作线程内推进。参考实现：`apps/qt/bindings/can_thread.cpp`、
`apps/tools/master_probe/main.c`。
