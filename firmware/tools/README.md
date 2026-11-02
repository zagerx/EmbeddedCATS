# tools/

辅助工具，不参与固件运行。

| 子目录 | 说明 |
|---|---|
| `master_probe/` | 无界面运行器：20 ms 推进 `cats_app_poll()`，验证启动流程与心跳监控 |
| `fake_drive/` | CANopen 从站模拟器：无驱动器时在两路 PCAN 上模拟双轴节点（NMT + SDO + 心跳） |
| `hsm_demo/` | 状态机骨架：一状态一文件，`main.c` 暂空 |

`master_probe` 是命令行版的运行器：日志落在终端，方便 grep 和长时间跑。

`fake_drive` 是测试装置：用 PCAN 两个通道对连（`can0` 主站 / `can1` 从站）替代真实驱动器，
让主站在没有硬件时也能跑真实 CAN 报文；它的对象字典只是标准占位值。
两者都只认 CAN 帧，不引用 `src/` 下任何模块（`master_probe` 例外：它本身就是 app 的宿主）。

## hsm_demo

状态机骨架：`main.c` 暂空，一状态一文件，`states.c` 是唯一注册点。只依赖
`src/app/src/hsm/hsm.c`，不链 `cats_app`。

```
tools/hsm_demo/
├── main.c              入口（暂空，只留空 main 保证能链接）
├── states.h / states.c 状态声明、业务信号、注册表
├── state_root.c        ROOT
├── state_operational.c OPERATIONAL（IDLE / RUNNING / STOPPING 的父状态）
├── state_idle.c / state_running.c / state_stopping.c
└── state_fault.c / state_estop.c
```

## 尚未实现

只读对象字典扫描（遍历索引/子索引、记录存在性与 abort 码、导出 CSV）原来规划在 `od_scan/`，
该目录已改成 `master_probe/`；这个工具**还没写**，需要时另建目录。
