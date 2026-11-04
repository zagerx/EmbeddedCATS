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
| SDO 加速读 / 写（1/2/4 字节对象） | `0x600+id` / `0x580+id` | 已实现 |
| 每节点独立的对象字典 | — | 已实现（写节点 1 不影响节点 2） |
| 心跳生产者（`0x1017`，内容 = 当前 NMT 状态） | `0x700+id` | 已实现（默认 500 ms，boot-up 后自动开跑） |
| CiA 402 **使能**（`6040` → `6041`：0x40 → 0x21 → 0x23 → 0x27，bit7 故障复位） | `0x600+id` | 已实现（`--no-402` 可关掉，用来测使能超时） |
| CiA 402 **PP 最小运动模型**：写 `6081/6083/6084`，`6040` bit4 上升沿 ⇒ 按速度匀速走完并置 `6041` bit10；bit8 = 减速停（直接停在当前位置）；`6040=0x02` = quick stop | `0x600+id` | 已实现（无加减速曲线；位置可从 `6064` 读） |
| EMCY / 分段 SDO / PDO / SYNC | — | 未实现 |

经典 CAN（≤ 8 字节），不做 CAN FD。

心跳行为：**出厂周期 500 ms**，所以 `drive.start()` 发完 boot-up 帧就开始按周期发；
`--heartbeat-ms N` 改变出厂周期，`--heartbeat-ms 0` 则不主动发（出厂就不发，
等主站写 `0x1017`）。主站写 `0x1017` 即从写入时刻改成新周期，写 0 停发；
NMT 状态一变，心跳里的状态字节跟着变（Stopped 时照样发，值为 `0x04`）；
`reset communication` 把周期回出厂值（即启动参数）并继续发。
不做节点保护（远程帧）。

## 模块结构

```text
fake_drive/
├── fake_drive.py   CLI + 事件循环 + 总线分发（唯一装配点）
├── node.py         单节点的 CANopen 行为（NMT / SDO 编解码 / 心跳时基）
├── od.py           对象字典：条目规则 + 每节点一份的实例
├── od_data.json    条目数据（数值 / 访问属性 / 出处）
└── selftest.py     虚拟总线自测（不需要硬件）
```

依赖单向：`fake_drive → node → od → od_data.json`。`od.py` 不含 CAN，
`node.py` 不含总线循环。

## 对象字典

- 通信对象（`0x1000`~`0x1018`）：手册 8.5 只说明该区遵循通讯子协议，未列逐条数值
  ⇒ 用 DS 301 标准值。
- 402 对象（`0x603F`~`0x607E`）：按手册 5.1.3 参数表 + 5.3 位定义填初始值与访问属性。
- 每条都带 `ref` 字段记录出处；手册没给的数值填 0 并标注“待 od_scan”，不做推测。
- `0x6040` / `0x6041` 目前只是“可读写的存储”：402 状态机还没实现，写控制字不会改变响应。

## 自测（无硬件）

```bash
python3 firmware/tools/fake_drive/selftest.py
```

用 python-can 的 `virtual` 接口在同进程里开主/从两个口，逐帧断言应答。
加功能前先在这里补用例，再上真实 CAN 口对连复验。

## 用法

```bash
# 1. 打开两个通道（标准 CAN 2.0，1 Mbit）
bash ~/.yakut/setup_dual.sh

# 2. 启动模拟从站（默认 can1，站号 1、2）
python3 firmware/tools/fake_drive/fake_drive.py

# 可选：-i can1 -n 1,2 -t（-t 打印每个原始帧）
#      --heartbeat-ms 1000（改出厂心跳周期，默认 500；0 = 不主动发）
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
- 不按 `is_rx` 过滤自己的帧：自发的 `0x580+id` / `0x700+id` 不在节点接收集合里；
  而 python-can 的 virtual 接口不回填 `is_rx`，按它过滤会把帧全丢掉。
