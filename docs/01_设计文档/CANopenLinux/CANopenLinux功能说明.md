# CANopenLinux 功能说明

## 1. 仓库定位与组成

CANopenLinux 是运行在 Linux 设备上的 CANopen 软件仓库，项目主页为
<https://github.com/CANopenNode/CANopenLinux>，采用 Apache License 2.0。

CANopenLinux 包含 CANopenNode 子模块。CANopenNode 提供平台无关的 CANopen 协议核心，
CANopenLinux 在此基础上提供 Linux SocketCAN 驱动、事件循环、持久化存储、日志、通用节点程序和命令行工具。

仓库根目录主要文件：

| 路径 | 内容 |
|---|---|
| `CANopenNode/` | CANopen 协议核心子模块 |
| `CO_driver.c`、`CO_driver_target.h` | Linux SocketCAN 驱动及平台类型、临界区和配置定义 |
| `CO_epoll_interface.c`、`CO_epoll_interface.h` | 基于 epoll 的 CAN、定时器和命令网关事件处理 |
| `CO_error.c`、`CO_error.h`、`CO_error_msgs.h` | syslog 日志和错误信息 |
| `CO_storageLinux.c`、`CO_storageLinux.h` | 对象字典参数的文件保存与恢复 |
| `CO_main_basic.c` | 通用 CANopen Linux 节点程序入口 |
| `CO_application.h` | 可选应用回调接口 |
| `Makefile` | `canopend` 的构建与安装规则 |
| `cocomm/` | CANopen ASCII 命令客户端 |

## 2. CANopenNode 协议核心

CANopenNode 源码中的功能由 `CO_CONFIG_*` 宏裁剪，主要服务如下。

| 服务 | 功能 |
|---|---|
| NMT | 本机 NMT 状态、心跳生产、启动与复位处理；可配置 NMT 主站命令发送 |
| Heartbeat Consumer | 按对象字典 `0x1016` 的配置接收并监控其他节点心跳，提供节点状态与超时信息 |
| SDO Server | 响应客户端对本机对象字典的访问，支持加速、分段和块传输配置 |
| SDO Client | 发起对其他节点对象字典的上传和下载，支持加速、分段和块传输配置 |
| PDO | RPDO、TPDO 的通信参数、映射、接收、发送及同步处理 |
| SYNC | SYNC 报文的生产、消费及同步 PDO 协调 |
| TIME | CANopen 时间戳对象的生产与消费 |
| EMCY | 紧急报文生产、消费、错误寄存器、状态位和历史记录 |
| Node Guarding | 节点守护从站和主站功能 |
| LSS | LSS 从站与主站功能，包括站号和波特率相关操作 |
| GFC | 全局故障安全命令 |
| SRDO | 安全相关数据对象 |
| Gateway ASCII | CiA 309-3 ASCII 命令接口及 NMT、SDO、LSS 等命令处理 |
| Object Dictionary | 对象字典条目访问、SDO/PDO 使用的 OD 接口和扩展对象支持 |
| Storage | `0x1010` 保存、`0x1011` 恢复默认参数及自动保存的通用机制 |

对象字典由 `OD.h`、`OD.c` 定义。对象字典内容、启用的服务数量、通信参数和 PDO 映射决定了
协议核心初始化后实际提供的功能。

## 3. Linux SocketCAN 驱动

`CO_driver.c` 实现了 CANopenNode 的 Linux SocketCAN 驱动接口：

- 创建 `PF_CAN`、`SOCK_RAW`、`CAN_RAW` socket；
- 按接口名对应的 `can_ifindex` 绑定 SocketCAN 设备；
- 将 CAN socket 注册到 epoll；
- 接收经典 CAN 或 CAN FD 帧，并读取 socket 时间戳；
- 将接收帧转换为 CANopenNode 消息，按标识符和掩码匹配接收缓冲；
- 实现 `CO_CANinit()`、`CO_CANmodule_init()`、`CO_CANrxBufferInit()`、
  `CO_CANtxBufferInit()`、`CO_CANsend()` 等驱动接口；
- 管理发送缓冲、发送等待、总线错误状态和 CAN 模块状态；
- 在通信复位和关闭过程中释放 CAN 接口资源；
- 支持通过 `CO_DRIVER_MULTI_INTERFACE` 配置多 CAN 接口。

CAN 接口的波特率等链路参数由 Linux 的 CAN 配置完成，驱动使用已经配置的接口。

## 4. epoll 与协议栈推进

`CO_epoll_interface.c` 将 Linux 文件描述符、定时器与 CANopenNode 的处理函数连接起来：

- `CO_epoll_create()` 创建 epoll 实例、周期定时器和事件通知；
- `CO_epoll_wait()` 等待 CAN socket、定时器或其他已登记事件；
- `CO_epoll_processMain()` 推进非实时 CANopen 服务并处理通信复位；
- `CO_epoll_processRT()` 推进实时部分，包括 SYNC 和 PDO；
- `CO_epoll_processLast()` 更新时间差并执行每轮收尾；
- `CO_epoll_createGtw()` 创建命令网关所需的命令接口；
- `CO_epoll_processGtw()` 接收命令连接和文本命令并推进网关处理。

主循环按事件调用协议栈，不需要通过持续忙等查询 CAN socket。

## 5. 通用节点程序 canopend

CANopenLinux 根目录的 `Makefile` 将 `CO_main_basic.c`、Linux 驱动、epoll、存储、
CANopenNode 和默认对象字典编译为可执行文件 **`canopend`**。

`CO_main_basic.c` 实现了完整的 `main()`，负责：

1. 解析 CAN 接口、节点站号、实时优先级、存储路径和命令接口等参数；
2. 创建 CANopen 对象并初始化 Linux 存储；
3. 初始化 SocketCAN、LSS、NMT、SDO、心跳、EMCY 和 PDO；
4. 创建 epoll 主循环，并按配置选择单线程或主线程加实时线程；
5. 响应通信复位、程序退出和可选的 Linux 重启命令；
6. 通过 syslog 输出运行与错误信息。

默认对象字典来自 `CANopenNode/example/OD.c`，只包含通信参数示例。
对象字典和构建配置可以替换或扩展。

`canopend` 的主要参数：

| 参数 | 功能 |
|---|---|
| `-i <Node ID>` | 设置本机 CANopen 站号，范围为 1～127；`0xFF` 表示 LSS 未配置节点 |
| `-p <RT priority>` | 设置实时线程优先级；单线程构建不提供此参数 |
| `-r` | 允许收到 NMT reset node 后重启 Linux |
| `-s <storage path>` | 设置参数存储文件的路径和文件名前缀 |
| `-c <interface>` | 启用 ASCII 命令接口：`stdio`、`local-<路径>` 或 `tcp-<端口>` |
| `-T <timeout_time>` | 设置 local 或 TCP 命令连接的 socket 超时时间 |

默认 `Makefile` 定义 `CO_SINGLE_THREAD`，CAN 接收和定时器事件都在主循环中处理。
不使用该宏时，程序会创建周期为 1 ms 的实时线程，并可通过参数设置调度优先级。

## 6. 参数持久化与日志

`CO_storageLinux.c` 实现对象字典参数的本地文件存储：

- 启动时从文件恢复参数；
- 通过 `0x1010` 执行参数保存；
- 通过 `0x1011` 恢复默认参数；
- 对带自动保存属性的参数按周期写回文件；
- 可通过 `-s` 为不同运行实例指定不同的存储路径。

`CO_error.c` 和 `CO_main_basic.c` 提供日志输出。`canopend` 使用 syslog，
默认同时将日志写到标准错误。

## 7. CiA 309-3 命令网关与 cocomm

CANopenLinux 默认构建包含 CANopen ASCII 命令网关。`canopend` 可通过 `-c` 开启：

- 标准输入输出；
- Unix 本地 socket；
- TCP socket。

网关将文本命令转换为 CANopenNode 的 NMT、SDO、LSS 等操作，并将结果转换为文本响应。
socket 接口允许多个客户端使用带序号的命令与响应。

`cocomm/cocomm.c` 是独立命令行客户端，`cocomm/Makefile` 将其编译为 **`cocomm`**。
它连接 `canopend` 的命令 socket，发送 CiA 309-3 命令，并输出响应。
它支持单条命令、交互输入和命令文件。

典型命令包括：

```text
1 read 0x1017 0 u16
1 write 0x1017 0 u16 1000
1 start
1 reset node
```

## 8. 应用回调接口

`CO_application.h` 声明了可选应用回调：

| 函数 | 调用时机 |
|---|---|
| `app_programStart()` | 程序启动、CANopen 初始化之前 |
| `app_communicationReset()` | CANopen 通信复位初始化之后 |
| `app_programAsync()` | 主循环中周期调用，用于非实时处理 |
| `app_programRt()` | 实时线程中周期调用 |
| `app_programEnd()` | 程序结束之前 |

定义 `CO_USE_APPLICATION` 时，`CO_main_basic.c` 调用这些回调；应用实现放在外部源码中。

## 9. 构建与运行

获取仓库和协议核心子模块：

```bash
git clone https://github.com/CANopenNode/CANopenLinux.git
cd CANopenLinux
git submodule init
git submodule update
```

构建通用节点程序：

```bash
make
```

生成文件为仓库根目录下的 `canopend`。运行示例：

```bash
./canopend can0 -i 4
```

开启标准输入输出命令接口：

```bash
./canopend can0 -i 4 -c stdio
```

`make install` 将 `canopend` 复制到 `/usr/bin/`。
`cocomm` 在 `cocomm/` 目录中单独构建和安装。
