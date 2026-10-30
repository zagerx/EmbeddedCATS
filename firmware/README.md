# CATS Firmware

CANopen（CiA 402）双轴主站固件。开发目标与分层原则见 [`../docs/01_设计文档/CATS项目说明文档.md`](../docs/01_设计文档/CATS项目说明文档.md)。

> 当前状态：**仅目录骨架**，尚未添加构建脚本与实现代码。

## 语言

| 层 | 语言 |
|---|---|
| 内核层 / 边界层 / 设备层 / 轴层 / 应用层 | **C99** |
| 集成层（`src/cli`、`src/lyos_node`） | **C++** |

## 目录

```text
firmware/
├── cmake/            构建辅助（架构检查、编译选项）
├── config/           运行期配置：总线、节点、PDO 布局、轴映射
├── src/
│   ├── canopen/      内核层：CANopen 协议核心（含 can_io.h 接口）
│   ├── backends/     边界层：can_io.h 的实现（本机 / 回环 / 平台占位）
│   ├── cia402/       设备层：CiA 402 语义与 402 状态机
│   ├── axis/         轴层：轴命令 / 反馈 / 单位换算
│   ├── app/          应用层：流程、调度、装配
│   ├── cli/          集成层·本机形态
│   └── lyos_node/    集成层·orin-agx 形态（占位）
└── tools/            辅助工具（只读对象字典扫描等）
```

## 依赖方向（单向，不可逆）

```text
cli / lyos_node  →  app  →  axis  →  cia402  →  canopen  ←  backends
```

- `canopen`（内核）只依赖自己定义的能力接口 `can_io.h`，**不依赖任何 backend 实现**；
- `backends` 实现 `can_io.h`，由集成层在装配时注入；
- 具体 backend 由 `app` / 集成层选择，下层不得反查。

## 可见性约定

- 每个模块只从自己的 `include/` 目录发布公共头，实现细节留在模块内部；
- 跨层调用只能 include 对方的公共头；
- 平台相关头文件（如 `linux/can.h`）只允许出现在 `src/backends/socketcan/` 内。
