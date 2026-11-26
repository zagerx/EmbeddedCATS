---
name: 2ld2-drive-config
description: '只配置 2LD2-CAN7020B 驱动器本体的持久参数，使其与台架基准一致并回读验收；不依赖电机、滑台、限位开关，不做点动/回零。Use when: 新驱动器上机前预配置、更换驱动器、驱动器恢复出厂、在另一台电脑复刻配置、纯参数核对。步骤：can0 500k 起链路 → 心跳探活 → 只读核对 → 写输入分配/方向 → save → 断电重启 → 回读 → drive_setup 配 TPDO/回零/急停/抱闸参数 → 复验。基准值见 docs/02_驱动器说明/2LD2-CAN7020B驱动器配置基准.md。铁律：写 2403h 必须先于 2404h；所有写入后必须 save；必须断电重启后复验；改拨码后必须断电重启；不改 180x:03 禁止时间；605Ah 保存后必须重启才生效。'
argument-hint: '<CAN 接口名，默认 can0>'
---

# 2LD2-CAN7020B 驱动器配置（纯参数版）

## 何时使用

- 新驱动器上机前，先把它配置成与台架基准一致（此时可以没接电机、没接机械）。
- 更换驱动器 / 驱动器恢复出厂后重配。
- 核对某台驱动器的持久参数是否符合基准。

## 前提

- Linux + SocketCAN + PEAK PCAN-USB（FD）适配器；`python-can` 已安装（`pip install python-can`）。
- **驱动器本体**上电：24V 控制电即可通 CAN；48V 主电 + STO 24V 短接仅在点动/整机验收时需要；`SW6=on`；
  站号旋码 1/2；波特率拨码 500k。
- **没有其他程序占用 CAN**（`cats_qt` / `master_probe` / `drive_setup` 同时只能跑一个）。
- 接口操作需要 root（sudo 由人工执行）。

本 skill 只写**驱动器侧持久参数**；不要求电机/滑台/限位开关连接，不做点动与回零。
所有数字以 `docs/02_驱动器说明/2LD2-CAN7020B驱动器配置基准.md` 为准，不要凭记忆写值。

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

身份值必须全部 PASS：`1000h`=0x00020192、`6502h`=0x3002D、`608Fh`=131072/1、
`6092h:01`=10000、`2023h`=1/2、`2024h`=2。
`6079h`≈47~48V 仅在 48V 主电已上时有意义（只有 24V 时该项会 FAIL，可忽略）。
（参数项此时可能还不是基准值，下一步会写。）

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

必须全部 PASS：`2403h`=0、`2404h`=0x16、`2405h`=0x01、`607Eh`=128（断电后保留）。

### 7. 配 TPDO / 回零 / 急停 / 抱闸（仓库工具）

```bash
cd firmware/apps/tools/drive_setup && python3 drive_setup.py --save && python3 drive_setup.py --check
```

- 写入 TPDO1（`6041+603F+6061`，10 ms）、TPDO2（`6064+606C`，10 ms）、
  `6098h=21`、`6099h`=2000/500、`609Ah`=2000、`605Ah=2`、`6085h=4000000`、`2437h/2438h=100ms`。
- `--check` 必须全 PASS。
- **`605Ah` 保存后必须再断电重启一次才生效。**

### 8. 最终核对

断电重启后重跑 `check`（步骤 6）与 `drive_setup.py --check`。两者全 PASS 即完成配置。

## 验收清单（参数级）

| # | 项 | 通过标准 |
|---|---|---|
| 1 | 通信 | 两轴心跳 1 Hz，无 BUS-OFF |
| 2 | 识别 | `2023h`=1/2，`2024h`=2，识别读值全对 |
| 3 | 输入分配 | `2403h`=0、`2404h`=0x16、`2405h`=0x01（断电后保留） |
| 4 | 方向 | `607Eh`=128（断电后保留） |
| 5 | TPDO/回零/急停/抱闸 | `drive_setup.py --check` 全 PASS |

## 可选：整机验收（装好电机/滑台/限位开关后再做）

以下依赖机械与开关，不属于“只配驱动器”的范围：

- 按开关：近端 → `60FD` bit2（HOME）；远端 → `60FD` bit1（POT）（工具：`baseline_sdo.py watch can0`）
- PP 点动：`baseline_sdo.py jog can0 <node> 10000` → 位移 10000 单位（1 圈，±10），
  转向 = 面向出轴逆时针
- 回零：`baseline_sdo.py home can0 <node>` → 方法 21，`6041h` bit12=1、`6064h`=0
- 限位：按住时向该方向运动被拒绝（位移≈0），反向正常

## 常见问题（事实）

| 现象 | 原因 / 处理 |
|---|---|
| 发帧后 BUS-OFF、无应答 | 总线上没人 ACK：驱动器未上电 / 波特率不是 500k / 接错通道，先 `probe` 再查 |
| `No buffer space available` | 有程序占着接口或总线无 ACK；停掉其它 CAN 程序后重起接口 |
| `6041h` 恒 `0x0400` | 未发 NMT Start（`drive_setup --check` 会自己切 Operational）；或 48V/STO 缺失 |
| 写 `2404h` 报错 | 忘了先清 `2403h`（HOME 重复分配） |
| 写 `1801:03` 被拒（`0x06010002`） | 轴2 禁止时间只读，**不要写** |
| 看不到 TPDO | COB-ID 还是 `0x8000xxxx`：跑 `drive_setup.py` 启用 |
| `605Ah` 改了不生效 | 必须 `--save` + 断电重启 |
