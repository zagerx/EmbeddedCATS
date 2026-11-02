# tools/

辅助工具，不参与固件运行。

| 子目录 | 说明 |
|---|---|
| `master_probe/` | 无界面运行器：20 ms 推进 `cats_app_poll()`，验证启动流程与心跳监控 |
| `fake_drive/` | CANopen 从站模拟器：无驱动器时在两路 PCAN 上模拟双轴节点（NMT + SDO + 心跳） |

`master_probe` 是命令行版的运行器：日志落在终端，方便 grep 和长时间跑。

`fake_drive` 是测试装置：用 PCAN 两个通道对连（`can0` 主站 / `can1` 从站）替代真实驱动器，
让主站在没有硬件时也能跑真实 CAN 报文；它的对象字典只是标准占位值。
两者都只认 CAN 帧，不引用 `src/` 下任何模块（`master_probe` 例外：它本身就是 app 的宿主）。

## 尚未实现

只读对象字典扫描（遍历索引/子索引、记录存在性与 abort 码、导出 CSV）原来规划在 `od_scan/`，
该目录已改成 `master_probe/`；这个工具**还没写**，需要时另建目录。
