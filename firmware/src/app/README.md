# app/ — 应用层

流程与装配：把各层对象组装起来，驱动起来。

## 结构

```text
app/
├── include/app/   公共头（启动流程、周期调度入口）
└── src/           装配与调度实现
```

## 职责

- 启动 / 停止顺序与失败回退
- 周期调度（唯一的节拍来源）
- 配置加载与校验
- 故障策略（通信超时、驱动器报警 → 系统动作）
- **唯一装配点**：所有对象的创建与注入只在这里发生

## 边界

- 只依赖 `axis`、`cia402`、`canopen` 的公共头，以及 `can_io.h` 接口；
- 不直接操作 PDO 字节、不直接调用平台 CAN 接口；
- 具体 backend 由上层（集成层）传入，本层不 `#include` 任何 backend 实现。

## 对外契约（`include/app/app.h`）

集成层只 include 这一个头，接口为：
`cats_app_init` / `cats_app_poll` / `cats_app_state` / `cats_app_state_name` /
`cats_app_estop` / `cats_app_set_log`。

- **单线程轮询**：由壳按 10~20 ms 调 `cats_app_poll()`，本层内部不用线程、不用回调；
- **启动不靠外部事件**：`cats_app_init()` 建好状态机即为 PREPARING，之后由 `poll` 自行推进；
- **单例**：一个进程一条总线，静态实例，不动态分配内存；
- 时序、超时、状态全在本层及以下，壳只负责“什么时候调”。

> `app.c` 目前是骨架（记住总线指针、维护状态、拦住非法调用）；
> 时序与协议交互随功能逐步填进 `cats_app_poll()`。
