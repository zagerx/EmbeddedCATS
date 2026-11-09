"""对象字典（OD）：条目模型 + 每个节点各自一份的实例。

本文件只负责“数据的规则”：

  - 条目是否存在、读/写权限、数据长度是否匹配；
  - 不满足时抛 SdoAbort，携带 DS 301 7.2.4.2 的 abort 码。

不做的事：CAN 帧的编解码（在 node.py）、CiA 402 语义（模拟器不解释 402，只按字典回应读写）。

**一站一份**：load() 每次调用都重新解析 od_data.json 并新建 Entry 对象，
两个节点共用同一份数据模板，但各自持有自己的条目 ⇒ 写节点 1 不影响节点 2。

数据在 od_data.json，不在代码里：数值与出处（手册章节 / DS301 标准 / 待扫描）
都写在那个文件里，本文件只负责规则。
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

RO = "ro"
RW = "rw"
WO = "wo"

# ---- SDO abort 码（DS 301 7.2.4.2）-----------------------------------
ABORT_UNSUPPORTED = 0x06010000  # unsupported access to an object
ABORT_WRITE_ONLY = 0x06010001   # attempt to read a write only object
ABORT_READ_ONLY = 0x06010002    # attempt to write a read only object
ABORT_NO_OBJECT = 0x06020000    # object does not exist in the object dictionary
ABORT_LENGTH = 0x06070010       # data type / length does not match
ABORT_NO_SUBINDEX = 0x06090011  # sub-index does not exist


class SdoAbort(Exception):
    """SDO 服务器要回 abort：code 就是应答帧里的 4 字节 abort code。"""

    def __init__(self, code: int) -> None:
        super().__init__(f"SDO abort 0x{code:08X}")
        self.code = code


@dataclass
class Entry:
    """对象字典条目。size = 1/2/4 字节；ref 记录数值出处。"""

    value: int
    size: int
    access: str
    name: str = ""
    ref: str = ""

    @property
    def mask(self) -> int:
        return (1 << (8 * self.size)) - 1


class Od:
    """一个节点的对象字典视图。"""

    def __init__(self, entries: dict[tuple[int, int], Entry]) -> None:
        self._entries = entries

    def entry(self, index: int, sub: int) -> Entry:
        """查条目；不存在时按“索引没有 / 子索引没有”回不同的 abort。"""
        entry = self._entries.get((index, sub))
        if entry is not None:
            return entry
        if any(idx == index for idx, _ in self._entries):
            raise SdoAbort(ABORT_NO_SUBINDEX)
        raise SdoAbort(ABORT_NO_OBJECT)

    def read(self, index: int, sub: int) -> tuple[int, Entry]:
        """读一个条目；返回 (值, 条目) 供日志用名字。"""
        entry = self.entry(index, sub)
        if entry.access == WO:
            raise SdoAbort(ABORT_WRITE_ONLY)
        return entry.value & entry.mask, entry

    def write(self, index: int, sub: int, value: int, size: int) -> Entry:
        """写一个条目；长度不符回 abort，写超范围按条目宽度截断。"""
        entry = self.entry(index, sub)
        if entry.access == RO:
            raise SdoAbort(ABORT_READ_ONLY)
        if size != entry.size:
            raise SdoAbort(ABORT_LENGTH)
        entry.value = value & entry.mask
        return entry

    def set_internal(self, index: int, sub: int, value: int) -> Entry:
        """设备自己更新只读条目（如 6041 状态字）。不检查访问权限，不走 SDO。"""
        entry = self.entry(index, sub)
        entry.value = value & entry.mask
        return entry


DATA_FILE = Path(__file__).with_name("od_data.json")


def _num(value: object) -> int:
    """json 里允许写 "0x1000" 这样的十六进制字符串（便于与手册对照）。"""
    return int(value, 0) if isinstance(value, str) else int(value)


def load(path: Path | None = None) -> dict[tuple[int, int], Entry]:
    """解析 od_data.json，返回一份全新的条目表（每节点调一次）。"""
    path = path or DATA_FILE
    with path.open(encoding="utf-8") as fh:
        doc = json.load(fh)
    entries: dict[tuple[int, int], Entry] = {}
    for item in doc["entries"]:
        key = (_num(item["index"]), int(item["sub"]))
        if key in entries:
            raise ValueError(f"对象字典重复条目：0x{item['index']}:{item['sub']:02X}")
        entries[key] = Entry(
            value=_num(item["value"]),
            size=int(item["size"]),
            access=str(item["access"]),
            name=str(item.get("name", "")),
            ref=str(item.get("ref", "")),
        )
    return entries
