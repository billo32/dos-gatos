// Bridge to the Rust core. All device and network I/O lives in Rust; the UI only calls
// commands and listens to events. Outside Tauri (vite dev in a browser) a mock backend is used.
import { mock } from "./mock";

export type FontId = "3x5" | "4x6" | "5x7";

export interface AppValue {
  value: string | null;
  status: number;
  at: string;
  error: string | null;
}

export interface WifiInfo {
  ssid?: string;
  state?: "off" | "connecting" | "connected" | string;
  ip?: string;
  rssi?: number;
}

export interface Status {
  connected: boolean;
  port: string | null;
  fw: string | null;
  last_rx_ago: number | null;
  rtc: string | null;
  values: Record<string, AppValue>;
  api_error: string | null;
  apps_rev: number;
  font: FontId | null;
  wifi: WifiInfo | null;
  screen: string | null;
}

export interface ClockScreen { on: boolean; dur: number; pos: number; h24: boolean; wday: boolean }
export interface NotifyScreen { on: boolean; dur: number; text: string; color: string; icon: string }
export interface Settings { clock: ClockScreen; notify: NotifyScreen; brightness: number; tz: string | null }

/** One entry of apps.json (existing format; `dur`/`off` are the playlist flags). */
export interface Source {
  name: string;
  url: string;
  every?: number;
  path?: string;
  find?: string;
  keep?: number;
  re?: string;
  fmt?: string;
  color?: string;
  scale?: number;
  dec?: number;
  icon?: string;
  font?: FontId | "";
  dur?: number;
  off?: boolean;
}

export interface AppState {
  status: Status;
  settings: Settings;
  autostart: boolean;
  mac_tz: string | null;
  apps_path: string;
  version: string;
}

export interface TestResult {
  status: number;
  ms: number;
  bytes: number;
  value: string | null;
  formatted: string | null;
  error: string | null;
  preview: string;
}

export interface IconPreview { frames: string[][]; delays: number[] }

type Unlisten = () => void;

interface Backend {
  invoke<T>(cmd: string, args?: Record<string, unknown>): Promise<T>;
  listen<T>(event: string, cb: (payload: T) => void): Promise<Unlisten>;
}

const inTauri = typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;

let tauriBackend: Promise<Backend> | null = null;
function backend(): Promise<Backend> {
  if (!inTauri) return Promise.resolve(mock);
  tauriBackend ??= Promise.all([import("@tauri-apps/api/core"), import("@tauri-apps/api/event")]).then(([core, ev]) => ({
    invoke: <T,>(cmd: string, args?: Record<string, unknown>) => core.invoke<T>(cmd, args),
    listen: <T,>(event: string, cb: (p: T) => void) => ev.listen<T>(event, (e) => cb(e.payload)),
  }));
  return tauriBackend;
}

const call = async <T,>(cmd: string, args?: Record<string, unknown>) => (await backend()).invoke<T>(cmd, args);

export const api = {
  getState: () => call<AppState>("get_state"),
  getStatus: () => call<Status>("get_status"),
  getApps: () => call<Source[]>("get_apps"),
  saveApps: (apps: Source[]) => call<boolean>("save_apps", { apps }),
  saveSettings: (settings: Settings) => call<boolean>("save_settings", { settings }),
  testSource: (app: Source) => call<TestResult>("test_source", { app }),
  notify: (text: string, color: string, icon: string | null) => call<void>("notify", { text, color, icon }),
  iconPreview: (id: string) => call<IconPreview>("icon_preview", { id }),
  setFont: (font: FontId) => call<void>("set_font", { font }),
  wifiSet: (ssid: string, password: string) => call<void>("wifi_set", { ssid, password }),
  wifiOff: () => call<void>("wifi_off"),
  restartClock: () => call<void>("restart_clock"),
  reconnect: () => call<void>("reconnect"),
  listTimezones: () => call<string[]>("list_timezones"),
  logTail: () => call<string[]>("log_tail"),
  logClear: () => call<void>("log_clear"),
  logExport: () => call<string | null>("log_export"),
  revealApps: () => call<void>("reveal_apps"),
  openIconGallery: () => call<void>("open_icon_gallery"),
  setAutostart: (enabled: boolean) => call<void>("set_autostart", { enabled }),
};

export async function on<T>(event: string, cb: (payload: T) => void): Promise<Unlisten> {
  return (await backend()).listen<T>(event, cb);
}

export const errText = (e: unknown) => (e instanceof Error ? e.message : String(e));
