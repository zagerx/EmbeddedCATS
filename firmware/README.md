# firmware/ — 固件源码

```text
firmware/
├── controller/   控制主体：各层 + CANopenLinux 主站 + 组合根，不创建线程
├── apps/         宿主：线程、事件循环、界面/命令行
├── cmake/        构建辅助
└── CMakeLists.txt
```

## 两个目录的分工

- **`controller/`** 是“该做什么”：从 CAN 帧到业务状态机的全部逻辑。
  对外生命周期为 `controller_init(ifname)` / `controller_deinit()`；工作线程运行
  `controller_event_poll()`，整机状态机内部仍由 `controller_poll()` 推进。
- **`apps/`** 是“什么时候做”：创建工作线程、传递接口名、注入命令、显示状态。

| 宿主 | 说明 |
|---|---|
| `apps/qt/` | Qt6 界面：UI 线程 + 一个 CANopen 工作线程 |
| `apps/tools/master_probe/` | 主线程直接运行同一事件循环 |
| `apps/tools/fake_drive/` | CANopen 从站模拟器，无硬件时替代驱动器 |
| `apps/lyos_node/` | AGX 平台宿主，占位（未来实现） |

CANopenLinux 管理 SocketCAN、epoll、timerfd 和 eventfd。协议栈、SDO、CiA402 与
整机状态机只在同一工作线程内访问；UI 命令通过带锁队列和 eventfd 进入，查询只读
发布快照。

## 构建与验收

```bash
cmake -S . -B build && cmake --build build -j4
./build/cia402_selftest
apps/tools/layering_check.sh
python3 apps/tools/fake_drive/selftest.py
```

端到端（can0 主站 / can1 模拟从站，两路需先起好）：

```bash
python3 apps/tools/fake_drive/fake_drive.py -i can1 -n 1,2 &
./build/master_probe can0 6 --move 5 --move-at 1000 --stop-at 1200 --estop-at 1800
```

层次定义、依赖方向与线程模型见 `docs/01_设计文档/分层与线程模型.md`。
