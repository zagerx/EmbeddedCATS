#!/usr/bin/env python3
"""CANopen 从站模拟器（假的伺服驱动器）。

本机只有 PCAN-USB Pro FD（两个通道），没有真实驱动器。把两口对连：

    can0 = 主站口（CATS 跑这一侧）
    can1 = 从站口（本程序跑这一侧）

当前范围（刻意保持极小）：
  - NMT（0x000）：start / stop / enter pre-operational / reset node / reset communication
  - SDO 加速传输（0x600+id 请求 → 0x580+id 响应）：读对象字典；写仅对 rw 条目开放

不做的：PDO、SYNC、心跳、EMCY、分段 SDO、CiA 402 状态机与运动模型。

对象字典里的数值是 CiA 301/402 的**标准占位值**，不是 2LD2-CAN7020B 的实测值；
真机到货后用 tools/od_scan 扫一遍再覆盖，避免拿主站的期望当事实。
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass

import can

RO = "ro"
RW = "rw"
WO = "wo"

# ---- SDO abort 码（DS 301 7.2.4.2）-----------------------------------
ABORT_UNSUPPORTED = 0x06010000  # unsupported access to an object
ABORT_READ_ONLY = 0x06010002    # attempt to write a read only object
ABORT_NO_OBJECT = 0x06020000    # object does not exist in the object dictionary
ABORT_LENGTH = 0x06070010       # data type / length does not match
ABORT_NO_SUBINDEX = 0x06090011  # sub-index does not exist

# ---- NMT 状态（DS 301 7.3.2）-----------------------------------------
NMT_BOOTUP = 0x00
NMT_STOPPED = 0x04
NMT_OPERATIONAL = 0x05
NMT_PREOP = 0x7F

STATE_NAME = {
    NMT_BOOTUP: "Boot-up",
    NMT_STOPPED: "Stopped",
    NMT_OPERATIONAL: "Operational",
    NMT_PREOP: "Pre-operational",
}

# NMT 命令 → (目标状态, 名字, 是否重新初始化)
NMT_COMMANDS = {
    0x01: (NMT_OPERATIONAL, "Start remote node", False),
    0x02: (NMT_STOPPED, "Stop remote node", False),
    0x80: (NMT_PREOP, "Enter pre-operational", False),
    0x81: (NMT_PREOP, "Reset node", True),
    0x82: (NMT_PREOP, "Reset communication", True),
}


@dataclass
class Entry:
    """对象字典条目。size = 1/2/4 字节。"""

    value: int
    size: int
    access: str
    name: str

    @property
    def mask(self) -> int:
        return (1 << (8 * self.size)) - 1


# ---- 对象字典（占位值，真机以 od_scan 结果为准）----------------------
OD: dict[tuple[int, int], Entry] = {
    (0x1000, 0x00): Entry(0x00020192, 4, RO, "Device type（CiA 402 伺服驱动）"),
    (0x1001, 0x00): Entry(0x00, 1, RO, "Error register"),
    (0x1005, 0x00): Entry(0x00000080, 4, RW, "COB-ID SYNC"),
    (0x1006, 0x00): Entry(0x00000000, 4, RW, "Communication cycle period"),
    (0x1017, 0x00): Entry(0x0000, 2, RW, "Producer heartbeat time"),
    (0x1018, 0x01): Entry(0x00000000, 4, RO, "Identity: Vendor-ID"),
    (0x1018, 0x02): Entry(0x00000000, 4, RO, "Identity: Product code"),
    (0x1018, 0x03): Entry(0x00000000, 4, RO, "Identity: Revision number"),
    (0x1018, 0x04): Entry(0x00000000, 4, RO, "Identity: Serial number"),
}

# SDO 加速传输上传响应的命令字：按数据长度选
UPLOAD_CS = {1: 0x4F, 2: 0x4B, 4: 0x43}


class FakeDrive:
    def __init__(self, bus: can.BusABC, node_ids: list[int], trace: bool = False) -> None:
        self.bus = bus
        self.node_ids = node_ids
        self.trace = trace
        self.state = {nid: NMT_PREOP for nid in node_ids}

    # ---- 基础设施 ---------------------------------------------------
    def log(self, text: str) -> None:
        print(text, flush=True)

    def send_cob(self, cob_id: int, data: bytes) -> None:
        payload = bytes(data).ljust(8, b"\x00")
        self.bus.send(
            can.Message(arbitration_id=cob_id, data=payload, is_extended_id=False)
        )
        if self.trace:
            self.log(f"TX 0x{cob_id:03X} [{len(payload)}] {payload.hex(' ').upper()}")

    def boot_up(self, nid: int) -> None:
        """初始化完成后的 boot-up 帧：0x700+id，数据 0x00。"""
        self.state[nid] = NMT_PREOP
        self.send_cob(0x700 + nid, bytes([NMT_BOOTUP]))

    # ---- 收帧分发 ---------------------------------------------------
    def handle(self, msg: can.Message) -> None:
        if msg.is_error_frame or msg.is_extended_id:
            return  # CANopen 只用 11 位标准帧
        if not msg.is_rx:
            return  # 忽略 socketcan 回环出来的自己发的帧

        aid = msg.arbitration_id
        if self.trace:
            self.log(f"RX 0x{aid:03X} [{msg.dlc}] {bytes(msg.data).hex(' ').upper()}")

        if aid == 0x000:
            self.handle_nmt(msg)
            return

        for nid in self.node_ids:
            if aid == 0x600 + nid:
                if self.state[nid] == NMT_STOPPED:
                    self.log(f"节点 {nid}: 处于 Stopped，不应答 SDO（DS301）")
                    return
                self.handle_sdo(nid, msg)
                return

    # ---- NMT --------------------------------------------------------
    def handle_nmt(self, msg: can.Message) -> None:
        if len(msg.data) < 2:
            return
        cmd, target = msg.data[0], msg.data[1]
        if target != 0 and target not in self.node_ids:
            return  # 不是发给我们的
        for nid in self.node_ids if target == 0 else [target]:
            self.apply_nmt(nid, cmd)

    def apply_nmt(self, nid: int, cmd: int) -> None:
        entry = NMT_COMMANDS.get(cmd)
        if entry is None:
            self.log(f"节点 {nid}: NMT 未知命令 0x{cmd:02X}（忽略）")
            return
        new_state, name, reinit = entry
        self.state[nid] = new_state
        self.log(f"节点 {nid}: NMT {name} → {STATE_NAME[new_state]}")
        if reinit:
            self.boot_up(nid)

    # ---- SDO server -------------------------------------------------
    def handle_sdo(self, nid: int, msg: can.Message) -> None:
        d = bytes(msg.data)
        if len(d) < 8:
            return  # 短的 SDO 请求不是合法帧
        cs = d[0] >> 5
        index = int.from_bytes(d[1:3], "little")
        sub = d[3]
        if cs == 2:  # initiate upload（读）
            self.sdo_upload(nid, index, sub)
        elif cs == 1:  # initiate download（写）
            self.sdo_download(nid, index, sub, d)
        elif cs == 4:  # abort transfer
            self.log(f"节点 {nid}: 收到 SDO abort 0x{index:04X}:{sub:02X}")
        else:
            self.sdo_abort(nid, index, sub, ABORT_UNSUPPORTED)

    def lookup(self, nid: int, index: int, sub: int) -> Entry | None:
        entry = OD.get((index, sub))
        if entry is not None:
            return entry
        if any(idx == index for idx, _ in OD):
            self.sdo_abort(nid, index, sub, ABORT_NO_SUBINDEX)
        else:
            self.sdo_abort(nid, index, sub, ABORT_NO_OBJECT)
        return None

    def sdo_upload(self, nid: int, index: int, sub: int) -> None:
        entry = self.lookup(nid, index, sub)
        if entry is None:
            return
        if entry.access == WO:
            self.sdo_abort(nid, index, sub, ABORT_UNSUPPORTED)
            return
        value = entry.value & entry.mask
        data = (
            bytes([UPLOAD_CS[entry.size]])
            + index.to_bytes(2, "little")
            + bytes([sub])
            + value.to_bytes(entry.size, "little")
        )
        self.send_cob(0x580 + nid, data)
        self.log(
            f"节点 {nid}: SDO 读 0x{index:04X}:{sub:02X} = "
            f"0x{value:0{entry.size * 2}X}  （{entry.name}）"
        )

    def sdo_download(self, nid: int, index: int, sub: int, d: bytes) -> None:
        entry = self.lookup(nid, index, sub)
        if entry is None:
            return
        if entry.access == RO:
            self.sdo_abort(nid, index, sub, ABORT_READ_ONLY)
            return
        expedited = bool(d[0] & 0x02)
        size_valid = bool(d[0] & 0x01)
        if not expedited or not size_valid:
            self.sdo_abort(nid, index, sub, ABORT_UNSUPPORTED)  # 暂不支持分段
            return
        size = 4 - ((d[0] >> 2) & 0x03)
        if size != entry.size:
            self.sdo_abort(nid, index, sub, ABORT_LENGTH)
            return
        entry.value = int.from_bytes(d[4 : 4 + size], "little")
        self.send_cob(
            0x580 + nid,
            bytes([0x60]) + index.to_bytes(2, "little") + bytes([sub]),
        )
        self.log(
            f"节点 {nid}: SDO 写 0x{index:04X}:{sub:02X} = "
            f"0x{entry.value:0{entry.size * 2}X}  （{entry.name}）"
        )

    def sdo_abort(self, nid: int, index: int, sub: int, code: int) -> None:
        self.send_cob(
            0x580 + nid,
            bytes([0x80])
            + index.to_bytes(2, "little")
            + bytes([sub])
            + code.to_bytes(4, "little"),
        )
        self.log(f"节点 {nid}: SDO abort 0x{index:04X}:{sub:02X} → 0x{code:08X}")


def parse_nodes(text: str) -> list[int]:
    nodes = [int(x, 0) for x in text.replace(" ", "").split(",") if x]
    for nid in nodes:
        if not 1 <= nid <= 127:
            raise argparse.ArgumentTypeError(f"站号必须在 1..127：{nid}")
    return nodes


def main() -> int:
    ap = argparse.ArgumentParser(
        description="CANopen 从站模拟器（当前只支持 NMT + SDO 加速读）"
    )
    ap.add_argument("-i", "--interface", default="can1", help="CAN 通道（默认 can1）")
    ap.add_argument(
        "-n", "--nodes", default="1,2", help="模拟的站号，逗号分隔（默认 1,2）"
    )
    ap.add_argument("-t", "--trace", action="store_true", help="打印每个收发的原始帧")
    args = ap.parse_args()

    node_ids = parse_nodes(args.nodes)
    bus = can.Bus(interface="socketcan", channel=args.interface)

    drive = FakeDrive(bus, node_ids, trace=args.trace)
    drive.log(f"fake_drive 已启动：{args.interface}，站号 {node_ids}，波特率由接口决定")
    for nid in node_ids:
        drive.log(
            f"  节点 {nid}: NMT 0x000 | SDO 0x{0x600 + nid:03X} → 0x{0x580 + nid:03X} | "
            f"心跳 0x{0x700 + nid:03X}"
        )
        drive.boot_up(nid)  # 上电即发 boot-up

    try:
        while True:
            msg = bus.recv(timeout=0.5)
            if msg is not None:
                drive.handle(msg)
    except KeyboardInterrupt:
        drive.log("收到 Ctrl-C，退出")
    finally:
        bus.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
