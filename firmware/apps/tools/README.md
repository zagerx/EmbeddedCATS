# tools/

辅助工具，不参与固件运行。

| 子目录 | 说明 |
|---|---|
| `master_probe/` | 无界面运行器：自起 I/O（1 ms）与 svc 两线程，主循环 20 ms 推进 `cats_control_poll()` |
| `fake_drive/` | CANopen 从站模拟器：无驱动器时在两路 PCAN 上模拟双轴节点（NMT + SDO + 心跳） |
| `canopen_selftest/` | 协议栈单测：SDO 报文编解码 + 事务槽状态机（不碰 CAN） |
| `ds402_selftest/` | PDS 知识单测：状态字解析 + 起点快进（不碰 CAN） |
| `layering_check.sh` | 分层边界检查 R1–R10 |

`master_probe` 是命令行版的运行器：日志落在终端，方便 grep 和长时间跑。

`fake_drive` 是测试装置：用 PCAN 两个通道对连（`can0` 主站 / `can1` 从站）替代真实驱动器，
让主站在没有硬件时也能跑真实 CAN 报文；它的对象字典只是标准占位值。
`fake_drive` 只认 CAN 帧，不引用 `controller/` 下任何模块；`master_probe` 是宿主，只调 `control/control.h` 的接口。

## 尚未实现

只读对象字典扫描（遍历索引/子索引、记录存在性与 abort 码、导出 CSV）：**还没写**，
需要时另建目录。
