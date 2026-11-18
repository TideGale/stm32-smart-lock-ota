#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""往智能门锁发 OTA 升级包。

用法（先关掉串口助手，别让它占着 COM 口）：

    python send_fw.py COM4 门锁OTA升级包\\AppA.bin

脚本会自己发 'D' 让板子进收包模式，等它打印 ready 之后把 bin 流式发过去，
最后拿板子报的 CRC 和本地文件的 CRC 对一下，对不上就说明传输丢包。
"""

import re
import sys
import time
import zlib
import threading

import serial

CHUNK = 512                      # 一次写多少字节，太大 PC 端驱动缓冲会顶住
IDLE_WAIT = 9.0                  # 发完等多久（板子静默 5 秒才结束接收）


def main():
    if len(sys.argv) < 3:
        print("用法: python send_fw.py <串口号> <bin 文件>")
        print("例如: python send_fw.py COM4 门锁OTA升级包\\AppA.bin")
        return 2

    port = sys.argv[1]
    path = sys.argv[2]

    with open(path, "rb") as f:
        data = f.read()

    file_crc = zlib.crc32(data) & 0xFFFFFFFF
    print("文件 %s" % path)
    print("长度 %d 字节  CRC32=0x%08X" % (len(data), file_crc))

    ser = serial.Serial(port, 115200, bytesize=8, parity="N", stopbits=1, timeout=0.1)

    ready = threading.Event()
    seen = {"text": "", "recv": None, "crc": None}

    def reader():
        tail = b""
        while True:
            try:
                chunk = ser.read(256)
            except Exception:
                return
            if not chunk:
                continue

            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()

            tail = (tail + chunk)[-400:]
            seen["text"] += chunk.decode("utf-8", "replace")

            if b"send .bin now" in tail:
                ready.set()

            m = re.search(rb"recv (\d+) bytes, crc=0x([0-9A-Fa-f]+)", tail)
            if m:
                seen["recv"] = int(m.group(1))
                seen["crc"] = int(m.group(2), 16)

    threading.Thread(target=reader, daemon=True).start()

    time.sleep(0.3)
    ser.write(b"D")
    print("\n[PC] 已发 D，等板子擦完暂存区 ...")

    if not ready.wait(30):
        print("[PC] 等不到 ready 提示，检查板子是否在跑 App、串口是否被占用")
        ser.close()
        return 3

    time.sleep(0.3)

    print("[PC] 开始发送 %d 字节 ..." % len(data)]
    t0 = time.time()
    for i in range(0, len(data), CHUNK):
        ser.write(data[i:i + CHUNK])
        ser.flush()

    print("[PC] 发完，用时 %.2f 秒，等板子校验 ..." % (time.time() - t0))
    time.sleep(IDLE_WAIT)

    ser.close()

    ok = True
    if seen["recv"] is None:
        print("\n[PC] 没看到板子的 recv 汇总行")
        ok = False
    else:
        print("\n[PC] 板子收到 %d 字节，CRC32=0x%08X" % (seen["recv"], seen["crc"] or 0))
        if seen["recv"] != len(data):
            print("[PC] 长度对不上，丢了 %d 字节" % (len(data) - seen["recv"]))
            ok = False
        if seen["crc"] != file_crc:
            print("[PC] CRC 对不上，包是坏的，别发 A")
            ok = False

    if ok:
        print("[PC] 包完整，可以发 A 应用升级")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())