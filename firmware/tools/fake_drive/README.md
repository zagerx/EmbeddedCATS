# fake_drive/ — CANopen 从站模拟器

没有真实驱动器时，用 PCAN 的两个通道对连，模拟一台双轴驱动器（= 两个 CANopen 节点）。

```text
can0（主站口）──── 串联 120Ω 双绞线 ──── can1（从站口）
   ↑                                        ↑
CATS 主站                            tools/fake_drive/fake_drive.py
```

## 当前支持范围

| 功能 | COB-ID | 状态 |
|---|---|---|
| NMT start / stop / enter pre-op / reset node / reset communication | `0x000` | 已实现 |
| boot-up | `0x700+id` | 已实现（进程启动、收到 reset 时发） |
| SDO 加速读（读写 1/2/4 字节对象） | `0x600+id` / `0x580+id` | 已实现 |
| PDO / SYNC / 心跳 / EMCY / 分段 SDO / 402 状态机 | — | 未实现 |

## 用法

```bash
# 1. 打开两个通道（标准 CAN 2.0，1 Mbit）
bash ~/.yakut/setup_dual.sh

# 2. 启动模拟从站（默认 can1，站号 1、2）
python3 firmware/tools/fake_drive/fake_drive.py

# 可选：-i can1 -n 1,2 -t（-t 打印每个原始帧）
```

手工验证（在 can0 侧发，看 can1 侧程序的输出）：

```bash
# NMT start，站号 1
cansend can0 000#0101

# SDO 读 0x1000:00（期望响应 0x581: 43 00 10 00 92 01 02 00 = 0x00020192）
cansend can0 601#4000100000000000

# 读不存在的对象（期望 abort 0x06020000）
cansend can0 601#4099000000000000
```

## 约定

- 对象字典的数值是 **CiA 301/402 标准占位值**，不是 2LD2-CAN7020B 的实测值。
  真机到货后先跑 `tools/od_scan/` 取快照再替换，避免把主站的期望当成事实。
- 模拟器**只认 CAN 帧**，不 include `canopen/`、`cia402/`、`axis/` 的任何代码；
  它是测试装置，不是固件的一部分。
- 一帧响应只发一次，不做重传与超时，时序上的抖动会如实反映真实 USB-CAN 的延迟。
