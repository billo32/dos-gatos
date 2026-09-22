#!/usr/bin/env python3
"""TC001 USB agent for macOS.

Работает как HTTP-прокси для часов через USB-serial: часы сами решают, что и когда
запрашивать, агент только выполняет запросы (TLS на стороне Mac) и возвращает ответ.

Дополнительно:
  * синхронизирует время и конфиг apps.json на часы при каждом подключении;
  * локальный API http://127.0.0.1:7765 для пуша уведомлений из скриптов.

    curl -X POST localhost:7765/notify -d '{"text":"Build OK","color":"#00FF00"}'
"""

import argparse
import json
import logging
import re
import ssl
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial
from serial.tools import list_ports

try:
    import certifi
    SSL_CTX = ssl.create_default_context(cafile=certifi.where())
except ImportError:  # python.org-сборки на macOS без certifi часто не видят системные CA
    SSL_CTX = ssl.create_default_context()

BAUD = 460800  # переопределяется --baud
# CH340 (TC001), CH9102, CP2102 (встречается в ревизиях TC001)
KNOWN_USB_IDS = {(0x1A86, 0x7523), (0x1A86, 0x55D4), (0x10C4, 0xEA60)}
PING_EVERY = 3.0
TIME_SYNC_EVERY = 3600  # периодическая подстройка часов (и RTC)
MAX_BODY_OUT = 1024          # сколько отдаём на часы
MAX_BODY_IN = 2 * 1024 * 1024  # сколько читаем из сети

log = logging.getLogger("tc001")


def find_port():
    for p in list_ports.comports():
        if (p.vid, p.pid) in KNOWN_USB_IDS:
            return p.device
    return None


def extract(text, path=None, find=None, keep=128, regex=None):
    """find/keep: окно после подстроки (как в AWTRIX NG); path: 'a.b.0.c' по JSON;
    re: регулярка по результату (первая группа, если есть, иначе всё совпадение)."""
    if find:
        i = text.find(find)
        if i < 0:
            return None
        text = text[i:i + int(keep)]
    if path:
        node = json.loads(text)
        for part in path.split("."):
            if isinstance(node, list):
                node = node[int(part)]
            else:
                node = node[part]
        text = node if isinstance(node, str) else json.dumps(node, ensure_ascii=False)
    if regex:
        m = re.search(regex, text)
        if not m:
            return None
        return m.group(1) if m.groups() else m.group(0)
    return text


def fetch(url, timeout=20):
    req = urllib.request.Request(url, headers={"User-Agent": "tc001-usb-agent/0.1"})
    try:
        with urllib.request.urlopen(req, timeout=timeout, context=SSL_CTX) as r:
            return r.status, r.read(MAX_BODY_IN).decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read(MAX_BODY_IN).decode("utf-8", "replace")


class Link:
    def __init__(self, port_arg, apps_file, tz_override, baud=BAUD):
        self.port_arg = port_arg
        self.baud = baud
        self.apps_file = apps_file
        self.tz_override = tz_override
        self.ser = None
        self.wlock = threading.Lock()
        self.pool = ThreadPoolExecutor(max_workers=4)
        self.last_rx = 0.0
        self.last_time_sync = 0.0
        self.device = {}

    # ---------- io ----------
    def send(self, msg):
        data = (json.dumps(msg, ensure_ascii=False, separators=(",", ":")) + "\n").encode()
        with self.wlock:
            if self.ser is None:
                raise ConnectionError("device not connected")
            self.ser.write(data)
        log.debug("-> %s", msg)

    def open(self, port):
        s = serial.Serial()
        s.port = port
        s.baudrate = self.baud
        s.timeout = 0.2
        # Не дёргать DTR/RTS: через них auto-reset схема сбрасывает ESP32
        s.rts = False
        s.dtr = False
        s.open()
        return s

    # ---------- protocol ----------
    def tz_offset(self):
        if self.tz_override is not None:
            return self.tz_override
        lt = time.localtime()
        return lt.tm_gmtoff

    def send_time(self):
        self.send({"t": "time", "epoch": int(time.time()), "tz": self.tz_offset()})
        self.last_time_sync = time.time()

    def on_hello(self, msg):
        self.device = msg
        log.info("device hello: fw=%s apps=%s", msg.get("fw"), msg.get("apps"))
        self.send_time()
        if self.apps_file and Path(self.apps_file).exists():
            apps = json.loads(Path(self.apps_file).read_text())
            self.send({"t": "apps", "apps": apps})
            log.info("pushed %d apps from %s", len(apps), self.apps_file)

    def on_req(self, msg):
        def work():
            rid = msg.get("id")
            try:
                status, text = fetch(msg["url"])
                body = None
                if 200 <= status < 300:
                    body = extract(text, msg.get("path"), msg.get("find"),
                                   msg.get("keep", 128), msg.get("re"))
                    if body is not None:
                        body = body[:MAX_BODY_OUT]
                log.info("req %s %s -> %s %r", rid, msg["url"][:80], status, (body or "")[:60])
                self.send({"t": "resp", "id": rid, "status": status, "body": body})
            except Exception as e:  # сеть, парсинг, path не найден
                log.warning("req %s failed: %s", rid, e)
                try:
                    self.send({"t": "resp", "id": rid, "status": 0, "body": None})
                except ConnectionError:
                    pass
        self.pool.submit(work)

    def handle(self, line):
        # вывод ROM-бутлоадера (без перевода строки) может приклеиться к началу JSON
        i = line.find("{")
        if i > 0:
            log.debug("noise: %r", line[:i][:120])
            line = line[i:]
        try:
            msg = json.loads(line)
        except ValueError:
            log.debug("noise: %r", line[:120])   # ROM-бутлоадер при ресете и т.п.
            return
        self.last_rx = time.time()
        t = msg.get("t")
        if t == "hello":
            self.on_hello(msg)
        elif t == "req":
            self.on_req(msg)
        elif t == "log":
            log.info("device: %s", msg.get("m"))
        elif t == "btn":
            log.info("button: %s", msg.get("b"))
        elif t == "pong":
            pass

    # ---------- loops ----------
    def pinger(self):
        while True:
            time.sleep(PING_EVERY)
            try:
                self.send({"t": "ping"})
                if self.device and time.time() - self.last_time_sync > TIME_SYNC_EVERY:
                    self.send_time()      # заодно ловит переход на летнее/зимнее время
            except Exception:
                pass

    def run(self):
        threading.Thread(target=self.pinger, daemon=True).start()
        while True:
            port = self.port_arg or find_port()
            if not port:
                log.info("waiting for TC001 (CH340) ...")
                time.sleep(2)
                continue
            try:
                ser = self.open(port)
            except serial.SerialException as e:
                log.warning("open %s failed: %s", port, e)
                time.sleep(2)
                continue
            log.info("connected: %s", port)
            with self.wlock:
                self.ser = ser
            try:
                self.send({"t": "hello?"})
                buf = b""
                while True:
                    chunk = ser.read(4096)
                    if not chunk:
                        continue
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line = line.strip()
                        if line:
                            self.handle(line.decode("utf-8", "replace"))
                    if len(buf) > 65536:
                        buf = b""
            except (serial.SerialException, OSError) as e:
                log.warning("link lost: %s", e)
            finally:
                with self.wlock:
                    self.ser = None
                self.device = {}
                try:
                    ser.close()
                except Exception:
                    pass
            time.sleep(1)


def make_api(link):
    class Handler(BaseHTTPRequestHandler):
        def _reply(self, code, obj):
            data = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == "/status":
                self._reply(200, {
                    "connected": link.ser is not None,
                    "last_rx_ago": round(time.time() - link.last_rx, 1) if link.last_rx else None,
                    "device": link.device,
                })
            else:
                self._reply(404, {"error": "not found"})

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            try:
                body = json.loads(self.rfile.read(n) or b"{}")
            except ValueError:
                return self._reply(400, {"error": "invalid json"})
            routes = {
                "/notify": lambda b: {"t": "notify", **b},
                "/apps": lambda b: {"t": "apps", "apps": b},
                "/bright": lambda b: {"t": "bright", "v": b.get("v", 30)},
            }
            if self.path not in routes:
                return self._reply(404, {"error": "not found"})
            try:
                link.send(routes[self.path](body))
            except ConnectionError as e:
                return self._reply(503, {"error": str(e)})
            self._reply(200, {"ok": True})

        def log_message(self, *a):
            pass

    return Handler


def main():
    ap = argparse.ArgumentParser(description="TC001 USB agent")
    ap.add_argument("--port", help="serial port (по умолчанию ищется по VID:PID)")
    ap.add_argument("--apps", default=str(Path(__file__).with_name("apps.json")),
                    help="apps.json, отправляется на часы при подключении")
    ap.add_argument("--baud", type=int, default=BAUD, help="должна совпадать с прошивкой")
    ap.add_argument("--api-port", type=int, default=7765)
    ap.add_argument("--tz", type=int, help="смещение от UTC в секундах (по умолчанию из системы)")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s", datefmt="%H:%M:%S")
    link = Link(a.port, a.apps, a.tz, a.baud)
    api = ThreadingHTTPServer(("127.0.0.1", a.api_port), make_api(link))
    threading.Thread(target=api.serve_forever, daemon=True).start()
    log.info("local API on http://127.0.0.1:%d", a.api_port)
    link.run()


if __name__ == "__main__":
    main()
