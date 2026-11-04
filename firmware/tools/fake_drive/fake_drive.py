#!/usr/bin/env python3
"""CANopen 从站模拟器（假的伺服驱动器）。

本机只有 PCAN-USB Pro FD（两个通道），没有真实驱动器。把两口对连：

    can0 = 主站口（CATS 跑这一侧）
    can1 = 从站口（本程序跑这一侧）

模拟对象：一台双轴驱动器 = 同一根 CAN 上的**两个独立节点**（站号 1、2）。
两个节点的对象字典完全相同，只有站号不同（手册 3.11 / 3.12）。

当前范围（刻意保持极小）：
  - NMT（0x000）：start / stop / enter pre-operational / reset node / reset communication
  - SDO 加速传输（0x600+id 请求 → 0x580+id 响应）：读写对象字典
  - 心跳生产者（0x1017）：boot-up 之后按出厂周期自动开跑（默认 500 ms），
    内容 = 当前 NMT 状态；主站写 0x1017 可改周期，写 0 停发
  - 每个节点各自一份对象字典：写节点 1 不影响节点 2

不做的（按拍板顺序推进）：PDO / SYNC / EMCY / 分段 SDO /
CiA 402 状态机与运动模型。经典 CAN（≤8 字节），不做 CAN FD。

对象字典数据在 od_data.json（带出处），通信对象按 DS 301 标准值、
402 对象按手册 5.1.3 / 5.3；真机到货后用 tools/od_scan 的实读结果覆盖。

本程序只认 CAN 帧，不 import firmware/src 下任何模块：它是测试装置，不是固件。
"""

from __future__ import annotations

import argparse
import sys
import time

import can

from node import DEFAULT_HEARTBEAT_MS, Node


class Drive:
    """一台“双轴驱动器”：一根总线 + 若干节点。

    只做两件事：把收到的帧交给每个节点（各自判断是不是发给自己的），
    以及事件循环。协议行为在 node.py，对象字典数据在 od.py。
    """

    def __init__(
        self,
        bus: can.BusABC,
        node_ids: list[int],
        trace: bool = False,
        log=None,
        heartbeat_ms: int = DEFAULT_HEARTBEAT_MS,
        follow_402: bool = True,
        echo_mode: bool = True,
    ) -> None:
        self.bus = bus
        self.trace = trace
        self.nodes = [
            Node(
                bus,
                nid,
                trace=trace,
                log=log,
                heartbeat_ms=heartbeat_ms,
                follow_402=follow_402,
                echo_mode=echo_mode,
            )
            for nid in node_ids
        ]

    def start(self) -> None:
        """上电：每个节点先发一帧 boot-up，然后开始发心跳。"""
        for node in self.nodes:
            node.boot_up()
            node.start_heartbeat()

    def poll(self, timeout: float = 0.5) -> bool:
        """收一帧并分发；返回是否收到（没有就是超时）。

        等待时长取“调用者给的上限”与“最近一个周期任务的时限”中较小者，
        所以心跳不会因为总线静默而拖不准。
        """
        now = time.monotonic()
        deadline = self._earliest_deadline()
        wait = timeout if deadline is None else max(0.0, min(timeout, deadline - now))

        msg = self.bus.recv(wait)
        handled = False
        if msg is not None:
            handled = True
            if not (msg.is_error_frame or msg.is_extended_id):
                if self.trace:
                    print(
                        f"RX 0x{msg.arbitration_id:03X} [{msg.dlc}] "
                        f"{bytes(msg.data).hex(' ').upper()}",
                        flush=True,
                    )
                # 不按 is_rx 过滤：本机自己发的 COB-ID（0x580+id / 0x700+id）不在
                # Node 的接收集合（0x000 / 0x600+id）里，天然不会自问自答；
                # 而 python-can 的 virtual 接口不回填 is_rx，按它过滤会把帧全丢掉。
                for node in self.nodes:
                    node.on_frame(msg)

        now = time.monotonic()
        for node in self.nodes:
            node.tick(now)
        return handled

    def _earliest_deadline(self) -> float | None:
        deadlines = [
            d for d in (node.next_deadline() for node in self.nodes) if d is not None
        ]
        return min(deadlines) if deadlines else None


def parse_nodes(text: str) -> list[int]:
    nodes = [int(x, 0) for x in text.replace(" ", "").split(",") if x]
    for nid in nodes:
        if not 1 <= nid <= 127:
            raise argparse.ArgumentTypeError(f"站号必须在 1..127：{nid}")
    return nodes


def main() -> int:
    ap = argparse.ArgumentParser(
        description="CANopen 从站模拟器（当前只支持 NMT + SDO 加速读写）"
    )
    ap.add_argument("-i", "--interface", default="can1", help="CAN 通道（默认 can1）")
    ap.add_argument(
        "-n", "--nodes", default="1,2", help="模拟的站号，逗号分隔（默认 1,2）"
    )
    ap.add_argument("-t", "--trace", action="store_true", help="打印每个收发的原始帧")
    ap.add_argument(
        "--heartbeat-ms",
        type=int,
        default=DEFAULT_HEARTBEAT_MS,
        help=f"出厂心跳周期（ms），boot-up 后自动开跑；0 = 不发（默认 {DEFAULT_HEARTBEAT_MS}）",
    )
    ap.add_argument(
        "--no-402",
        action="store_true",
        help="状态字不跟控制字走（复现“能通信但使不上”的驱动器，用来测使能超时）",
    )
    ap.add_argument(
        "--no-mode-echo",
        action="store_true",
        help="写 0x6060 不影响 0x6061（复现“模式设不进去”的驱动器，用来测配置失败）",
    )
    args = ap.parse_args()

    node_ids = parse_nodes(args.nodes)
    bus = can.Bus(interface="socketcan", channel=args.interface)

    drive = Drive(
        bus,
        node_ids,
        trace=args.trace,
        heartbeat_ms=args.heartbeat_ms,
        follow_402=not args.no_402,
        echo_mode=not args.no_mode_echo,
    )
    print(
        f"fake_drive 已启动：{args.interface}，站号 {node_ids}，"
        f"经典 CAN（≤8 字节），心跳 {args.heartbeat_ms} ms，波特率由接口决定",
        flush=True,
    )
    for nid in node_ids:
        print(
            f"  节点 {nid}: NMT 0x000 | SDO 0x{0x600 + nid:03X} → 0x{0x580 + nid:03X} | "
            f"心跳 0x{0x700 + nid:03X}",
            flush=True,
        )
    drive.start()  # 上电即发 boot-up，随后开始心跳

    try:
        while True:
            drive.poll(0.5)
    except KeyboardInterrupt:
        print("收到 Ctrl-C，退出", flush=True)
    finally:
        bus.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
