#!/usr/bin/env python3
"""Эмулятор прошивки TC001 на pty — для проверки агента без железа.

    python3 fake_device.py            # печатает путь порта
    python3 agent.py --port <путь>

Ведёт себя как main.cpp: мусор ROM-бутлоадера, hello, pong, по получении apps
сразу шлёт req на каждое, печатает то, что было бы на экране.
"""
import json
import os
import pty
import sys
import time
import tty


def main(duration=None):
    master, slave = pty.openpty()
    tty.setraw(slave)
    print(os.ttyname(slave), flush=True)

    def send(msg):
        os.write(master, (json.dumps(msg) + "\n").encode())

    state = {"font": "5x7", "wifi": {"ssid": "", "state": "off"}}
    hello = lambda n: {"t": "hello", "fw": "fake", "apps": n, "font": state["font"], "wifi": state["wifi"]}

    os.write(master, b"ets Jun  8 2016 00:22:57\r\nrst:0x1 (POWERON_RESET),boot:0x13\r\n")
    send(hello(0))

    apps, pending, next_id, buf = [], {}, 1, b""
    t0 = time.time()
    while duration is None or time.time() - t0 < duration:
        try:
            buf += os.read(master, 4096)
        except OSError:
            break
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            msg = json.loads(line)
            t = msg["t"]
            if t == "ping":
                send({"t": "pong"})
            elif t == "hello?":
                send(hello(len(apps)))
            elif t == "time":
                print(f"[screen] clock synced epoch={msg['epoch']} tz={msg['tz']} tzp={msg.get('tzp')}", flush=True)
            elif t == "icon":
                print(f"[device] icon {msg['id']} stored ({len(msg['px'])} hex chars)", flush=True)
            elif t == "settings":
                state["font"] = msg.get("font", state["font"])
                print(f"[device] default font -> {state['font']}", flush=True)
                send(hello(len(apps)))
            elif t == "wifi":
                state["wifi"] = {"ssid": msg["ssid"], "state": "connected" if msg["ssid"] else "off"}
                if msg["ssid"]:
                    state["wifi"].update(ip="192.168.1.77", rssi=-58)
                print(f"[device] wifi -> {state['wifi']}", flush=True)
                send({"t": "wifi", **state["wifi"]})
            elif t == "apps":
                apps = msg["apps"]
                print(f"[device] got {len(apps)} apps", flush=True)
                for a in apps:
                    req = {"t": "req", "id": next_id, "url": a["url"]}
                    for k in ("path", "find", "keep", "re"):
                        if k in a:
                            req[k] = a[k]
                    pending[next_id] = a
                    next_id += 1
                    send(req)
            elif t == "resp":
                a = pending.pop(msg["id"], {})
                v = msg.get("body")
                if msg["status"] // 100 == 2 and v is not None:
                    if "scale" in a or "dec" in a:
                        v = f"{float(v) * a.get('scale', 1):.{a.get('dec', 2)}f}"
                    print(f"[screen] {a.get('name')}: {a.get('fmt', '{v}').replace('{v}', v)}", flush=True)
                else:
                    print(f"[screen] {a.get('name')}: error status={msg['status']}", flush=True)
            elif t == "notify":
                print(f"[screen] NOTIFY {msg.get('text')!r} {msg.get('color')} icon={msg.get('icon')}", flush=True)


if __name__ == "__main__":
    main(float(sys.argv[1]) if len(sys.argv) > 1 else None)
