// In-browser stand-in for the Rust core (vite dev / screenshots). Never used inside the app.
import type { AppState, Settings, Source, Status } from "./api";

const sources: Source[] = [
  { name: "malaga-temp", url: "https://api.open-meteo.com/v1/forecast?latitude=36.72&longitude=-4.42&current=temperature_2m", path: "current.temperature_2m", fmt: "{v}C", dec: 0, every: 600, color: "#FFB23F", dur: 8 },
  { name: "btc", url: "https://api.coinbase.com/v2/prices/BTC-USD/spot", path: "data.amount", fmt: "{v}", dec: 0, every: 60, color: "#FF7A3D", icon: "", dur: 8 },
  { name: "rain-find", url: "https://api.open-meteo.com/v1/forecast?latitude=36.72&longitude=-4.42&hourly=precipitation_probability", find: "\"precipitation_probability\":[", keep: 128, re: "\\[(\\d+)", fmt: "{v}%", every: 1800, color: "#4DA3FF", dur: 6 },
  { name: "amp-futures", url: "https://example.com/api/futures/quote", path: "last", fmt: "{v}", every: 30, color: "#FFF1DC", dur: 6, off: true },
];

let settings: Settings = {
  clock: { on: true, dur: 10, pos: 0, h24: true, wday: true },
  notify: { on: true, dur: 5, text: "Hello", color: "#4DA3FF", icon: "" },
  brightness: 35,
  tz: null,
};

const status: Status = {
  connected: true,
  port: "/dev/cu.usbserial-110",
  fw: "0.4.3",
  last_rx_ago: 2,
  rtc: "rtc drift -1 s, ok",
  values: {
    "malaga-temp": { value: "21.0", status: 200, at: "20:45:02", error: null },
    btc: { value: "84486.12", status: 200, at: "20:44:12", error: null },
    "rain-find": { value: "0", status: 200, at: "20:43:01", error: null },
    "amp-futures": { value: null, status: 429, at: "20:44:30", error: "HTTP 429" },
  },
  api_error: null,
  apps_rev: 1,
  font: "3x5",
  wifi: { ssid: "", state: "off" },
  screen: "malaga-temp",
};

const log = [
  "20:39:10 INFO Dos GatOS started",
  "20:39:11 WARN port busy, retrying",
  "20:39:12 INFO connected /dev/cu.usbserial-110 · fw 0.4.3",
  "20:41:50 ERROR amp-futures · timeout after 10 s",
  "20:42:15 INFO apps saved and sent to the clock (4)",
  "20:43:01 INFO rain-find → 0 · 211 ms",
  "20:44:12 INFO btc → 84486.12 · 94 ms",
  "20:44:30 WARN amp-futures · HTTP 429",
  "20:44:58 INFO device: rtc drift -1 s, ok",
  "20:45:02 INFO malaga-temp → 21.0 · 182 ms",
];

const sun = ["", "", "", "#FFD23F", "", "", "", ""];
const handlers: Record<string, (a: Record<string, unknown>) => unknown> = {
  get_state: (): AppState => ({ status, settings, autostart: true, mac_tz: "Europe/Madrid", apps_path: "~/Library/Application Support/dev.tls1.dosgatos/apps.json", version: "0.4.3" }),
  get_status: () => status,
  get_apps: () => structuredClone(sources),
  save_apps: (a) => { sources.splice(0, sources.length, ...(a.apps as Source[])); return true; },
  save_settings: (a) => { settings = a.settings as Settings; return true; },
  test_source: () => ({ status: 200, ms: 182, bytes: 312, value: "21.0", formatted: "21C", error: null, preview: '{\n  "latitude": 36.72,\n  "longitude": -4.42,\n  "current": {\n    "time": "2026-09-24T12:00",\n    "temperature_2m": 21.0\n  }\n}' }),
  icon_preview: () => ({ frames: [Array.from({ length: 64 }, (_, i) => sun[(i * 7) % 8] || (i % 9 === 0 ? "#FF7A3D" : ""))], delays: [1000] }),
  list_timezones: () => ["UTC", "Europe/London", "Europe/Madrid", "America/New_York"],
  log_tail: () => log,
};

export const mock = {
  async invoke<T>(cmd: string, args: Record<string, unknown> = {}): Promise<T> {
    await new Promise((r) => setTimeout(r, 30));
    const h = handlers[cmd];
    return (h ? h(args) : undefined) as T;
  },
  async listen<T>(_event: string, _cb: (p: T) => void) {
    return () => {};
  },
};
