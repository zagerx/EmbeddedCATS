"""单个 CANopen 从站：NMT、加速 SDO、心跳和最小 402/PP 行为。"""

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

# ---- CiA 402：状态字取值（手册 5.3 / DS 402）--------------------------
# 本模拟器实现“使能”与“PP 最小运动模型”：0x40 → 0x21 → 0x23 → 0x27，
# 写 607A + 6040 bit4 上升沿 ⇒ 按 6081 匀速走到目标（加减速未模拟）。
CIA402_SWITCH_ON_DISABLED = 0x0040
CIA402_READY = 0x0021
CIA402_SWITCHED_ON = 0x0023
CIA402_OP_ENABLED = 0x0027
CIA402_FAULT = 0x0008

SW_TARGET_REACHED = 0x0400  # bit10：已到位
SW_SETPOINT_ACK = 0x1000  # bit12：新目标已接收（PP 握手）


class Node:
    """一个从站节点：自己的对象字典、自己的 NMT 状态。"""

    def __init__(
        self,
        bus: can.BusABC,
        node_id: int,
        trace: bool = False,
        log: Callable[[str], None] | None = None,
        heartbeat_ms: int = DEFAULT_HEARTBEAT_MS,
        follow_402: bool = True,
        echo_mode: bool = True,
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
        # 402 状态机：上电即“禁止启动”，状态字随时同步进 6041
        # follow_402 = False 时状态字不跟控制字走（复现“只能通信、使不上”的驱动器）
        self.follow_402 = follow_402
        # echo_mode = False 时写 0x6060 不影响 0x6061（复现“模式设不进去”的驱动器）
        self.echo_mode = echo_mode
        self.controlword = 0
        self.state402 = CIA402_SWITCH_ON_DISABLED
        # PP 最小模型：位置 / 目标 / 速度，以及运动结束时刻
        self.position = 0
        self.target = 0
        self.velocity = 0
        self._move_start = 0.0
        self._move_end: float | None = None
        self._move_from = 0
        self._reached = True
        self._setpoint_ack = False
        self._publish_statusword()

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
        """下一次要主动干活的时刻（monotonic 秒）；没有周期任务返回 None。"""
        deadlines = [d for d in (self._next_heartbeat, self._move_end) if d is not None]
        return min(deadlines) if deadlines else None

    def tick(self, now: float) -> None:
        """事件循环每次醒来调一次（不管有没有收到帧）。"""
        self._tick_motion(now)
        if self._next_heartbeat is None or now < self._next_heartbeat:
            return
        self.send_cob(0x700 + self.node_id, bytes([self.nmt]), pad=False)
        # 不追赶欠下的拍数：迟了就迟了，如实反映 USB-CAN 的抖动
        self._next_heartbeat = now + self.heartbeat_ms / 1000.0

    # ---- PP 运动（最小模型：匀速，无加减速）--------------------------
    def _tick_motion(self, now: float) -> None:
        if self._move_end is None or now < self._move_end:
            return
        self.position = self.target
        self._move_end = None
        self._reached = True
        self.od.set_internal(0x6064, 0, self.position & 0xFFFFFFFF)
        self._publish_statusword()
        self.log(f"PP：到位 {self.position} pulse")

    def _start_move(self, now: float) -> None:
        """6040 bit4 上升沿：把 607A 当目标，按 6081 匀速走完。"""
        self.target = self.od.read(0x607A, 0)[0]
        if self.target >= 0x80000000:  # 转为 32 位有符号位置
            self.target -= 0x100000000
        self.velocity = self.od.read(0x6081, 0)[0]
        self._move_from = self.position
        distance = abs(self.target - self.position)
        self._reached = distance == 0
        if self.velocity <= 0 or distance == 0:
            self._move_end = None
            self.position = self.target
        else:
            self._move_start = now
            self._move_end = now + distance / float(self.velocity)
        self._setpoint_ack = True
        self.od.set_internal(0x6064, 0, self.position & 0xFFFFFFFF)
        self._publish_statusword()
        self.log(
            f"PP：新目标 {self.target} pulse，速度 {self.velocity} pulse/s，"
            f"距离 {distance}"
        )

    def _halt_move(self, now: float) -> None:
        """6040 bit8：减速停。本模型直接停在当前位置。"""
        if self._move_end is not None:
            done = (now - self._move_start) * self.velocity
            step = min(abs(self.target - self._move_from), int(done))
            sign = 1 if self.target >= self._move_from else -1
            self.position = self._move_from + sign * step
            self._move_end = None
            self.od.set_internal(0x6064, 0, self.position & 0xFFFFFFFF)
        self._reached = True
        self._publish_statusword()
        self.log(f"PP：Halt，停在 {self.position} pulse")

    def _statusword(self) -> int:
        sw = self.state402
        if self._reached:
            sw |= SW_TARGET_REACHED
        if self._setpoint_ack:
            sw |= SW_SETPOINT_ACK
        return sw

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
        elif index == 0x6060:
            # 操作模式显示（6061）跟随设定值，跟真驱动器一致
            if self.echo_mode:
                self.od.set_internal(0x6061, 0, entry.value)
            else:
                self.log(f"0x6060 写入 {entry.value:#04x} 但 0x6061 不跟随（--no-mode-echo）")
        elif index == 0x6040:
            self.apply_controlword(entry.value)
        elif index == 0x607A:
            pass  # 目标位置先存着，等 bit4 上升沿才真正开始走
        elif index == 0x6081:
            pass  # 速度同上

    # ---- CiA 402 使能与 PP 触发 ---------------------------------------
    def apply_controlword(self, cw: int) -> None:
        """控制字 → 402 迁移 + PP 触发（bit4 上升沿 / bit8 停顿）。"""
        prev = self.controlword
        self.controlword = cw
        if not self.follow_402:
            self.log(f"402：收到控制字 0x{cw:04X}，但状态机未启用（--no-402）")
            return
        if (cw & 0x0F) == 0x00:  # 断使能（写 0x00）
            self._set_state402(CIA402_SWITCH_ON_DISABLED, "禁止电压输出")
            return
        if self.state402 == CIA402_FAULT:
            if cw & 0x80:  # bit7 上升沿：故障复位
                self._set_state402(CIA402_SWITCH_ON_DISABLED, "故障复位")
            return
        if (cw & 0x02) and not (cw & 0x04):  # quick stop
            self._halt_move(time.monotonic())
            self._set_state402(CIA402_SWITCH_ON_DISABLED, "Quick stop")
            return
        if (cw & 0x8F) == 0x0F:  # Enable operation（0x1F/0x2F/0x4F/0x5F 同）
            self._set_state402(CIA402_OP_ENABLED, "Enable operation")
        elif (cw & 0x87) == 0x07:  # Switch on
            if self.state402 in (CIA402_READY, CIA402_SWITCHED_ON):
                self._set_state402(CIA402_SWITCHED_ON, "Switch on")
        elif (cw & 0x87) == 0x06:  # Shutdown
            self._set_state402(CIA402_READY, "Shutdown")

        now = time.monotonic()
        if (cw & 0x100) and not (prev & 0x100):  # bit8 上升沿：减速停
            self._halt_move(now)
        elif not (cw & 0x100) and (prev & 0x100):
            self._reached = True
            self._publish_statusword()
        if (cw & 0x10) and not (prev & 0x10):  # bit4 上升沿：新目标
            self._start_move(now)
        elif not (cw & 0x10) and (prev & 0x10):
            self._setpoint_ack = False
            self._publish_statusword()

    def _set_state402(self, state: int, why: str) -> None:
        if state == self.state402:
            return
        self.state402 = state
        self._publish_statusword()
        self.log(f"402：{why} → 状态字 0x{state:04X}")

    def _publish_statusword(self) -> None:
        """状态字是只读对象，由设备自己更新（不走 SDO 写权限检查）。"""
        self.od.set_internal(0x6041, 0, self._statusword())

    def sdo_abort(self, index: int, sub: int, code: int) -> None:
        self.send_cob(
            0x580 + self.node_id,
            bytes([0x80])
            + index.to_bytes(2, "little")
            + bytes([sub])
            + code.to_bytes(4, "little"),
        )
        self.log(f"SDO abort 0x{index:04X}:{sub:02X} → 0x{code:08X}")
