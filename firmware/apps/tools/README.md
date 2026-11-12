# tools/

辅助工具，不参与固件运行。

| 子目录 | 说明 |
|---|---|
| `master_probe/` | 无界面宿主：I/O 与服务各一线程，主循环按 1 ms 推进状态机 |
| `fake_drive/` | CANopen 从站模拟器：NMT、加速 SDO、心跳和最小 402/PP 模型 |
| `cia402_selftest/` | PDS 状态解析与起点快进纯逻辑自测 |
| `layering_check.sh` | 分层边界检查 R1–R10 |

`master_probe` 将日志输出到终端，便于检索和长时间回归。`fake_drive` 可在两路 CAN 对连时替代真实驱动器，也提供基于 python-can virtual 接口的无硬件自测。

对象字典扫描工具尚未实现，需要时另建目录。
