# master_probe — 无界面运行器

界面层（`apps/qt/bindings/`）能跑同样的东西，但它的日志在面板里，
不好 grep。这个小程序就是命令行版的“发动机”：开通道 → 建主站（节点 1/2）→ 每 20 ms 推进一次状态机。

## 用法

```bash
./build/master_probe can0 30     # 跑 30 秒
./build/master_probe can0        # 一直跑，Ctrl-C 退出
```

## 输出

```text
接口 can0，节点 1/2，节拍 20 ms
[     0] state = PREPARING
[  1012] state = RUNNING
```

## 它能验证什么

| 场景 | 期望 |
|---|---|
| 驱动器响应 0x82 并正常发心跳 | 静默 1000 ms 后 `state = RUNNING` |
| 驱动器始终不发心跳 | 3000 ms 后 `state = FAULT (BOOTUP_TIMEOUT)` |
| `RUNNING` 中停掉驱动器心跳 | 约 750 ms 后 `state = FAULT (HEARTBEAT_LOST)` |

节拍 20 ms 对 750 ms 判定只引入 2.7% 误差；将来做运动（PP 模式 2~10 ms）再升级节拍。
