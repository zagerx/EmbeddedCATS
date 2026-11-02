# master_probe — 无界面运行器

界面层（`src/qt/bindings/`）能跑同样的东西，但它的日志在面板里，
不好 grep。这个小程序就是命令行版的“发动机”：开通道 → 建主站（节点 1/2）→ 每 20 ms 推进一次状态机。

## 用法

```bash
./build/master_probe can0 30     # 跑 30 秒
./build/master_probe can0        # 一直跑，Ctrl-C 退出
```

## 输出

```text
接口 can0，节点 1/2，节拍 20 ms
[  1234] IDLE -> PREPARING
[  1234] state = PREPARING
[  1300] PREPARING -> RUNNING
[  1300] state = RUNNING
```

## 它能验证什么

| 场景 | 期望 |
|---|---|
| 从站复位后发 boot-up | `PREPARING → RUNNING` |
| 从站一直不发 boot-up | 2000 ms 后 `FAULT: BOOTUP_TIMEOUT` |
| `RUNNING` 中停掉从站心跳 | 约 750 ms 后 `FAULT: HEARTBEAT_LOST` |
| `RUNNING` 中从站又发 boot-up | `FAULT: NODE_REBOOTED` |

节拍 20 ms 对 750 ms 判定只引入 2.7% 误差；将来做运动（PP 模式 2~10 ms）再升级节拍。
