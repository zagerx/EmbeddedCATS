#!/usr/bin/env python3
"""2LD2-CAN7020B 配置基准的 SDO 工具（供 skill 使用）。

命令：
  probe [iface]                 监听 3 s，报告两轴心跳
  check [iface]                 只读核对基线（身份值 + 配置参数），PASS/FAIL
  apply [iface]                 写 2403=0 -> 2404=0x16 -> 2405=0x01 -> 607E=128，
                                回读后 save (1010h:01)
  watch [iface]                 60 s 监视两轴 60FD（按压开关复验）
  jog [iface] <node> [units]    PP 正转 units（默认 10000 = 1 圈），验收后断使能
  home [iface] <node> [s]       HM 方法 21：启动回零，等 bit12/bit13

只依赖 python-can（socketcan）。
"""
import sys
import time

import can


def make_bus(iface):
    return can.Bus(interface="socketcan", channel=iface)


class Sdo:
    def __init__(self, bus):
        self.bus = bus

    def _rx(self, node, idx, sub, timeout):
        end = time.time() + timeout
        while time.time() < end:
            m = self.bus.recv(timeout=max(0.0, end - time.time()))
            if m is None:
                return None
            if m.arbitration_id == 0x580 + node and len(m.data) == 8 and \
               (m.data[1] | (m.data[2] << 8)) == idx and m.data[3] == sub:
                return m
        return None

    def read(self, node, idx, sub=0, tries=2):
        for _ in range(tries):
            self.bus.send(can.Message(arbitration_id=0x600 + node,
                                      data=bytes([0x40, idx & 0xFF, idx >> 8, sub, 0, 0, 0, 0]),
                                      is_extended_id=False))
            m = self._rx(node, idx, sub, 0.3)
            if m is not None:
                d = m.data
                if d[0] == 0x80:
                    return "ABORT:%08X" % int.from_bytes(d[4:8], "little")
                if d[0] & 0x01:
                    n = 4 - ((d[0] >> 2) & 3)
                    return int.from_bytes(d[4:4 + n], "little", signed=True)
                return None
        return "TIMEOUT"

    def write(self, node, idx, value, size, sub=0, tries=2):
        cmds = {1: 0x2F, 2: 0x2B, 4: 0x23}
        req = bytes([cmds[size], idx & 0xFF, idx >> 8, sub]) + int(value).to_bytes(size, "little", signed=value < 0)
        req += bytes(8 - len(req))
        for _ in range(tries):
            self.bus.send(can.Message(arbitration_id=0x600 + node, data=req, is_extended_id=False))
            m = self._rx(node, idx, sub, 0.3)
            if m is not None:
                if m.data[0] == 0x60:
                    return "OK"
                if m.data[0] == 0x80:
                    return "ABORT:%08X" % int.from_bytes(m.data[4:8], "little")
        return "TIMEOUT"


def cmd_probe(bus):
    end = time.time() + 3
    seen = {}
    while time.time() < end:
        m = bus.recv(timeout=max(0.0, end - time.time()))
        if m is not None and m.arbitration_id in (0x701, 0x702):
            seen.setdefault(m.arbitration_id, m.data[:1].hex())
    for node in (1, 2):
        st = seen.get(0x700 + node)
        print("节点%d 心跳: %s" % (node, ("状态 0x%s" % st) if st else "无"))
    print("结果:", "PASS" if 0x701 in seen and 0x702 in seen else "FAIL")


EXPECT_PARAMS = [
    (0x2403, 0, 0), (0x2404, 0, 0x16), (0x2405, 0, 0x01),
    (0x2504, 0, 0), (0x607E, 0, 128), (0x2015, 0, 0),
]
EXPECT_ID = [
    (0x1000, 0, 0x00020192), (0x6502, 0, 0x3002D),
    (0x608F, 1, 131072), (0x608F, 2, 1), (0x6092, 1, 10000),
]


def cmd_check(bus):
    sdo = Sdo(bus)
    ok = True
    for node in (1, 2):
        print("== 节点%d ==" % node)
        for idx, sub, exp in [(0x2023, 0, node), (0x2024, 0, 2)] + EXPECT_ID + EXPECT_PARAMS:
            v = sdo.read(node, idx, sub)
            p = "PASS" if v == exp else "FAIL"
            if v != exp:
                ok = False
            print("  %04X:%02X = %-12s 期望 %-10s %s" % (idx, sub, v, exp, p))
        mv = sdo.read(node, 0x6079)
        print("  6079 母线电压 = %s mV" % mv)
        if not isinstance(mv, int) or mv < 40000:
            ok = False
            print("  FAIL：母线电压异常（检查 48V）")
    print("结果:", "PASS" if ok else "FAIL")


def cmd_apply(bus):
    sdo = Sdo(bus)
    steps = [(0x2403, 0, 4), (0x2404, 0x16, 4), (0x2405, 0x01, 4), (0x607E, 128, 4)]
    for node in (1, 2):
        print("== 节点%d ==" % node)
        for idx, val, size in steps:
            r = sdo.write(node, idx, val, size)
            print("  写 %04X <- %s : %s" % (idx, hex(val), r))
        for idx, val, _ in steps:
            v = sdo.read(node, idx)
            print("  回读 %04X = %s %s" % (idx, v, "PASS" if v == val else "FAIL"))
        r = sdo.write(node, 0x1010, 0x65766173, 4, sub=1)
        print("  save 1010h:01 : %s" % r)
    print("完成。断电重启后必须再跑 check 复验。")


def cmd_watch(bus, seconds=60):
    sdo = Sdo(bus)
    names = {0: "NOT", 1: "POT", 2: "HOME", 24: "INP", 25: "BRAKE", 31: "Z"}
    last = {}
    t0 = time.time()
    print("监视 60FD %d s，按压开关（每步保持 2 s）：" % seconds)
    while time.time() - t0 < seconds:
        for node in (1, 2):
            v = sdo.read(node, 0x60FD, tries=1)
            if not isinstance(v, int):
                continue
            if node not in last:
                last[node] = v
                print("%5.1fs 轴%d 基线 0x%08X" % (time.time() - t0, node, v))
            elif v != last[node]:
                old, new = last[node], v
                diff = old ^ new
                ch = ", ".join("%s:%d->%d" % (names.get(b, "bit%d" % b), (old >> b) & 1, (new >> b) & 1)
                               for b in sorted(names) if diff >> b & 1)
                print("%5.1fs 轴%d 0x%08X -> 0x%08X [%s]" % (time.time() - t0, node, old, new, ch))
                last[node] = v
        time.sleep(0.1)


def _enable(sdo, node):
    bus = sdo.bus
    bus.send(can.Message(arbitration_id=0x000, data=[0x01, node], is_extended_id=False))
    time.sleep(0.2)
    for val in (0x0002, 0x0006, 0x0007, 0x000F):
        sdo.write(node, 0x6040, val, 2)
        time.sleep(0.25)
    return sdo.read(node, 0x6041)


def _disable(sdo, node):
    sdo.write(node, 0x6060, 1, 1)
    sdo.write(node, 0x6040, 0x0000, 2)
    time.sleep(0.1)
    print("  已断使能，6041 = 0x%04X" % (sdo.read(node, 0x6041) & 0xFFFF))


def cmd_jog(bus, node, units=10000, vel=2000):
    sdo = Sdo(bus)
    sw = _enable(sdo, node)
    print("  使能 6041=0x%04X" % (sw & 0xFFFF))
    pos = sdo.read(node, 0x6064)
    sdo.write(node, 0x6081, vel, 4)
    sdo.write(node, 0x6083, vel, 4)
    sdo.write(node, 0x6084, vel, 4)
    sdo.write(node, 0x607A, pos + units, 4)
    sdo.write(node, 0x6040, 0x001F, 2)
    time.sleep(0.05)
    sdo.write(node, 0x6040, 0x000F, 2)
    end = time.time() + max(10, abs(units) // vel + 5)
    while time.time() < end:
        if sdo.read(node, 0x6041) & 0x0400:
            break
        time.sleep(0.1)
    end_pos = sdo.read(node, 0x6064)
    print("  起点 %d -> 终点 %d，增量 %d（期望 %d）%s" % (
        pos, end_pos, end_pos - pos, units, "PASS" if abs((end_pos - pos) - units) <= 10 else "FAIL"))
    _disable(sdo, node)


def cmd_home(bus, node, timeout=60):
    sdo = Sdo(bus)
    sdo.write(node, 0x6060, 6, 1)
    sdo.write(node, 0x6098, 21, 1)
    sdo.write(node, 0x6099, 2000, 4, sub=1)
    sdo.write(node, 0x6099, 500, 4, sub=2)
    sdo.write(node, 0x609A, 2000, 4)
    sw = _enable(sdo, node)
    print("  使能 6041=0x%04X，开始回零（bit4 保持 1）" % (sw & 0xFFFF))
    sdo.write(node, 0x6040, 0x001F, 2)
    end = time.time() + timeout
    while time.time() < end:
        sw = sdo.read(node, 0x6041)
        if sw & 0x1000:
            print("  回零完成：6041=0x%04X 6064=%d" % (sw & 0xFFFF, sdo.read(node, 0x6064)))
            break
        if sw & 0x2000:
            print("  回零错误：6041=0x%04X" % (sw & 0xFFFF))
            break
        time.sleep(0.05)
    else:
        print("  超时未完成：6041=0x%04X" % (sdo.read(node, 0x6041) & 0xFFFF))
    sdo.write(node, 0x6040, 0x000F, 2)
    _disable(sdo, node)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    cmd = sys.argv[1]
    iface = sys.argv[2] if len(sys.argv) > 2 else "can0"
    bus = make_bus(iface)
    try:
        if cmd == "probe":
            cmd_probe(bus)
        elif cmd == "check":
            cmd_check(bus)
        elif cmd == "apply":
            cmd_apply(bus)
        elif cmd == "watch":
            cmd_watch(bus)
        elif cmd == "jog":
            cmd_jog(bus, int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 10000)
        elif cmd == "home":
            cmd_home(bus, int(sys.argv[3]), int(sys.argv[4]) if len(sys.argv) > 4 else 60)
        else:
            print(__doc__)
            return 1
    finally:
        bus.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
