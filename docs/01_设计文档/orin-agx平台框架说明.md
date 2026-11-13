# orin-agx 平台框架说明

> 本文只描述 orin-agx 侧的**既有框架**，用于界定 CATS 未来接入的位置与接口形态。
> 平台代码为他人维护，本文仅作阅读结论，**不代表可修改**。
> 返回：[CATS 项目说明](CATS项目说明文档.md)

## 1. 一句话定位

`build_all` 是总装 + 编译 + 发布外壳：拉取多个独立仓库 → colcon 编成 `/app` →
`system_manager` 按配置拉起一组 lyos 进程 → 进程之间只用 topic 通信 → 驱动进程内部自行分层。

## 2. 总体结构

```text
build_all/                     总装仓库（Makefile + repos/ + ament/ + script/ + toolchain/）
├── repos/<产品>/<平台>.repos   各产品平台的仓库清单与分支（vcs import 依据）
├── src/                       每个子目录是一个独立 git 仓库
│   ├── platform/              lyos 平台中间件（lyoslib / DDS / system_manager）
│   ├── robot_ai_msgs/         消息定义（lyos_msg）
│   ├── lyos_app/              应用层：单进程多 actor
│   ├── algo-lrs/              算法层
│   └── driver/                驱动层（独立仓库 driver.git）
└── build/<PRODUCT>/<PLATFORM>/install   编译产物（对外即 /app）
```

产品线：`lrs-x`、`lrd-w`、`chassis`、`chx`。**orin-agx 属于 `chx` 产品的 `agx` 平台**
（对应 `repos/chx/agx.repos`）。

## 3. 构建与发布

| 环节 | 说明 |
|---|---|
| 拉取 | `make vcs_x86_64` 等目标，按 `repos/<产品>/<平台>.repos` 拉齐仓库 |
| 编译 | `ament_cmake` + `colcon build --merge-install`，产物落 `build/<PRODUCT>/<PLATFORM>/install` |
| 布局 | `/app/bin`（可执行）、`/app/etc`（配置，环境变量 `ENV_ROBOTETC`）、`/app/lib` |
| 发布 | `script/release/`（打包、版本管理、升级文件），顶层 Makefile 内含 Jenkins 与 OTA 地址 |

## 4. 平台中间件（`src/platform` = lyos）

| 模块 | 作用 |
|---|---|
| `lyoslib` | ROS 风格 API：`lyos::init(name)` / `lyos::spin()` / `lyos::nh()`，topic 发布订阅、param、tf、日志 |
| `modules/lyosdds` | DDS 通信层（含 `lyoscdr`、`fastddsgen`） |
| 其他 | 共享内存、yaml、日志、asio 封装等 |
| `system_manager` | 进程管理器：按 yaml 依优先级启动 `/app/bin/*`，监控 + 崩溃自动重启 + 资源限制 |

进程自启配置位于 `platform/system_manager/config/system_manager/<产品>/<平台>/robot_init*.yaml`，
`monitor.yaml` 管理 CPU 频率/温度告警阈值。

## 5. 应用层

- `lyos_app`：**单进程多 actor**（`state_control` 系统状态机、`cmd_controller`、`power_control`、
  `led_control`、`upgrade`、`iot_rccontroller` 手柄）。
- `algo-lrs`：算法层，产出电机目标指令。

## 6. 驱动层（`src/driver`）

每个驱动 = **一个独立 lyos 进程**，骨架统一：

```text
main → lyos::init("xxx_driver") → 解析 --etc（默认 /app/etc）
     → 读取 yaml → 构造 Hal 对象 → start() → lyos::spin()
```

| 驱动 | 说明 |
|---|---|
| `camera` / `imu` / `power` | 相机 / IMU / 电源（电源驱动内部已有原生 SocketCAN 用法） |
| `motor` | UDP 电机驱动 |
| `robot_ecat_driver` | EtherCAT 驱动，唯一的"重"驱动：内部再分 Scheduler / Runtime / Adapter / Supervisor / Robot |

对外接口统一为 topic（以 ecat 驱动为例）：

| topic | 方向 |
|---|---|
| `/motor_control/motor_control` | 进（算法 → 驱动） |
| `/motor_control/motor_comand_feedback` | 出（驱动 → 算法） |
| `/motor_control/motor_health` | 出（健康状态） |
| `/robot_ecat/status`、`/robot_ecat/cmd` | 出 / 进（状态与命令） |

## 7. 整体控制链

```text
手柄/APP → /iot/command → state_control → /system/lrs_cmd → 算法
        → /motor_control/motor_control → 驱动进程 → CAN / EtherCAT / UDP → 电机
        ← /motor_control/motor_comand_feedback ← 电机
```

## 8. CATS 的接入位置

- **对等替换**：CATS 未来作为 `src/driver/` 下的新驱动包，与 `motor`（UDP）驱动同一层级。
- **接入即换壳**：只替换 CATS 的集成层（进程入口 + 对外 topic 映射），内核层与设备/轴层不动。
- **边界层统一**：开发机与 Orin-AGX 均使用 SocketCAN，由上游 `CANopenLinux` 承接。

对应关系：

| CATS 层 | 接入 orin-agx 时的形态 |
|---|---|
| 集成层 | `main` + `lyos::init` + 订阅/发布上表 topic |
| 应用层 | 启动/使能/故障复位流程，节拍驱动 |
| 轴层 | 2 个轴的目标与反馈，映射到 `motor_control` 消息字段 |
| 设备层 + 内核层 | 原样保留 |
| 边界层 | 沿用 CANopenLinux 的 SocketCAN 后端 |
