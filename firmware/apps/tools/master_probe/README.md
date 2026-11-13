# master_probe — 无界面 CANopen 事件循环验证器

主线程就是唯一的 CANopen 工作线程。它按接口名调用 `controller_init()`，随后运行
`controller_event_poll()`，定时命令在每轮事件后注入；退出时调用 `controller_deinit()`。

## 用法

```bash
./build/master_probe can0 30     # 跑 30 秒
./build/master_probe can0        # 一直跑，Ctrl-C 退出
```

### 注入指令（可选）

```bash
# 单轴 5 mm，1 秒时发，1.2 秒时减速停，1.8 秒时急停
./build/master_probe can0 6 --move 5 --move-at 1000 --stop-at 1200 --estop-at 1800

# 两轴同时运动；move2-at 与 move-at 相同则走批量接口
./build/master_probe can0 10 --move 5 --move2 8 --move-at 1500 --move2-at 1500

# 速度与加速度覆盖默认值 10 mm/s、50 mm/s²
./build/master_probe can0 10 --move 5 --vel 4 --acc 20
```

| 参数 | 含义 |
|---|---|
| `--move <mm>` / `--move-at <ms>` | 轴 1 位置与注入时刻 |
| `--move2 <mm>` / `--move2-at <ms>` | 轴 2 位置与注入时刻 |
| `--vel <mm/s>` / `--acc <mm/s²>` | 速度与加速度 |
| `--stop-at <ms>` / `--estop-at <ms>` | 停止 / 急停注入时刻 |
| `--spam <n>` / `--spam-at <ms>` | 连续提交 n 条命令，检查队列拒绝行为 |

机械换算参数固定来自 `controller/config/cats_mech.h`，不再通过命令行覆盖。

## 输出与验收

```text
接口 can0，节点 1/2，CANopenLinux epoll（基础定时 1 ms）
[     0] state = PREPARING
[   ...] state = CONFIGURING
[   ...] state = ENABLING
[   ...] state = HOLDING
```

配合 `fake_drive` 可检查双轴配置、使能、运动、停止、急停，以及 eventfd 唤醒后的
命令响应。CAN 数据到来也会提前唤醒；基础定时器只负责周期推进和超时，不再是唯一
的收帧途径。
