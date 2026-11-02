# qt/ — 集成层（本机形态：Qt6 桌面界面）

开发机上的**图形壳**。当前阶段**只有界面、没有逻辑**：能编译、能跑、能看到样子，
按钮点了没反应是预期。

- 语言：**C++ / Qt6 Widgets**
- 与 `lyos_node/` **同级、二选一**：两个壳最终都只依赖 `app/include/app/app.h`
- 本版**不 include** `canopen` / `cia402` / `axis` / `backends` / `app` 的任何头

## 目录

| 路径 | 内容 |
|---|---|
| `main.cpp` | 入口：`QApplication` + `theme::apply()` + 窗口 |
| `shell/` | 壳层：`MainWindow` 只做区域拼接 |
| `ui/` | 区块控件：`state_bar`、`axis_card`、`log_panel`、`status_led` |
| `theme/` | 调色板 `theme.h` + 样式表 `app.qss`（`@token` 由 `theme.cpp` 替换） |
| `res/qt.qrc` | 资源打包（当前只有 `app.qss`） |
| `bindings/` | **预留·暂空**：将来接 `cats_app_*` 的唯一落点，约定见其 README |

依赖方向：`main → shell → ui → theme`，**单向**；`ui/` 不知道壳的存在。

## 区域布局

1. **1 号区域**：左右均分。`1.1` = `state_bar`（状态机状态），`1.2` = 一个「启动」按钮；最小高度 240 px（约占 1/3）
2. **2 号区域**：`2.1` = 空白预留（64 px），`2.2` / `2.3` = `axis_card` ×2 左右等分
3. **3 号区域**：`log_panel`，日志面板

**视觉层级：大区域靠空间分，内部子区域靠线分。**

- 大区域之间：`root` 布局 10 px 留白，露出最深底色 `@deep`；状态区/日志区用表面色
  `@surface`，中部工作区用 `@bg`，靠色差再区分。
- 区域内部：`1.1 / 1.2`、`2.2 / 2.3` 各一条纵向 1 px 分隔条，`2.1 / 双轴` 一条横向 1 px 分隔条。
  分隔条是固定尺寸的 `QFrame`（`objectName = divider`），颜色 `@divider`，
  **不使用 QSplitter**（其手柄尺寸受 QSS 影响不稳定，曾导致线条画不出来）。

要核对效果可直接截图自查：

```bash
QT_QPA_PLATFORM=offscreen ./firmware/build/cats_qt   # 仅冒烟
```

## 状态机状态

`StateBar` 的枚举与 `app` 的 `cats_run_state` **一一对应**，但界面侧不 include `app.h`：

| StateBar::State | 状态码 | 中文 | 圆点颜色 |
|---|---|---|---|
| `Idle` | IDLE | 空闲 | 灰 |
| `Starting` | STARTING | 启动中 | 蓝 |
| `Running` | RUNNING | 运行中 | 绿 |
| `Stopping` | STOPPING | 停止中 | 蓝 |
| `Fault` | FAULT | 故障 | 红 |
| `EStop` | ESTOP | 急停 | 红 |

本版固定显示 `Idle`（`StateBar` 构造末尾调用一次 `setState(State::Idle)`），
接逻辑时由 `bindings/` 把 `cats_app_state()` 映射过来。

## 命名规范

- 文件 / 目录 `snake_case`；类 `CamelCase` 且与文件名一致。
- 每个控件都有 `objectName`（`btn_estop`、`btn_start`、`targetPosition`、`value`…），
  QSS 只按 objectName / 自定义属性 `role` / `active` 命中，不依赖控件层级。
- 颜色只在 `theme/theme.h` 定义；QSS 用 `@bg`、`@ok`、`@fault` 这类占位符，不写死色值。

## 构建与运行

```bash
cmake -S firmware -B firmware/build
cmake --build firmware/build -j
./firmware/build/cats_qt
```
