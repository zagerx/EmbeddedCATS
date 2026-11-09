# firmware/ — 固件源码

```text
firmware/
├── controller/   控制主体：各层 + 主站（CANopenNode）+ 组合根，不创建线程
├── apps/         宿主：谁起线程、怎么装配、界面/命令行
├── cmake/        构建辅助
└── CMakeLists.txt
```

## 两个目录的分工

- **`controller/`** 是"该做什么"：从 CAN 帧到业务状态机的全部逻辑。
  它**不创建线程**，只提供三个"推进一步"的接口
  （`cats_control_io_poll` / `cats_control_poll` / `cats_control_svc_poll`）。
- **`apps/`** 是"什么时候做"：起线程、定节拍、把事件送进状态机、显示状态。

换个运行环境时，换掉 `apps/` 里的宿主即可——`controller/` 一行不用改。

| 宿主 | 说明 |
|---|---|
| `apps/qt/` | 现役产品界面（Qt6 Widgets）：io+ctrl 一条线程，svc 一条 |
| `apps/tools/master_probe/` | 命令行验证器：同样三线程模型，无界面 |
| `apps/tools/fake_drive/` | CANopen 从站模拟器（Python），无硬件时替代驱动器 |
| `apps/lyos_node/` | AGX 平台宿主，占位（未来实现） |

## 构建与验收

```bash
cmake -S . -B build && cmake --build build -j4     # 期望 0 error / 0 warning
./build/ds402_selftest                             # PDS：状态字解析与起点快进
apps/tools/layering_check.sh                       # R1–R10 边界检查
```

端到端（can0 主站 / can1 模拟从站，两路需先起好）：

```bash
python3 apps/tools/fake_drive/fake_drive.py -i can1 -n 1,2 &
./build/master_probe can0 6 --move 5 --move-at 1000 --stop-at 1800
```

层次定义、依赖方向与线程模型见 `docs/01_设计文档/分层与线程模型.md`。
