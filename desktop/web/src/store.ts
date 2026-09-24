import { create } from "zustand";
import { api, errText, on, type IconPreview, type Settings, type Source, type Status } from "./api";

export type Tab = "playlist" | "sources" | "device" | "log";

export interface LogLine { t: string; lv: "info" | "warn" | "error"; m: string }

interface Store {
  ready: boolean;
  tab: Tab;
  status: Status | null;
  settings: Settings | null;
  sources: Source[];
  autostart: boolean;
  macTz: string | null;
  appsPath: string;
  version: string;
  log: LogLine[];
  toast: { text: string; bad?: boolean } | null;
  /** open the source editor (index, "new" or null) — shared by Playlist and Sources */
  editing: number | "new" | null;
  setTab(t: Tab): void;
  edit(e: number | "new" | null): void;
  flash(text: string, bad?: boolean): void;
  /** save apps.json with optimistic UI; rolls back and shows the error if it fails */
  saveSources(next: Source[]): Promise<boolean>;
  saveSettings(next: Settings): Promise<boolean>;
  setAutostart(on: boolean): Promise<void>;
}

export function parseLog(line: string): LogLine {
  const m = /^(?:\d{4}-\d{2}-\d{2}[ T])?(\d{2}:\d{2}:\d{2})\S*\s+(INFO|WARN|ERROR)\s+(.*)$/.exec(line);
  if (!m) return { t: "", lv: "info", m: line };
  return { t: m[1], lv: m[2].toLowerCase() as LogLine["lv"], m: m[3] };
}

let toastTimer: number | undefined;

export const useStore = create<Store>((set, get) => ({
  ready: false,
  tab: "playlist",
  status: null,
  settings: null,
  sources: [],
  autostart: false,
  macTz: null,
  appsPath: "",
  version: "",
  log: [],
  toast: null,
  editing: null,
  setTab: (tab) => set({ tab }),
  edit: (editing) => set({ editing }),
  flash(text, bad) {
    clearTimeout(toastTimer);
    set({ toast: { text, bad } });
    toastTimer = window.setTimeout(() => set({ toast: null }), 2800);
  },
  async saveSources(next) {
    const prev = get().sources;
    set({ sources: next });
    try {
      const sent = await api.saveApps(next);
      if (!sent) get().flash("Saved — will be sent when the clock reconnects");
      return true;
    } catch (e) {
      set({ sources: prev });
      get().flash(errText(e), true);
      return false;
    }
  },
  async saveSettings(next) {
    const prev = get().settings;
    set({ settings: next });
    try {
      await api.saveSettings(next);
      return true;
    } catch (e) {
      set({ settings: prev });
      get().flash(errText(e), true);
      return false;
    }
  },
  async setAutostart(v) {
    const prev = get().autostart;
    set({ autostart: v });
    try {
      await api.setAutostart(v);
    } catch (e) {
      set({ autostart: prev });
      get().flash(errText(e), true);
    }
  },
}));

export async function boot() {
  const st = await api.getState();
  const [sources, log] = await Promise.all([api.getApps().catch(() => []), api.logTail().catch(() => [])]);
  useStore.setState({
    ready: true,
    status: st.status,
    settings: st.settings,
    autostart: st.autostart,
    macTz: st.mac_tz,
    appsPath: st.apps_path,
    version: st.version,
    sources,
    log: log.map(parseLog).reverse(),
  });

  let rev = st.status.apps_rev;
  await on<Status>("status", async (s) => {
    useStore.setState({ status: s });
    if (s.apps_rev !== rev) {
      rev = s.apps_rev; // apps.json changed (local API, another save) → reload
      useStore.setState({ sources: await api.getApps() });
    }
  });
  await on<string>("log", (line) => useStore.setState((x) => ({ log: [parseLog(line), ...x.log].slice(0, 1000) })));
  await on<Settings>("settings", (settings) => useStore.setState({ settings }));
  await on<boolean>("autostart", (autostart) => useStore.setState({ autostart }));

  // "replied N s ago" needs a fresh reading even when nothing else changes
  window.setInterval(async () => {
    if (document.visibilityState !== "visible") return;
    try {
      useStore.setState({ status: await api.getStatus() });
    } catch {
      /* ignore */
    }
  }, 2000);
}

// ---- LaMetric icon previews, shared cache ----
const icons = new Map<string, Promise<IconPreview>>();
export function iconPreview(id: string): Promise<IconPreview> {
  id = id.trim();
  let p = icons.get(id);
  if (!p) {
    p = api.iconPreview(id);
    p.catch(() => icons.delete(id));
    icons.set(id, p);
  }
  return p;
}
