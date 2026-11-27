#!/usr/bin/env python3
"""2LD2 双轴驱动器的持久配置工具（TPDO 反馈 + 回零 + 急停）。

配置内容：
  TPDO1  1A00 = 6041:16 + 603F:16 + 6061:8    1800:05 = 10 ms
  TPDO2  1A01 = 6064:32 + 606C:32             1801:05 = 10 ms
  回零    6098 = 21、6099:01 = 100000（50 mm/s）、6099:02 = 4000（2 mm/s）、609A = 1000000（500 mm/s²）
  急停    605A = 2（按 6085 减速后自动断使能并抱闸）、6085 = 2000 mm/s²、
          2437/2438 = 100 ms（抱闸延时打开 / 等抱闸完全打开）
TPDO3/TPDO4 保持出厂禁用，禁止时间（1800:03/1801:03）不写：
节点 1 可写、节点 2 只读，写它会让两轴行为不一致。

写参数前会先写 6040 = 0x0000 断使能（使能态下参数可能被拒写），结束时也
保持断使能，不自动使能。

驱动器出厂时四路 TPDO 全部禁用（COB-ID bit31 = 1），必须由本工具清 bit31 才会发送。
默认不写 1010h：驱动器掉电后配置回到出厂，需要重新执行；--save 才保存到驱动器。

本工具只负责配置与校验，不参与固件运行。主站启动前必须已经执行过；
固件不发这些写请求，回零时只切模式、发启动。

Author: zager
"""

from __future__ import annotations

import argparse
import os
import statistics
import sys
import time

import can

SDO_TIMEOUT_S = 0.2
SDO_RETRIES = 3
SAVE_SIGNATURE = 0x65766173  # "save"

# TPDO1 / TPDO2 的目标映射（每项为 索引:子索引:位长）
MAP_TPDO1 = [0x60410010, 0x603F0010, 0x60610008]
MAP_TPDO2 = [0x60640020, 0x606C0020]

EVENT_TIMER_MS = 10

# 回零参数：方法与速度由驱动器执行，属于驱动器持久配置。
# 速度单位是指令单位/s，1 圈 = 10000；方法 21 = 原点开关 + 不用 Z 信号 + 负向起始。
# 2000 = 0.2 圈/s（台架实测可用，实机按行程重算）。
HOMING_PARAMS = [
    ("6098h 回零方法", 0x6098, 0, 21, 1),
    ("6099:01 搜索速度", 0x6099, 1, 100000, 4),
    ("6099:02 一级速度", 0x6099, 2, 4000, 4),
    ("609Ah 回零加减速", 0x609A, 0, 1000000, 4),
]

# 急停与抱闸：急停后由驱动器自己减速、自己断使能、抱闸自动抱住。
# 605A = 2：按 6085 快停减速度停车，然后进 Switch on disabled（断使能）。
# 6085 = 4,000,000 指令单位/s² = 2000 mm/s²（1 mm = 2000）。
# 2437 / 2438：抱闸延时打开 / 等抱闸完全打开，单位 ms。手册写的范围是
# 0~10000，但这两个对象在本机 OD 里是 32 位，用 2 字节写会被拒（0x08000000）。
SAFETY_PARAMS = [
    ("605Ah 快停选项码", 0x605A, 0, 2, 2),
    ("6085h 快停减速度", 0x6085, 0, 4_000_000, 4),
    ("2437h 抱闸延时打开", 0x2437, 0, 100, 4),
    ("2438h 抱闸解除等待", 0x2438, 0, 100, 4),
]

# 6041 里 bit0..3 = 0、bit6 = 1 即 Switch on disabled
STATUS_MASK = 0x4F
STATUS_SWITCH_ON_DISABLED = 0x40

CMD_READ = 0x40
CMD_WRITE_1 = 0x2F
CMD_WRITE_2 = 0x2B
CMD_WRITE_4 = 0x23
CMD_WRITE_ACK = 0x60
CMD_ABORT = 0x80
RESP_SIZE = {0x43: 4, 0x4B: 2, 0x4F: 1}

ABORT_TEXT = {
    0x06010000: "不支持对该对象的访问",
    0x06010001: "试图读只写对象",
    0x06010002: "试图写只读对象",
    0x06020000: "对象字典中不存在该对象",
    0x06040041: "对象不能映射到 PDO",
    0x06040042: "映射长度超出 PDO 容量",
    0x06090011: "子索引不存在",
}


class SdoError(Exception):
    """SDO 事务失败或从站返回中止。"""


class SdoClient:
    """一台从站的加速 SDO 客户端。本工具只用到 1/2/4 字节的加速读写。"""

    def __init__(self, bus: can.BusABC, node_id: int) -> None:
        self.bus = bus
        self.node_id = node_id
        self.tx_id = 0x600 + node_id
        self.rx_id = 0x580 + node_id

    def _transact(self, request: bytes) -> bytes:
        for _ in range(SDO_RETRIES):
            self.bus.send(
                can.Message(arbitration_id=self.tx_id, data=request, is_extended_id=False)
            )
            deadline = time.monotonic() + SDO_TIMEOUT_S
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0.0:
                    break
                message = self.bus.recv(remaining)
                if message is None or message.arbitration_id != self.rx_id:
                    continue
                return bytes(message.data)
        raise SdoError(f"节点 {self.node_id} 无应答")

    def read(self, index: int, sub: int = 0) -> int:
        response = self._transact(
            bytes([CMD_READ, index & 0xFF, index >> 8, sub, 0, 0, 0, 0])
        )
        self._raise_on_abort(response, index, sub)
        size = RESP_SIZE.get(response[0])
        if size is None:
            raise SdoError(f"节点 {self.node_id} 读到分段应答，本工具不支持")
        return int.from_bytes(response[4 : 4 + size], "little")

    def write(self, index: int, sub: int, value: int, size: int) -> None:
        command = {1: CMD_WRITE_1, 2: CMD_WRITE_2, 4: CMD_WRITE_4}[size]
        payload = value.to_bytes(size, "little").ljust(4, b"\x00")
        response = self._transact(
            bytes([command, index & 0xFF, index >> 8, sub]) + payload
        )
        self._raise_on_abort(response, index, sub)
        if response[0] != CMD_WRITE_ACK:
            raise SdoError(
                f"节点 {self.node_id} 写 0x{index:04X}:{sub:02X} 应答异常 0x{response[0]:02X}"
            )

    def _raise_on_abort(self, response: bytes, index: int, sub: int) -> None:
        if response[0] != CMD_ABORT:
            return
        code = int.from_bytes(response[4:8], "little")
        text = ABORT_TEXT.get(code, "未收录的中止码")
        raise SdoError(
            f"节点 {self.node_id} 访问 0x{index:04X}:{sub:02X} 中止 "
            f"0x{code:08X}（{text}）"
        )

    def nmt(self, command: int, target: int) -> None:
        self.bus.send(
            can.Message(
                arbitration_id=0x000,
                data=bytes([command, target]),
                is_extended_id=False,
            )
        )


def mapping_matches(client: SdoClient, index: int, entries: list[int]) -> bool:
    """映射与目标一致时返回 True。只比较有效条目，跳过未使用的尾部。"""
    if client.read(index, 0) != len(entries):
        return False
    return all(client.read(index, i) == entry for i, entry in enumerate(entries, 1))


def apply_mapping(client: SdoClient, index: int, entries: list[int]) -> bool:
    """按 CiA 301 的四步法写映射；已是目标值时不动。返回是否发生写入。"""
    if mapping_matches(client, index, entries):
        return False
    client.write(index, 0, 0, 1)
    for sub, entry in enumerate(entries, 1):
        client.write(index, sub, entry, 4)
    client.write(index, 0, len(entries), 1)
    return True


def apply_cob_id(client: SdoClient, index: int, target: int) -> bool:
    """写 COB-ID。bit31 由 1 变 0 即启用该 TPDO。"""
    current = client.read(index, 1)
    if current == target:
        return False
    if current & 0x80000000 == 0:
        client.write(index, 1, current | 0x80000000, 4)
    client.write(index, 1, target, 4)
    return True


def apply_homing(client: SdoClient) -> bool:
    """写回零参数；已是目标值时不动。返回是否发生写入。"""
    changed = False
    for _, index, sub, value, size in HOMING_PARAMS:
        if client.read(index, sub) != value:
            client.write(index, sub, value, size)
            changed = True
    return changed


def apply_safety(client: SdoClient) -> bool:
    """写急停与抱闸参数；已是目标值时不动。返回是否发生写入。"""
    changed = False
    for _, index, sub, value, size in SAFETY_PARAMS:
        if client.read(index, sub) != value:
            client.write(index, sub, value, size)
            changed = True
    return changed


def ensure_disabled(client: SdoClient) -> None:
    """写参数前先断使能：使能态下参数可能被驱动器拒写。

    Not ready（刚上电未 NMT Start）状态下写 6040 无效，先切 Operational 让状态机就绪。
    """
    if client.read(0x6041) & STATUS_MASK == 0:
        client.nmt(0x01, client.node_id)
        time.sleep(0.1)
    client.write(0x6040, 0, 0x0000, 2)
    for _ in range(10):
        if client.read(0x6041) & STATUS_MASK == STATUS_SWITCH_ON_DISABLED:
            return
        time.sleep(0.03)
    raise SdoError(f"节点 {client.node_id} 断使能后未进入 Switch on disabled")


def apply_node(client: SdoClient, node_id: int) -> None:
    """配置单个从站。配置期间节点处于 Pre-operational，且保持断使能。"""
    ensure_disabled(client)
    client.nmt(0x00, node_id)  # Pre-operational：映射只能在配置态写
    time.sleep(0.05)

    # 映射在 PDO 启用时只读——先置 bit31 禁用，写完映射再由 apply_cob_id 启用。
    for index in (0x1800, 0x1801):
        current = client.read(index, 1)
        if current & 0x80000000 == 0:
            client.write(index, 1, current | 0x80000000, 4)

    changed = apply_mapping(client, 0x1A00, MAP_TPDO1)
    changed |= apply_mapping(client, 0x1A01, MAP_TPDO2)
    changed |= apply_cob_id(client, 0x1800, 0x180 + node_id)
    changed |= apply_cob_id(client, 0x1801, 0x280 + node_id)
    for index in (0x1800, 0x1801):
        if client.read(index, 2) != 0xFF:
            client.write(index, 2, 0xFF, 1)  # 传输类型 255：事件定时器独立生效
            changed = True
        if client.read(index, 5) != EVENT_TIMER_MS:
            client.write(index, 5, EVENT_TIMER_MS, 2)
            changed = True

    changed |= apply_homing(client)
    changed |= apply_safety(client)

    client.nmt(0x01, node_id)  # Operational：TPDO 只在运行态发送
    print(f"  节点 {node_id}: {'已写入目标配置' if changed else '已是目标配置，未改动'}")


def verify_node(client: SdoClient, node_id: int) -> bool:
    """回读全部目标值，逐项比对。"""
    checks = [
        ("1A00 条目数", client.read(0x1A00, 0), len(MAP_TPDO1)),
        ("1800 COB-ID", client.read(0x1800, 1), 0x180 + node_id),
        ("1800 事件定时器", client.read(0x1800, 5), EVENT_TIMER_MS),
        ("1A01 条目数", client.read(0x1A01, 0), len(MAP_TPDO2)),
        ("1801 COB-ID", client.read(0x1801, 1), 0x280 + node_id),
        ("1801 事件定时器", client.read(0x1801, 5), EVENT_TIMER_MS),
    ]
    for i, entry in enumerate(MAP_TPDO1, 1):
        checks.append((f"1A00:{i:02X} 映射", client.read(0x1A00, i), entry))
    for i, entry in enumerate(MAP_TPDO2, 1):
        checks.append((f"1A01:{i:02X} 映射", client.read(0x1A01, i), entry))
    for name, index, sub, value, _ in HOMING_PARAMS:
        checks.append((name, client.read(index, sub), value))
    for name, index, sub, value, _ in SAFETY_PARAMS:
        checks.append((name, client.read(index, sub), value))

    ok = True
    for name, actual, expected in checks:
        mark = "ok  " if actual == expected else "错  "
        if actual != expected:
            ok = False
        print(f"    {mark}{name:<16} 实测 0x{actual:08X}  期望 0x{expected:08X}")
    return ok


def observe(bus: can.BusABC, node_id: int, seconds: float) -> bool:
    """被动听取 TPDO，给出每帧数量与周期。"""
    wanted = {0x180 + node_id: [], 0x280 + node_id: []}
    deadline = time.monotonic() + seconds
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            break
        message = bus.recv(remaining)
        if message is not None and message.arbitration_id in wanted:
            wanted[message.arbitration_id].append((time.monotonic(), bytes(message.data)))

    ok = True
    for cob_id, received in wanted.items():
        if len(received) < 2:
            print(f"    错  0x{cob_id:03X}  {len(received)} 帧，未达到周期上报")
            ok = False
            continue
        gaps = [
            (received[i][0] - received[i - 1][0]) * 1000.0
            for i in range(1, len(received))
        ]
        payload = received[-1][1].hex(" ").upper()
        print(
            f"    ok  0x{cob_id:03X}  {len(received):3d} 帧  "
            f"周期 中位 {statistics.median(gaps):5.1f} ms "
            f"[{min(gaps):5.1f} ~ {max(gaps):5.1f}]  帧长 {len(received[-1][1])}B  数据 {payload}"
        )
    return ok


def busy_masters() -> list[str]:
    """找出可能正在占用总线的程序，避免配置过程中打断运行中的主站。"""
    names = ("cats_qt", "master_probe", "fake_drive")
    found = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        try:
            with open(f"/proc/{entry}/cmdline", "rb") as handle:
                command = handle.read().decode("utf-8", "replace")
        except OSError:
            continue
        if any(name in command for name in names):
            found.append(f"{entry}: {command.replace(chr(0), ' ').strip()}")
    return found


def main() -> int:
    parser = argparse.ArgumentParser(
        description="配置 2LD2 从站的 TPDO 反馈通道并校验",
    )
    parser.add_argument("--iface", default="can0", help="SocketCAN 接口名（默认 can0）")
    parser.add_argument(
        "--nodes", type=int, nargs="+", default=[1, 2], help="从站站号（默认 1 2）"
    )
    parser.add_argument("--save", action="store_true", help="写 1010:01 保存到驱动器")
    parser.add_argument(
        "--check",
        action="store_true",
        help="只回读校验，不写任何东西（主站启动前自检用）",
    )
    parser.add_argument(
        "--duration", type=float, default=1.5, help="听取 TPDO 的时长（默认 1.5 s）"
    )
    parser.add_argument("--force", action="store_true", help="忽略总线占用检查")
    args = parser.parse_args()

    running = busy_masters()
    if running and not args.force:
        print("检测到可能占用总线的程序，配置会打断它们：")
        for line in running:
            print(f"  {line}")
        print("确认已停止后重试，或加 --force")
        return 2

    try:
        bus = can.Bus(interface="socketcan", channel=args.iface)
    except OSError as error:
        print(f"打开 {args.iface} 失败：{error}")
        return 2

    result = 0
    try:
        for node_id in args.nodes:
            print(f"[节点 {node_id}]")
            client = SdoClient(bus, node_id)
            try:
                if args.check:
                    print("  只读校验：")
                    if not verify_node(client, node_id):
                        result = 1
                    # 断电重启后节点停在 Pre-operational，TPDO 只在运行态发送；
                    # 这里只切 NMT 状态，不写任何配置。
                    client.nmt(0x01, node_id)
                    time.sleep(0.05)
                else:
                    apply_node(client, node_id)
                    print("  回读校验：")
                    if not verify_node(client, node_id):
                        result = 1
                    if args.save:
                        client.write(0x1010, 1, SAVE_SIGNATURE, 4)
                        print("  已写 1010:01 保存")
                print(f"  听取 {args.duration:.1f} s：")
                if not observe(bus, node_id, args.duration):
                    result = 1
            except SdoError as error:
                print(f"  失败：{error}")
                result = 1
    finally:
        bus.shutdown()

    return result


if __name__ == "__main__":
    sys.exit(main())
