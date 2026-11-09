#!/usr/bin/env python3
"""虚拟总线自测：不需要 CAN 硬件。

    python3 firmware/apps/tools/fake_drive/selftest.py

用 python-can 的 virtual 接口在同一进程里开“主站口”和“从站口”，
把 fake_drive.Drive 挂在从站口上，逐帧断言应答。

这里是回归基线：以后每加一项功能（心跳、402、运动模型），先在这里补用例，
再上真实 CAN 口对连复验。
"""

from __future__ import annotations

import sys
import time

import can

from fake_drive import Drive

CHANNEL = "cats-selftest"
NODES = [1, 2]
TIMEOUT = 0.02

failures: list[str] = []


def check(name: str, cond: bool, detail: str = "") -> None:
    if cond:
        print(f"  ok    {name}")
    else:
        failures.append(name)
        print(f"  FAIL  {name}  {detail}")


# ---- 总线小工具 ------------------------------------------------------
def send(bus: can.BusABC, cob_id: int, data: bytes) -> None:
    bus.send(can.Message(arbitration_id=cob_id, data=bytes(data), is_extended_id=False))


def drain(bus: can.BusABC) -> list[can.Message]:
    frames = []
    while True:
        msg = bus.recv(TIMEOUT)
        if msg is None:
            return frames
        frames.append(msg)


def pump(drive: Drive) -> None:
    while drive.poll(TIMEOUT / 4):
        pass


def pump_for(drive: Drive, seconds: float) -> None:
    """跑一段真实时间，让周期任务（心跳）到期。"""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        drive.poll(TIMEOUT / 4)


def only(frames: list[can.Message], cob_id: int) -> can.Message | None:
    hits = [f for f in frames if f.arbitration_id == cob_id]
    return hits[0] if len(hits) == 1 else None


def sdo_read(drive, master, nid: int, index: int, sub: int = 0):
    send(master, 0x600 + nid, bytes([0x40, index & 0xFF, index >> 8, sub, 0, 0, 0, 0]))
    pump(drive)
    return only(drain(master), 0x580 + nid)


def sdo_write(drive, master, nid: int, index: int, sub: int, value: int, size: int):
    cs = 0x20 | ((4 - size) << 2) | 0x03  # 加速 + 长度有效
    payload = (
        bytes([cs, index & 0xFF, index >> 8, sub])
        + value.to_bytes(size, "little")
        + bytes(4 - size)
    )
    send(master, 0x600 + nid, payload)
    pump(drive)
    return only(drain(master), 0x580 + nid)


def test_default_heartbeat() -> None:
    """默认配置：boot-up 后自动按出厂周期（500ms）起跳，通讯复位后仍继续。"""
    master = can.Bus(interface="virtual", channel=CHANNEL + "-hb")
    slave = can.Bus(interface="virtual", channel=CHANNEL + "-hb")
    drive = Drive(slave, NODES, log=lambda _text: None)  # 默认出厂周期

    drive.start()
    pump_for(drive, 1.3)
    frames = drain(master)
    hb1 = [f for f in frames if f.arbitration_id == 0x701]
    check("默认配置：boot-up 后自动发心跳（1.3s 内 ≥3 帧）",
          len(hb1) >= 3, f"实际 {len(hb1)} 帧")
    check("首帧 boot-up(0x00)，其后 Pre-op(0x7F)",
          bool(hb1)
          and bytes(hb1[0].data)[0] == 0x00
          and all(bytes(f.data)[0] == 0x7F for f in hb1[1:]),
          f"实际 {[bytes(f.data)[0] for f in hb1]}")
    check("节点 2 同样自动发心跳",
          any(f.arbitration_id == 0x702 for f in frames))

    send(master, 0x000, bytes([0x82, 0x01]))  # reset communication，节点 1
    pump_for(drive, 1.2)
    hb1 = [f for f in drain(master) if f.arbitration_id == 0x701]
    check("通讯复位后按出厂周期继续发（boot-up + ≥2 心跳）",
          len(hb1) >= 3 and bytes(hb1[0].data)[0] == 0x00,
          f"实际 {[bytes(f.data)[0] for f in hb1]}")

    master.shutdown()
    slave.shutdown()


def main() -> int:
    master = can.Bus(interface="virtual", channel=CHANNEL)
    slave = can.Bus(interface="virtual", channel=CHANNEL)
    drive = Drive(slave, NODES, log=lambda _text: None, heartbeat_ms=0)

    print("fake_drive 自测（virtual 总线）")
    print(f"  节点 {NODES}，经典 CAN")

    # 1. 上电 boot-up
    drive.start()
    frames = drain(master)
    check("上电各发一帧 boot-up", {f.arbitration_id for f in frames} == {0x701, 0x702},
          f"实际 {[hex(f.arbitration_id) for f in frames]}")
    check("boot-up 数据为 0x00", all(bytes(f.data)[0] == 0x00 for f in frames))

    # 2. SDO 加速读
    resp = sdo_read(drive, master, 1, 0x1000)
    check(
        "读 0x1000:00 = 0x00020192",
        resp is not None and bytes(resp.data) == bytes([0x43, 0x00, 0x10, 0x00, 0x92, 0x01, 0x02, 0x00]),
        f"实际 {None if resp is None else bytes(resp.data).hex(' ')}",
    )

    # 3. SDO 加速写 + 两节点独立
    resp = sdo_write(drive, master, 1, 0x1017, 0, 1000, 2)
    check("写 0x1017:00 节点 1 确认帧", resp is not None and bytes(resp.data)[0] == 0x60,
          f"实际 {None if resp is None else bytes(resp.data).hex(' ')}")
    resp = sdo_read(drive, master, 1, 0x1017)
    check("节点 1 读回 1000", resp is not None and int.from_bytes(bytes(resp.data)[4:6], "little") == 1000,
          f"实际 {None if resp is None else bytes(resp.data).hex(' ')}")
    resp = sdo_read(drive, master, 2, 0x1017)
    check("节点 2 的 0x1017 不受影响（仍为 0）",
          resp is not None and int.from_bytes(bytes(resp.data)[4:6], "little") == 0,
          f"实际 {None if resp is None else bytes(resp.data).hex(' ')}")

    # 4. abort：对象不存在 / 只读 / 长度不符
    resp = sdo_read(drive, master, 1, 0x9999)
    check("读不存在对象 → abort 0x06020000",
          resp is not None and bytes(resp.data) == bytes([0x80, 0x99, 0x99, 0x00, 0x00, 0x00, 0x02, 0x06]),
          f"实际 {None if resp is None else bytes(resp.data).hex(' ')}")
    resp = sdo_write(drive, master, 2, 0x1000, 0, 0, 4)
    check("写只读对象 → abort 0x06010002",
          resp is not None and bytes(resp.data) == bytes([0x80, 0x00, 0x10, 0x00, 0x02, 0x00, 0x01, 0x06]),
          f"实际 {None if resp is None else bytes(resp.data).hex(' ')}")
    resp = sdo_write(drive, master, 1, 0x1017, 0, 1000, 4)
    check("写 2 字节对象用 4 字节 → abort 0x06070010",
          resp is not None and bytes(resp.data) == bytes([0x80, 0x17, 0x10, 0x00, 0x10, 0x00, 0x07, 0x06]),
          f"实际 {None if resp is None else bytes(resp.data).hex(' ')}")

    # 5. NMT stopped：该节点不应答 SDO，另一个节点照常
    send(master, 0x000, bytes([0x02, 0x01]))  # stop 节点 1
    pump(drive)
    drain(master)
    resp = sdo_read(drive, master, 1, 0x1000)
    check("Stopped 节点不应答 SDO", resp is None,
          f"实际 {None if resp is None else bytes(resp.data).hex(' ')}")
    resp = sdo_read(drive, master, 2, 0x1000)
    check("另一个节点照常应答", resp is not None, "无应答")

    # 6. NMT reset node 广播 → 两节点都重发 boot-up
    send(master, 0x000, bytes([0x81, 0x00]))
    pump(drive)
    frames = drain(master)
    check("reset node 广播后都发 boot-up",
          {f.arbitration_id for f in frames} == {0x701, 0x702},
          f"实际 {[hex(f.arbitration_id) for f in frames]}")
    resp = sdo_read(drive, master, 1, 0x1000)
    check("复位后节点 1 恢复应答", resp is not None, "无应答")

    # 7. 心跳（0x1017）：写周期 → 周期发 → 内容跟 NMT 状态 → 写 0 停发
    drain(master)
    resp = sdo_write(drive, master, 1, 0x1017, 0, 50, 2)
    check("写 0x1017 = 50ms", resp is not None and bytes(resp.data)[0] == 0x60,
          f"实际 {None if resp is None else bytes(resp.data).hex(' ')}")
    pump_for(drive, 0.26)
    frames = drain(master)
    hb1 = [f for f in frames if f.arbitration_id == 0x701]
    check("节点 1 周期发心跳（260ms 内 ≥4 帧）", len(hb1) >= 4, f"实际 {len(hb1)} 帧")
    check("心跳内容 = Pre-op(0x7F)",
          bool(hb1) and all(bytes(f.data)[0] == 0x7F for f in hb1),
          f"实际 {[bytes(f.data)[0] for f in hb1]}")
    check("节点 2 没配心跳就不发 0x702",
          not any(f.arbitration_id == 0x702 for f in frames))

    send(master, 0x000, bytes([0x01, 0x01]))  # start 节点 1
    pump_for(drive, 0.13)
    hb1 = [f for f in drain(master) if f.arbitration_id == 0x701]
    check("start 后心跳变为 Operational(0x05)",
          bool(hb1) and all(bytes(f.data)[0] == 0x05 for f in hb1),
          f"实际 {[bytes(f.data)[0] for f in hb1]}")

    sdo_write(drive, master, 1, 0x1017, 0, 0, 2)
    drain(master)
    pump_for(drive, 0.15)
    check("写 0x1017 = 0 后停发",
          not any(f.arbitration_id == 0x701 for f in drain(master)))

    # 8. 通讯复位：心跳周期回出厂值，只留一帧 boot-up
    sdo_write(drive, master, 1, 0x1017, 0, 20, 2)
    send(master, 0x000, bytes([0x82, 0x01]))  # reset communication，节点 1
    pump_for(drive, 0.12)
    hb1 = [f for f in drain(master) if f.arbitration_id == 0x701]
    check("通讯复位后心跳停发（只留 boot-up）",
          len(hb1) == 1 and bytes(hb1[0].data)[0] == 0x00,
          f"实际 {[(hex(f.arbitration_id), bytes(f.data).hex(' ')) for f in hb1]}")

    master.shutdown()
    slave.shutdown()

    test_default_heartbeat()

    print()
    if failures:
        print(f"失败 {len(failures)} 项：{failures}")
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
