import { useStore, type Tab } from "./store";
import { cx } from "./components/ui";
import { Playlist } from "./screens/Playlist";
import { Sources } from "./screens/Sources";
import { Device } from "./screens/Device";
import { Log } from "./screens/Log";
import { SourceEditor } from "./screens/SourceEditor";
import type { Status } from "./api";

const NAV: { id: Tab; label: string; icon: string }[] = [
  { id: "playlist", label: "Playlist", icon: "M4 6h16M4 12h16M4 18h10" },
  { id: "sources", label: "Sources", icon: "M4 7c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3zM4 7v5c0 1.7 3.6 3 8 3s8-1.3 8-3V7M4 12v5c0 1.7 3.6 3 8 3s8-1.3 8-3v-5" },
  // Extensions (design/AppExtensions.dc.html) is hidden until there is a catalog to install from.
  { id: "device", label: "Device", icon: "M3 8h18v8H3zM7 12h.01M11 12h6" },
  { id: "log", label: "Log", icon: "M5 4h14v16H5zM9 8h6M9 12h6M9 16h4" },
];

export function ago(s: number | null | undefined) {
  if (s == null) return "";
  return s < 60 ? `${s} s ago` : `${Math.floor(s / 60)} min ago`;
}

export function linkState(st: Status | null): "usb" | "wifi" | "waiting" | "off" {
  if (st?.connected && st.fw) return "usb";
  if (st?.connected) return "waiting";
  return "off";
}

function DeviceCard() {
  const st = useStore((s) => s.status);
  const state = linkState(st);
  const badge = {
    usb: { text: "USB", dot: "bg-ok", fg: "text-ok-text" },
    waiting: { text: "USB", dot: "bg-[#EDB54A]", fg: "text-muted" },
    wifi: { text: "Wi‑Fi", dot: "bg-[#4DA3FF]", fg: "text-muted" },
    off: { text: "Offline", dot: "bg-toggle-off", fg: "text-subtle" },
  }[state];
  return (
    <div className="flex flex-col gap-[5px] rounded-[10px] border border-line bg-white p-3">
      <div className="flex items-center justify-between">
        <div className="text-[14px] font-semibold">TC001</div>
        <div className={cx("flex items-center gap-1.5 text-[12px] font-medium", badge.fg)}>
          <span className={cx("size-2 rounded-full", badge.dot)} />
          {badge.text}
        </div>
      </div>
      <div className="truncate font-mono text-[11px] text-muted">{st?.port ?? "no cable"}</div>
      <div className="text-[12px] text-subtle">
        {state === "usb" ? `fw ${st!.fw} · replied ${ago(st!.last_rx_ago)}` : state === "waiting" ? "waiting for the clock…" : "Plug in the USB cable"}
      </div>
    </div>
  );
}

export function App() {
  const { tab, setTab, editing, toast, ready } = useStore();
  return (
    <div className="relative flex h-full flex-col overflow-hidden bg-white">
      <div data-tauri-drag-region className="flex h-11 shrink-0 items-center border-b border-line bg-titlebar px-4">
        <div data-tauri-drag-region className="grow pl-[70px] pr-[70px] text-center text-[13px] font-medium text-muted">Dos GatOS</div>
      </div>
      <div className="flex min-h-0 grow">
        <aside className="box-border flex w-[220px] shrink-0 flex-col gap-4 border-r border-line bg-milk px-3 py-4">
          <DeviceCard />
          <nav aria-label="Sections" className="flex flex-col gap-0.5">
            {NAV.map((n) => (
              <button
                key={n.id}
                type="button"
                aria-current={tab === n.id ? "page" : undefined}
                onClick={() => setTab(n.id)}
                className={cx("flex h-[38px] items-center gap-2.5 rounded-lg border-0 px-2.5 text-left text-[14px] text-ink", tab === n.id ? "bg-nav-active font-semibold" : "bg-transparent font-normal hover:bg-sand-2")}
              >
                <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d={n.icon} /></svg>
                {n.label}
              </button>
            ))}
          </nav>
        </aside>
        <main className={cx("box-border flex min-w-0 grow flex-col gap-[18px] px-7 py-[22px]", tab === "device" ? "bg-milk" : "bg-white")}>
          {ready && tab === "playlist" && <Playlist />}
          {ready && tab === "sources" && <Sources />}
          {ready && tab === "device" && <Device />}
          {ready && tab === "log" && <Log />}
        </main>
      </div>
      {editing !== null && <SourceEditor key={String(editing)} />}
      {toast && (
        <div role="status" className={cx("absolute bottom-5 left-1/2 z-20 -translate-x-1/2 rounded-[10px] px-4 py-2.5 text-[13px] font-medium shadow-[0_8px_24px_rgba(28,26,23,0.18)]", toast.bad ? "bg-danger text-white" : "bg-ink text-milk")}>
          {toast.text}
        </div>
      )}
    </div>
  );
}
