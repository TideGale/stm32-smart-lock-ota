#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""从 PC 侧验证门锁的 MQTT 链路是否真的通。

做两件事：
1. 订阅 lock/state，看门锁会不会把自己的状态发上来；
2. 往 lock/cmd 发一条 open，等门锁回执。

用法:
    python mqtt_check.py                    # 默认 broker.hivemq.com，发 open
    python mqtt_check.py broker.hivemq.com  # 指定 broker
    python mqtt_check.py broker.hivemq.com close
"""

import sys
import time

import paho.mqtt.client as mqtt

BROKER = "broker.hivemq.com"
PORT = 1883
TOPIC_CMD = "lock/cmd"
TOPIC_STATE = "lock/state"

got = {"state": None, "connected": False}


def make_client():
    # paho-mqtt 2.x 要显式给回调版本，1.x 没有这个参数
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except (AttributeError, TypeError):
        return mqtt.Client()


def main():
    global BROKER

    argv = sys.argv[1:]
    if argv:
        BROKER = argv[0]
    cmd = argv[1] if len(argv) > 1 else "open"

    print("broker = %s:%d   要发的命令 = %s" % (BROKER, PORT, cmd))

    c = make_client()

    def on_connect(client, userdata, flags, reason_code, properties=None):
        print("[PC] 连上 broker，订阅 %s" % TOPIC_STATE)
        client.subscribe(TOPIC_STATE, qos=0)
        got["connected"] = True

    def on_message(client, userdata, msg):
        payload = msg.payload.decode("utf-8", "replace")
        print("[PC] 收到 %s = %s" % (msg.topic, payload))
        if msg.topic == TOPIC_STATE:
            got["state"] = payload

    c.on_connect = on_connect
    c.on_message = on_message

    try:
        c.connect(BROKER, PORT, 30)
    except Exception as e:
        print("[PC] 连不上 broker: %s" % e)
        return 2

    c.loop_start()

    t0 = time.time()
    while not got["connected"] and time.time() - t0 < 15:
        time.sleep(0.2)

    if not got["connected"]:
        print("[PC] 15 秒没连上 broker")
        c.loop_stop()
        return 3

    time.sleep(2)                                   # 订阅生效再发，避免错过自己的消息
    print("[PC] 发布 %s -> %s" % (TOPIC_CMD, cmd))
    c.publish(TOPIC_CMD, cmd, qos=0)

    t0 = time.time()
    while got["state"] is None and time.time() - t0 < 20:
        time.sleep(0.2)

    c.loop_stop()
    c.disconnect()

    if got["state"] is None:
        print("\n[PC] 20 秒内没收到 lock/state —— 门锁没连上 broker，或者门锁没在跑")
        return 1

    print("\n[PC] 门锁回执: lock/state = %s" % got["state"])
    if got["state"] == cmd:
        print("[PC] 回执与命令一致，远程开锁链路正常")
        return 0

    print("[PC] 回执与命令不一致（可能是上一轮的状态），看门锁串口确认")
    return 1


if __name__ == "__main__":
    sys.exit(main())