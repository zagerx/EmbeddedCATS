# master_probe — 无界面运行器

界面层（`apps/qt/bindings/`）能跑同样的东西，但它的日志在面板里，
不好 grep。这个小程序就是命令行版的“发动机”：开通道 → 建主站（节点 1/2）→ 每 1 ms 推进一次状态机。

## 用法

```bash
./build/master_probe can0 30     # 跑 30 秒
./build/master_probe can0        # 一直跑，Ctrl-C 退出
```

### 注入指令（可选）

```bash
# 2 mm 绝对位置，速度 4 mm/s；换算参数是**测试用**的（不是实测机械参数）
./build/master_probe can0 10 --pulses 10000 --lead 5000 --move 2000 --vel 4000

# 运动中停（MOVING → STOPPING → READY）
./build/master_probe can0 10 --pulses 10000 --lead 5000 --move 20000 --vel 4000 --stop-at 2500

# 运动中急停（MOVING → ESTOP）
./build/master_probe can0 10 --pulses 10000 --lead 5000 --move 20000 --vel 2000 --estop-at 3000
```

| 参数 | 含义 |
|---|---|
| `--move <mm>` / `--move-at <ms>` | 位置指令（默认 1500 ms 发，等使能完） |
| `--vel <mm/s>` / `--acc <mm/s²>` | 速度与加速度（默认 10 / 50） |
| `--stop-at <ms>` / `--estop-at <ms>` | 到点发停止 / 急停 |
| `--pulses <n>` / `--lead <mm>` | 每转脉冲数与每转毫米（开跑前打印一行提醒这是测试值） |

## 输出

```text
接口 can0，节点 1/2，节拍 1 ms
[     0] state = PREPARING
[  1012] state = RUNNING
```

## 它能验证什么

| 场景 | 期望 |
|---|---|
| 驱动器响应 0x82 并正常发心跳 | 静默 1000 ms 后 `state = RUNNING` |
| 驱动器始终不发心跳 | 3000 ms 后 `state = FAULT (BOOTUP_TIMEOUT)` |
| `RUNNING` 中停掉驱动器心跳 | 约 750 ms 后 `state = FAULT (HEARTBEAT_LOST)` |

节拍 1 ms，与 I/O、服务线程一致：指令从下发到状态机响应只差一拍。
750 ms 心跳判定误差 0.13%。
