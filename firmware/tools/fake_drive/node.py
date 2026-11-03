"""单个 CANopen 从站在总线上的行为（NMT + SDO 加速传输 + 心跳）。

视野：Node 只知道“自己是哪个站号”，一帧进来先看是不是发给自己的；总线上的
其它节点、事件循环、命令行都不属于这里（在 fake_drive.py）。

对象字典的数据归 od.py，本文件只做 CAN 帧的编解码、NMT 状态与心跳时基。
"""

from __future__ import annotations

import time
from collections.abc import Callable

import can

from od import ABORT_UNSUPPORTED, Entry, Od, SdoAbort, load

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

# SDO 加速传输上传响应的命令字：按数据长度选
UPLOAD_CS = {1: 0x4F, 2: 0x4B, 4: 0x43}

# 出厂心跳周期（ms）：0 = 出厂就不发，等主站写 0x1017
DEFAULT_HEARTBEAT_MS = 500


class Node:
    """一个从站节点：自己的对象字典、自己的 NMT 状态。"""

    def __init__(
        self,
        bus: can.BusABC,
        node_id: int,
        trace: bool = False,
        log: Callable[[str], None] | None = None,
        heartbeat_ms: int = DEFAULT_HEARTBEAT_MS,
    ) -> None:
        self.bus = bus
        self.node_id = node_id
        self.trace = trace
        self._log = log or (lambda text: print(text, flush=True))
        self.od = Od(load())
        self.nmt = NMT_PREOP
        # 出厂心跳周期 = 启动参数；0 表示出厂就不发，等主站写 0x1017
        self.default_heartbeat_ms = heartbeat_ms
        self.heartbeat_ms = 0
        self._next_heartbeat: float | None = None
        self.od.write(0x1017, 0, heartbeat_ms, 2)

    # ---- 基础设施 ---------------------------------------------------
    def log(self, text: str) -> None:
        self._log(f"节点 {self.node_id}: {text}")

    def send_cob(self, cob_id: int, data: bytes, pad: bool = True) -> None:
        """发一帧。SDO 响应按惯例补零到 8 字节；心跳/boot-up 是 1 字节（CiA 301）。"""
        payload = bytes(data).ljust(8, b"\x00") if pad else bytes(data)
        self.bus.send(
            can.Message(arbitration_id=cob_id, data=payload, is_extended_id=False)
        )
        if self.trace:
            self.log(f"TX 0x{cob_id:03X} [{len(payload)}] {payload.hex(' ').upper()}")

    def boot_up(self) -> None:
        """初始化完成后的 boot-up 帧：0x700+id，数据 0x00。"""
        self.nmt = NMT_PREOP
        self.send_cob(0x700 + self.node_id, bytes([NMT_BOOTUP]), pad=False)

    # ---- 心跳（生产者，0x1017）--------------------------------------
    def set_heartbeat_period(self, period_ms: int) -> None:
        """0x1017 生效：0 = 停发；>0 = 从此刻起按周期发，内容是当前 NMT 状态。"""
        self.heartbeat_ms = period_ms
        self._next_heartbeat = (
            time.monotonic() + period_ms / 1000.0 if period_ms > 0 else None
        )

    def next_deadline(self) -> float | None:
        """下一次要主动发帧的时刻（monotonic 秒）；没有周期任务返回 None。"""
        return self._next_heartbeat

    def tick(self, now: float) -> None:
        """事件循环每次醒来调一次（不管有没有收到帧）。"""
        if self._next_heartbeat is None or now < self._next_heartbeat:
            return
        self.send_cob(0x700 + self.node_id, bytes([self.nmt]), pad=False)
        # 不追赶欠下的拍数：迟了就迟了，如实反映 USB-CAN 的抖动
        self._next_heartbeat = now + self.heartbeat_ms / 1000.0

    def reset_communication(self) -> None:
        """通讯复位（NMT 0x82）：心跳周期回出厂值并重发 boot-up。其它通讯参数本期未实现。"""
        self.od.write(0x1017, 0, self.default_heartbeat_ms, 2)
        self.heartbeat_ms = 0
        self._next_heartbeat = None
        self.boot_up()
        self.start_heartbeat()

    def start_heartbeat(self) -> None:
        """boot-up 之后调用：按出厂周期开始发心跳（周期 0 则不发）。"""
        self.set_heartbeat_period(self.default_heartbeat_ms)

    # ---- 收帧：只处理“发给自己的” -----------------------------------
    def on_frame(self, msg: can.Message) -> None:
        aid = msg.arbitration_id
        if aid == 0x000:
            self.on_nmt(msg)
        elif aid == 0x600 + self.node_id:
            self.on_sdo(msg)

    # ---- NMT --------------------------------------------------------
    def on_nmt(self, msg: can.Message) -> None:
        if len(msg.data) < 2:
            return
        cmd, target = msg.data[0], msg.data[1]
        if target not in (0, self.node_id):
            return  # 广播或指名发给本节点以外的站号
        self.apply_nmt(cmd)

    def apply_nmt(self, cmd: int) -> None:
        entry = NMT_COMMANDS.get(cmd)
        if entry is None:
            self.log(f"NMT 未知命令 0x{cmd:02X}（忽略）")
            return
        new_state, name, reinit = entry
        self.nmt = new_state
        self.log(f"NMT {name} → {STATE_NAME[new_state]}")
        if reinit:
            self.reset_communication()

    # ---- SDO server -------------------------------------------------
    def on_sdo(self, msg: can.Message) -> None:
        if self.nmt == NMT_STOPPED:
            self.log("处于 Stopped，不应答 SDO（DS301）")
            return
        d = bytes(msg.data)
        if len(d) < 8:
            return  # 短的 SDO 请求不是合法帧
        cs = d[0] >> 5
        index = int.from_bytes(d[1:3], "little")
        sub = d[3]
        if cs == 2:  # initiate upload（读）
            self.sdo_upload(index, sub)
        elif cs == 1:  # initiate download（写）
            self.sdo_download(index, sub, d)
        elif cs == 4:  # abort transfer
            self.log(f"收到 SDO abort 0x{index:04X}:{sub:02X}")
        else:
            self.sdo_abort(index, sub, ABORT_UNSUPPORTED)

    def sdo_upload(self, index: int, sub: int) -> None:
        try:
            value, entry = self.od.read(index, sub)
        except SdoAbort as exc:
            self.sdo_abort(index, sub, exc.code)
            return
        data = (
            bytes([UPLOAD_CS[entry.size]])
            + index.to_bytes(2, "little")
            + bytes([sub])
            + value.to_bytes(entry.size, "little")
        )
        self.send_cob(0x580 + self.node_id, data)
        self.log(
            f"SDO 读 0x{index:04X}:{sub:02X} = "
            f"0x{value:0{entry.size * 2}X}  （{entry.name}）"
        )

    def sdo_download(self, index: int, sub: int, d: bytes) -> None:
        expedited = bool(d[0] & 0x02)
        size_valid = bool(d[0] & 0x01)
        if not expedited or not size_valid:
            self.sdo_abort(index, sub, ABORT_UNSUPPORTED)  # 暂不支持分段
            return
        size = 4 - ((d[0] >> 2) & 0x03)
        try:
            entry = self.od.write(index, sub, int.from_bytes(d[4 : 4 + size], "little"), size)
        except SdoAbort as exc:
            self.sdo_abort(index, sub, exc.code)
            return
        self.send_cob(
            0x580 + self.node_id,
            bytes([0x60]) + index.to_bytes(2, "little") + bytes([sub]),
        )
        self.log(
            f"SDO 写 0x{index:04X}:{sub:02X} = "
            f"0x{entry.value:0{entry.size * 2}X}  （{entry.name}）"
        )
        self.after_write(index, entry)

    def after_write(self, index: int, entry: Entry) -> None:
        """写入生效动作：需要驱动自身行为的对象在这里接线。"""
        if index == 0x1017:
            self.set_heartbeat_period(entry.value)
            self.log(
                "心跳" + (f"周期改为 {entry.value} ms" if entry.value else "已停发")
            )

    def sdo_abort(self, index: int, sub: int, code: int) -> None:
        self.send_cob(
            0x580 + self.node_id,
            bytes([0x80])
            + index.to_bytes(2, "little")
            + bytes([sub])
            + code.to_bytes(4, "little"),
        )
        self.log(f"SDO abort 0x{index:04X}:{sub:02X} → 0x{code:08X}")
