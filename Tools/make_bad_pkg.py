#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""把正常的 bin 改成一个"能通过包头校验、但一启动就崩"的坏包。

用来验证试启动回滚：坏包的复位向量被指到目标槽尾部那片 0xFF 区，
Bootloader 的 CRC 校验能过（它只看内容一不一致），App 一跑就 HardFault，
于是永远确认不了，连续几次之后 Bootloader 自动退回旧槽。

用法: python make_bad_pkg.py AppB.bin AppB_bad.bin [复位向量]
"""

import sys

SLOT_A = 0x08004000
SLOT_B = 0x0801C000
SLOT_SIZE = 0x18000


def main():
    if len(sys.argv) < 3:
        print("用法: python make_bad_pkg.py <正常 bin> <坏包输出> [复位向量]")
        return 2

    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, "rb").read())

    old = int.from_bytes(data[4:8], "little")
    slot = SLOT_A if SLOT_A <= old < SLOT_A + SLOT_SIZE else SLOT_B

    if len(sys.argv) > 3:
        bad = int(sys.argv[3], 0)
    else:
        bad = slot + SLOT_SIZE - 0x4000 + 1      # 槽尾 16KB 处，那里是擦除后的 0xFF

    data[4:8] = bad.to_bytes(4, "little")
    open(dst, "wb").write(bytes(data))

    print("原复位向量 0x%08X -> 0x%08X" % (old, bad))
    print("写好了 %s，%d 字节" % (dst, len(data)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
