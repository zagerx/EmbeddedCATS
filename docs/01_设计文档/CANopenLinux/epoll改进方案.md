# CANopenLinux 改进方案（临时）

## 1. 改进目标

本次改进解决三个相互关联的问题：

1. 宿主直接打开、持有和关闭 CAN fd，使 SocketCAN 生命周期暴露在应用层。
2. 自写的 SocketCAN 驱动与 1 ms 轮询调度没有利用 CANopenLinux 已有的 Linux 实现。
3. 当前 `transport/sdo_client` 同时承担 SDO 协议适配和跨线程交接，后者依赖固定的轮询线程模型。

改进后的目标结构为：

```text
Qt UI / 命令行宿主
    │ 启动参数、用户命令、状态查询
    ▼
controller：整机状态机、命令队列、状态发布
    │
    ▼
axis + CiA 402：单位换算、设备流程、对象访问请求
    │ sdo_access
    ▼
极薄的 SDO 后端适配
    │
    ▼
CANopenNode：NMT、心跳、SDO 等协议机制
    │
    ▼
CANopenLinux：SocketCAN + epoll + timerfd + eventfd
```

## 2. 第三方代码的组织原则

- `third_party/CANopenLinux/` 保持上游源码原样，不在子模块内修改代码或增加构建文件。
- 在 `controller/third_party/CMakeLists.txt` 中集中描述 CANopenLinux 的源码、头文件路径、编译宏和链接依赖。
- 顶层 `firmware/CMakeLists.txt` 通过 `add_subdirectory()` 引入该构建入口，不再逐项拼接上游协议核心源文件。
- 本项目只编译需要的上游组件；不编译上游 `CO_main_basic.c`，不生成或运行上游 `canopend`。
- 采用上游 `CO_driver.c`、`CO_error.c`、`CO_epoll_interface.c` 及其对应头文件。
- 上游 SocketCAN 驱动使用动态内存管理接口和过滤资源，不再以本项目的零 malloc 约定限制第三方实现。
- 继续由 `controller/config/canopen_node/` 提供本项目对象字典，不使用上游示例对象字典。
- `CO_driver_target.h` 使用上游版本。启用裁剪后的服务宏；多线程锁宏即使定义也保持为空，因为协议核心只由一个工作线程访问。

## 3. CAN 资源所有权

### 3.1 对外接口

公共初始化入口从接收 fd 改为接收接口名：

```c
enum controller_result controller_init(const char *can_ifname);
enum controller_result controller_deinit(void);
```

Qt 与 `master_probe` 只传递 `"can0"` 等启动参数，不再调用 SocketCAN 打开或关闭函数，不保存 fd。

### 3.2 下层所有权

`master_init()` 接收接口名，组织 CANopenLinux 初始化。主站持有：

```text
CO_t                  CANopenNode 实例
CO_epoll_t            epoll、timerfd、eventfd 与时间信息
CO_CANptrSocketCan_t  CAN 接口索引和 epoll fd
```

初始化顺序：

```text
创建 epoll / timerfd / eventfd
    → if_nametoindex() 得到接口索引
    → CO_new()
    → CO_CANinit()
    → CO_CANopenInit()
    → 登记从站并装配轴与状态机
    → CO_CANsetNormalMode()
```

CANopenLinux 的 `CO_CANmodule_init()` 负责创建、配置、绑定 SocketCAN socket，并将其加入 epoll。
socket 由 `CO_CANmodule_t` 管理，不再另存一个由宿主负责关闭的 fd。

所有初始化函数均检查返回值。任何一步失败都逆序释放已经创建的资源，并向上层返回初始化失败。
现有忽略 `CO_CANinit()`、`CO_CANopenInit()` 返回值的写法同时取消。

### 3.3 停止与释放

停止顺序固定为：

```text
置停止标志
    → eventfd 唤醒工作线程
    → 工作线程退出循环
    → 宿主 join 工作线程
    → controller_deinit()
    → CO_CANsetConfigurationMode()
    → CO_CANmodule_disable()
    → CO_delete()（当前构建使用动态分配时）
    → CO_epoll_close()
```

只有工作线程退出后才能关闭协议栈和 socket，保证没有线程继续访问这些资源。
`controller_deinit()` 负责完整释放并恢复未初始化状态，不允许重复关闭同一资源。

## 4. 线程模型

采用一个 CANopen 工作线程；Qt 仅保留 UI 线程。

```text
Qt UI 线程
    ├── controller_move() / controller_stop() / controller_estop()
    ├── 查询发布快照
    └── 通过 eventfd 唤醒工作线程

CANopen 工作线程
    ├── SocketCAN 接收与发送
    ├── CANopenNode 主循环和实时处理
    ├── 各轴在途 SDO 事务
    ├── CiA 402 状态机
    └── controller 整机状态机
```

`controller` 仍不创建线程。宿主只创建一个工作线程，在工作线程内完成初始化、事件循环和退出。

取消宿主中独立的：

- `run_io()` / I/O 线程；
- `run_svc()` / 服务线程；
- 仅运行 `controller_poll()` 的固定节拍控制线程。

Qt 的 `CanThread` 保留为工作线程的创建、停止和统计载体，但其内部改为运行一个 CANopen 事件循环。

### 同线程化后的边界

以下数据不再跨越 CANopen 工作线程内部的多个线程：

- CiA 402 请求 mailbox 与结果 outbox；
- `transport/sdo_client` 的请求和结果；
- NMT 待发槽。

对应的跨线程互斥与转发状态应取消，不能仅删除线程而遗留不再适用的调度层。

Qt 与 CANopen 工作线程之间仍需要：

- controller 命令队列的互斥保护；
- 状态快照的互斥保护；
- 命令入队后的 `eventfd` 唤醒。

## 5. epoll 事件循环

### 5.1 三类事件

| 事件源 | 作用 |
|---|---|
| CAN socket | 收到 CAN 帧或错误事件后立即处理 |
| timerfd | 提供基础周期，推进协议超时、402 流程期限和反馈轮询 |
| eventfd | 通知外部命令到达或请求线程退出 |

初版保留 1 ms 基础定时器，避免一次改动同时重写所有流程超时和反馈调度。
有 CAN 帧或 UI 命令时提前醒来，不等待下一次 1 ms 定时器。

CANopenLinux 的 `CO_epoll_processLast()` 只在 `timerNext_us` 小于基础周期时缩短下一次唤醒，
不提供任意期限定时器管理。因此本次不把全部超时改造成独立的动态定时器系统。

### 5.2 一轮处理顺序

```c
while (!stop_requested) {
    CO_epoll_wait(&master->ep);

    CO_epoll_processRT(&master->ep, master->co, false);

    for_each_axis(controller) {
        sdo_backend_step(axis, master->ep.timeDifference_us);
    }

    CO_epoll_processMain(&master->ep, master->co, false, &reset);

    controller_axis_step(controller);
    controller_step(controller);

    CO_epoll_processLast(&master->ep);
}
```

这是本项目的编排逻辑，不是要求修改上游源码。

各步骤职责：

1. `CO_epoll_wait()`：阻塞等待 CAN、定时器或 eventfd；同时计算真实 `timeDifference_us`。
2. `CO_epoll_processRT()`：接收 CAN 帧并交给 CANopenNode，推进 SYNC、RPDO、TPDO。
3. `sdo_backend_step()`：调用上游 `CO_SDOclientUpload()` 或 `CO_SDOclientDownload()`，形成完成、失败或等待结果。
4. `CO_epoll_processMain()`：调用 `CO_process()`，推进 NMT、心跳消费、EMCY、CAN 发送状态及其他启用的主循环服务。
5. `controller_axis_step()`：推进各轴 CiA 402，消费 SDO 结果并按需发起下一笔对象访问。
6. `controller_step()`：消费 UI 命令，读取心跳和轴状态，推进整机状态机。
7. `CO_epoll_processLast()`：完成本轮事件处理，必要时根据 `timerNext_us` 缩短下一次定时唤醒。

### 5.3 顺序约束

- CAN 帧先交给协议栈，再推进 SDO，使刚收到的响应在当前轮可见。
- SDO 发起或推进后再执行 CANopen 主循环，使本轮产生的 CAN 请求进入发送处理。
- CiA 402 在同轮消费 SDO 完成结果，并推进设备流程。
- 整机状态机只读取设备事实和心跳事实，不解释 CAN 帧或 SDO 协议步骤。
- `CO_epoll_processRT()`、`CO_epoll_processMain()` 与 SDO 客户端始终由同一工作线程调用。

## 6. SDO 请求的事件路径

以读取 `0x6041` 为例：

```text
controller_axis_step() 发起 SDO Upload
    → sdo_backend_step() 调用 CO_SDOclientUploadInitiate()
    → CO_epoll_processMain() 处理本轮发送
    → CO_epoll_processLast()
    → CO_epoll_wait() 等待
    → 从站响应使 CAN socket 就绪
    → CO_epoll_processRT() 接收并分派响应
    → sdo_backend_step() 调用 CO_SDOclientUpload()
    → 保存读取值或 abort code
    → controller_axis_step() 消费结果并推进 402
```

`CO_process()` 不自动推进本项目的 `CO_SDOclientUpload()` / `CO_SDOclientDownload()`。
这两个函数必须由 SDO 后端在每轮显式调用。

SDO 步进使用 epoll 提供的真实时间差，不再假定每次调用恰好经过 1 ms。
上游 SDO 提供的 `timerNext_us` 用于需要提前处理的情况。

## 7. transport 的调整

### 7.1 保留的边界

保留 `sdo_access` 所表达的异步对象访问语义：

- 请求发起后立即返回；
- 通信完成后可取得结果；
- CiA 402 不依赖 CANopenNode 类型；
- 每笔请求独立记录成功、失败和 abort code。

该边界隔离的是 CiA 402 与对象访问协议，不是为 1 ms 轮询而存在。

### 7.2 取消的职责

当前 `transport/sdo_client` 中以下跨线程机制不再保留：

- 服务线程与 I/O 线程共享的请求槽互斥锁；
- 为跨线程交接设置的 `QUEUED / RUNNING / DONE` 状态；
- 独立的 `sdo_client_io_poll()` 轮询入口；
- 由另一线程搬动结果后再唤醒设备状态机的流程。

### 7.3 新的后端形态

SDO 后端只在 CANopen 工作线程内执行：

```text
接收访问请求 → 调用上游 Initiate / BufWrite
每轮推进     → 调用上游 Upload / Download
事务完成     → 保存结果，交由同线程 CiA 402 消费
取消         → 按上游 Close / abort 语义结束本次事务
```

`transport` 不再负责线程调度。若最终只剩少量同线程适配代码，应合并到所属 CANopen 后端，
不为保留旧目录而保留空壳；`sdo_access` 作为 CiA 402 依赖的契约仍可独立保留。

## 8. 对外命令与状态发布

`controller_move()`、`controller_stop()`、`controller_estop()` 可由 UI 线程调用：

```text
锁定命令队列
    → 写入命令
    → 解锁
    → 写 eventfd 唤醒工作线程
```

急停和停止仍具有优先处理规则，eventfd 使其无需等待下一次基础定时器。

状态快照继续由 `controller` 统一发布，UI 只读取快照，不访问主站、SDO 客户端或 CiA 402 内部对象。

NMT 请求不再经过跨线程待发槽，由工作线程中的整机状态机调用主站发送接口。
主站仍负责具体 NMT 命令，业务状态机不直接操作 CAN 帧。

## 9. 代码调整范围

| 模块 | 调整 |
|---|---|
| `third_party/CMakeLists.txt` | 新增 CANopenLinux 构建入口，集中管理第三方源码和宏 |
| `canopen/master/` | 持有上游 CANopen/epoll 对象，负责初始化、事件处理、节点与资源释放 |
| `canopen/driver/` | 删除被上游替代的自写 SocketCAN 驱动和 `co_driver_cats` 接口 |
| `canopen/transport/` | 移除跨线程调度，只保留必要的异步 SDO 适配与访问契约 |
| `cia402/` | 改为同线程请求与结果处理，保留设备状态机和流程语义 |
| `controller/` | 初始化参数改为接口名，统一工作线程内的轴与整机推进，增加退出清理 |
| `apps/qt/` | 不再打开或关闭 CAN；只创建一个工作线程，保留 UI 与状态显示 |
| `master_probe/` | 不再管理 CAN fd，使用同一 controller 生命周期和工作线程入口 |
| 文档与分层检查 | 同步线程、依赖、目录和资源所有权规则 |

## 10. 分层约束

改进后仍保持：

```text
宿主 → controller → axis → CiA 402 → sdo_access
                              │
                              ▼
                       SDO 后端 → CANopenNode
                              │
                              ▼
                    CANopenLinux / SocketCAN
```

- 宿主不知道 fd、CAN 帧、对象字典和 402 步骤。
- `controller` 不知道 SDO 报文与 402 控制字细节。
- CiA 402 不知道 SocketCAN、epoll 和 CANopenNode 类型。
- SDO 后端不知道“使能”“运动”“停车”等设备语义。
- 主站不知道 CiA 402 对象号和业务状态机。
- 所有底层对象只由 CANopen 工作线程访问。

分层检查需要随新规则调整；不能仅为通过检查而排除本项目代码，也不能把第三方示例当成本项目线程实现。

## 11. 实施顺序

1. 在 `third_party/CMakeLists.txt` 接入上游 Linux 驱动、epoll、错误日志和所需 CANopenNode 服务。
2. 重写主站初始化与资源释放，取消宿主 fd 所有权。
3. 建立单工作线程的 epoll 循环和 eventfd 唤醒。
4. 将 SDO 后端改为同线程事件步进，删除旧跨线程交接。
5. 将 CiA 402 与整机状态机接入同一循环，删除已无意义的 mailbox 和 NMT 跨线程槽。
6. 修改 Qt 与 `master_probe` 的启动、停止和错误处理。
7. 更新文档和分层检查，再执行完整验证。

每一步只迁移对应职责，不修改 CANopenLinux 子模块源码。

## 12. 验证范围

静态与软件验证：

- 独立构建目录全量编译，零错误、零警告；
- `cia402_selftest`；
- 分层检查；
- `fake_drive` 自测；
- Qt 与 `master_probe` 的启动失败、正常停止、重复初始化路径；
- 用模拟 CAN 环境验证双轴配置、使能、运动、停止和急停；
- 检查停止后 CAN socket、epoll、timerfd、eventfd 和动态资源均释放；
- 验证 UI 命令通过 eventfd 唤醒，不依赖额外轮询才能被处理。

实机验证需另行进行。本方案中的软件构建与模拟测试不等同于真实驱动器上的时序和运动安全确认。
