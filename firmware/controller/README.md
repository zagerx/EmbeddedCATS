# controller/ — 整机控制主体

## 结构

```text
controller/
├── bus/            CAN 帧契约与 SocketCAN 后端
├── canopen_node/   CANopenNode CO_driver 移植层
├── master/         主站、节点表、NMT、心跳和协议栈推进
├── third_party/    CANopenNode，只读
├── transport/      对象访问契约与 CANopen SDO 后端
├── cia402/         DS402 纯知识与单从站运行控制
├── axis/           单位换算与多轴请求
├── statemachine/   整机业务状态图
├── hsm/            通用层次状态机内核
├── config/         机械参数与 CANopen 对象字典
├── include/control/control.h
├── controller.c           组合根与 I/O、服务推进
├── control_api.c          状态机推进、指令与发布
└── controller_internal.h
```

## 职责

- 在 `controller.c` 集中创建并连接主站、对象访问通道、CiA 402 设备、轴和业务状态机。
- 提供 `controller_io_poll()`、`controller_poll()`、`controller_svc_poll()` 三个推进接口。
- 提供位置、停止、急停、状态、故障和轴样本接口。
- 不创建线程，不直接操作 SocketCAN 或 CANopen PDO 字节。

## 状态机

```text
PREPARING → CONFIGURING → ENABLING → HOLDING ⇄ MOVING
                                      ↑           │
                                      └─ HALTING ─┘
FAULT / ESTOP 为终态。
```

`ROOT` 统一处理故障与急停，`ONLINE` 为运行期子状态统一检查心跳。

## 宿主调用约束

| 接口 | 线程 | 节拍 | 是否阻塞 |
|---|---|---|---|
| `controller_io_poll()` | I/O，独占总线 | 1 ms | 否 |
| `controller_poll()` | 控制，可与 I/O 同线程且后调用 | 1 ms | 否 |
| `controller_svc_poll()` | 独立服务线程 | 约 1 ms | 是 |

参考实现：`apps/qt/bindings/can_thread.cpp`、`apps/tools/master_probe/main.c`。
