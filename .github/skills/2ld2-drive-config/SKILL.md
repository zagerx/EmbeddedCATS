---
name: 2ld2-drive-config
description: '把一台新的 2LD2-CAN7020B 双轴驱动器配置成与台架基准完全一致并验收。Use when: 新驱动器上机、更换驱动器、驱动器恢复出厂、需要在另一台电脑复刻配置、驱动器配置验收。步骤：can0 500k 起链路 → 心跳探活 → 只读核对 → 写输入分配/方向 → 保存 → 断电重启 → drive_setup 配 TPDO/回零/急停/抱闸 → 按开关复验 → PP 点动与回零验收。基准值见 docs/02_驱动器说明/2LD2-CAN7020B驱动器配置基准.md。铁律：写 2403h 必须先于 2404h；所有写入后必须 save；必须断电重启后复验；拔改拨码后必须断电重启；不改 180x:03 禁止时间；605Ah 保存后必须重启才生效。'
argument-hint: '<CAN 接口名，默认 can0>'
---

# 2LD2-CAN7020B 驱动器配置（照基准复刻）

## 何时使用

- 一台新驱动器/换机/恢复出厂后，要配置成与台架基准一致。
- 需要验收某台驱动器是否符合基准。

## 前提

- Linux + SocketCAN + PEAK PCAN-USB（FD）适配器；`python-can` 已安装（`pip install python-can`）。
- 驱动器上电：48V 主电 + STO 24V 短接；`SW6=on`；站号旋码 1/2；波特率拨码 500k。
- **没有其他程序占用 CAN**（`cats_qt` / `master_probe` / `drive_setup` 同时只能跑一个）。
- 接口操作需要 root（sudo 由人工执行）。

不要凭记忆写值——本 skill 的所有数字以
`docs/02_驱动器说明/2LD2-CAN7020B驱动器配置基准.md` 为准。

## 步骤

### 1. 起链路

```bash
sudo ip link set can0 down
sudo ip link set can0 up type can bitrate 500000 restart-ms 100
ip -details link show can0 | sed -n '3,4p'
```

### 2. 心跳探活

```bash
python3 .github/skills/2ld2-drive-config/scripts/baseline_sdo.py probe can0
```

期望 3 s 内收到 `0x701` 和 `0x702`（1 Hz）。收不到 → 查 `SW6`、接线、拨码是否 500k（改过拨码必须断电重启）。

### 3. 只读核对

```bash
python3 .github/skills/2ld2-drive-config/scripts/baseline_sdo.py check can0
```

必须全部 PASS：`1000h`=0x00020192、`6502h`=0x3002D、`608Fh`=131072/1、`6092h:01`=10000、
`2023h`=1/2、`2024h`=2、`6079h`≈47~48V。

### 4. 写输入分配与方向（顺序不可颠倒）

```bash
python3 .github/skills/2ld2-drive-config/scripts/baseline_sdo.py apply can0
```

脚本执行：`2403h=0` → `2404h=0x16` → `2405h=0x01` → `607Eh=128` → 回读 → `1010h:01` save。
（`2403h` 必须先清空，否则 HOME 功能重复分配会 Err210/211。）

### 5. 断电重启

人工断电、等几秒再上电。重新 `ip link set can0 up ... 500000`（接口会掉）。

### 6. 回读复验

```bash
python3 .github/skills/2ld2-drive-config/scripts/baseline_sdo.py check can0
```

`2403h/2404h/2405h/607Eh` 必须保持 `0 / 0x16 / 0x01 / 128`。

### 7. 配 TPDO / 回零 / 急停 / 抱闸（仓库工具）

```bash
cd firmware/apps/tools/drive_setup && python3 drive_setup.py --save && python3 drive_setup.py --check
```

- 写入 TPDO1（`6041+603F+6061`，10 ms）、TPDO2（`6064+606C`，10 ms）、
  `6098h=21`、`6099h`=2000/500、`609Ah`=2000、`605Ah=2`、`6085h=4000000`、`2437h/2438h=100ms`。
- `--check` 必须全 PASS。
- **`605Ah` 保存后必须再断电重启一次才生效。**

### 8. 按开关复验输入

```bash
python3 .github/skills/2ld2-drive-config/scripts/baseline_sdo.py watch can0
```

60 s 内依次按压：轴1 近端 → 轴1 远端 → 轴2 近端 → 轴2 远端。
期望依次出现 `HOME`（bit2）、`POT`（bit1）、`HOME`、`POT`，释放即复位。

### 9. PP 点动验收（两轴各一次）

```bash
python3 .github/skills/2ld2-drive-config/scripts/baseline_sdo.py jog can0 1 10000
python3 .github/skills/2ld2-drive-config/scripts/baseline_sdo.py jog can0 2 10000
```

期望：位移 = 10000 指令单位（±10，正好 1 圈）；转向 = 面向出轴逆时针（滑台远离电机）；
`6041h` bit10 到位、`603Fh`=0；结束后已断使能、模式回 PP。

### 10. 回零验收（方法 21）

```bash
python3 .github/skills/2ld2-drive-config/scripts/baseline_sdo.py home can0 1
python3 .github/skills/2ld2-drive-config/scripts/baseline_sdo.py home can0 2
```

脚本提示后，在电机运动期间**挡住原点开关**，再松开。期望：挡开关后减速反向、松开处停下，
`6041h` bit12=1、`6064h`=0。（`6040h` bit4 全程保持 1，是电平触发。）

## 验收清单

| # | 项 | 通过标准 |
|---|---|---|
| 1 | 通信 | 两轴心跳 1 Hz，无 BUS-OFF |
| 2 | 识别 | `2023h`=1/2，`2024h`=2，§2.3 读值全对 |
| 3 | 参数 | `2403h`=0、`2404h`=0x16、`2405h`=0x01、`607Eh`=128（断电后保留） |
| 4 | 输入 | 近端 HOME / 远端 POT，按压干净置位 |
| 5 | 使能 | 06→07→0F：`0x0631→0x0633→0x8637` |
| 6 | 点动 | 10000 单位 = 1 圈（±10），约 5 s，bit10 到位 |
| 7 | 方向 | 正方向 = 面向出轴逆时针 |
| 8 | 限位 | 按住时向该方向运动被拒绝，反向正常 |
| 9 | 回零 | 方法 21：bit12=1，`6064h`=0 |
| 10 | TPDO | `drive_setup.py --check` 全 PASS，周期 10 ms |

## 常见问题（事实）

| 现象 | 原因 / 处理 |
|---|---|
| 发帧后 BUS-OFF、无应答 | 总线上没人 ACK：驱动器未上电 / 波特率不是 500k / 接错通道，先 `probe` 再查 |
| `No buffer space available` | 有程序占着接口或总线无 ACK；停掉其它 CAN 程序后重起接口 |
| `6041h` 恒 `0x0400` | 未发 NMT Start；或 48V/STO 缺失 |
| 写 `2404h` 报错 | 忘了先清 `2403h`（HOME 重复分配） |
| 急停后无法再使能 | `605A` 未生效：需 `--save` + 断电重启；先写 `6040=0x0000` 恢复 |
| 写 `1801:03` 被拒（`0x06010002`） | 轴2 禁止时间只读，**不要写** |
| 看不到 TPDO | COB-ID 还是 `0x8000xxxx`：跑 `drive_setup.py` 启用 |
| 上电位置不是上次的值 | 无电池盒、`2015h=0` 增量模式，属基线行为：需回零或主站记忆恢复 |
