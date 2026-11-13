# qt/ — 集成层（本机形态：Qt6 桌面界面）

开发机上的图形壳：把 `control` 层接到界面上，方便没有 orin-agx、没有上位机时手动观察。

- 语言：**C++ / Qt6 Widgets**
- 与 `apps/lyos_node/` **同级、二选一**：两个壳业务上都只依赖 `control/control.h`
- 业务头只允许出现在 `bindings/`；`shell/`、`ui/`、`theme/` 不 include 任何
  `control` / `states` / `axis` / `cia402` / `transport` / `canopen` / `bus` 的头

## 目录

| 路径 | 内容 |
|---|---|
| `main.cpp` | 入口：`QApplication` + `theme::apply()` + 建窗口 + `AppBinder::attach()` |
| `shell/` | 壳层：`MainWindow` 只做区域拼接，不建定时器、不开通道 |
| `ui/` | 区块控件：`state_bar`、`axis_card`、`log_panel`、`status_led` |
| `theme/` | 调色板 `theme.h` + 样式表 `app.qss`（`@token` 由 `theme.cpp` 替换） |
| `res/qt.qrc` | 资源打包（当前只有 `app.qss`） |
| `bindings/` | 唯一接缝：`app_binder`（找控件、刷界面）、`can_thread`（CANopen 工作线程） |

依赖方向：

```
main → bindings → control（业务头只在这里）
main → shell → ui → theme（这一支完全不认识业务）
```

## 线程与时序

- **CANopen 工作线程**：接收接口名，初始化主站并运行 `controller_event_poll()`。
  CAN 数据、基础 timerfd 和命令 eventfd 都会唤醒它，不再运行三个固定节拍线程。
- **界面线程**：`QTimer 50 ms` 读取发布快照刷状态与轴反馈；`QTimer 5 s` 写循环统计。
  按钮通过命令队列下发，eventfd 立即唤醒工作线程，界面不碰总线或设备对象。
- **退出**：停止标志 → eventfd 唤醒 → join → `controller_deinit()`。

## 区域布局

1. **1 号区域**（最小高度 240 px，约占 1/3）：`1.1` = 状态条，`1.2` = 空白预留
2. **2 号区域**：`2.1` = 急停 / 暂停 / 运动 三个按钮（已绑定 `controller_*`），
   `2.2` / `2.3` = `axis_card` ×2 左右等分
3. **3 号区域**：日志面板

**视觉层级：大区域靠空间分，内部子区域靠线分。**

- 大区域之间：10 px 留白露出最深底色 `@deep`；状态区/日志区用 `@surface`，工作区用
  `@bg`，再靠色差区分。
- 区域内部：`1.1 / 1.2`、`2.2 / 2.3` 各一条纵向 1 px 分隔条，`2.1 / 双轴` 一条横向
  1 px 分隔条；分隔条是固定尺寸 `QFrame`（`objectName = divider`），颜色 `@divider`，
  **不使用 QSplitter**（其手柄尺寸受 QSS 影响不稳定）。

## 状态显示

状态条的**状态码不在界面里写死**：由 `bindings` 传 `controller_state_name()` 的结果，
保证界面与日志同一套名字（PREPARING / CONFIGURING / IDLE / ENABLING / HOLDING /
MOVING / HALTING / FAULT / ESTOP）。

界面只按**粗粒度档位**决定圆点颜色与说明文字：

| 控制层状态 | 界面档位 | 圆点 | 说明 |
|---|---|---|---|
| `PREPARING` | `Preparing` | 蓝 | 正在复位节点 |
| `CONFIGURING` | `Preparing` | 蓝 | 正在写参数 |
| `IDLE` | `Idle` | 黄 | 已配置，等待使能指令 |
| `ENABLING` | `Enabling` | 蓝 | 正在让驱动器进入 PP 并使能 |
| `HOLDING`、`MOVING` | `Running` | 绿 | 保持位置，可接受位置指令 |
| `HALTING` | `Halting` | 蓝 | 正在减速停在当前位置 |
| `FAULT` | `Fault` | 红 | 已停住，需重启程序 |
| `ESTOP` | `EStop` | 红 | 已断使能，需重启程序 |

尚未接入控制层时状态码显示占位符 `—`。

## 实时反馈（轴卡片下半部分）

数据源是 CiA 402 设备层的反馈轮转：每 50 ms 读取一个对象
（`6041` 状态字 → `6064` 实际位置 → `606C` 实际速度 → `603F` 驱动器错误码），
与状态机串行共用同一条 SDO 通道（流程优先，监控读让路；超时 100 ms，失败保留上一次有效值）。

- 位置 / 速度在轴层按 `controller/config/cats_mech.h` 的编译期常量换算成 `mm`、`mm/s`
  （当前临时值：1000 pulse/rev、5 mm/rev）；读到过才有值，没读到显示 `—`。
- 状态字显示成 `0x001F` 形式。
- 「错误码」**只显示驱动器自己报的 `603F`**（`0x0000` = 驱动器无错误）；
  主站自己的故障原因（`HEARTBEAT_LOST`、`ENABLE_FAILED`…）属于整机状态，
  显示在顶部状态区，不进轴卡片；SDO abort 只写日志。

## 运动与启动

- 「目标位置 / 目标速度 / 目标加速度」三项用 `mm`、`mm/s`、`mm/s²`，与
  `controller_move()` 的单位一致。数值是 `float`，取整到脉冲只发生在轴层，
  机械换算参数是编译期常量（`controller/config/cats_mech.h`）。
- 运动按钮按轴选择器提交单轴或双轴命令；暂停与急停分别调用停止和急停接口。
- 启动不需要按钮：`controller_init()` 建完即 `PREPARING`；配置完成后停在 `IDLE`，
  点“使能”按钮才进 `ENABLING`。

## 命名规范

- 文件 / 目录 `snake_case`；类 `CamelCase` 且与文件名一致。
- 每个控件都有 `objectName`（`targetPosition`、`value`、`logView`…），QSS 只按
  objectName 命中，不依赖控件层级。
- 颜色只在 `theme/theme.h` 定义；QSS 用 `@bg`、`@ok`、`@fault` 占位符，不写死色值。

## 构建与运行

```bash
cmake -S firmware -B firmware/build
cmake --build firmware/build -j
./firmware/build/cats_qt            # 需要 can0 已 up，否则日志里会提示初始化失败
```

离屏冒烟（无显示器时确认能构造出界面）：

```bash
QT_QPA_PLATFORM=offscreen timeout 3 ./firmware/build/cats_qt
```
