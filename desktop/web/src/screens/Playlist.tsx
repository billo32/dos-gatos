import { useEffect, useRef, useState } from "react";
import { api, errText, type IconPreview, type Settings, type Source } from "../api";
import { iconPreview, useStore } from "../store";
import { PixelClock } from "../clock/PixelClock";
import { formatValue, type Scene } from "../clock/render";
import { Button, Eyebrow, Field, GearButton, Input, Modal, PageHeader, Swatches, Switch, cx } from "../components/ui";

// Playlist = the clock screen + every source (apps.json order) + notifications (an interrupt, always last).
type Item = { kind: "clock" } | { kind: "source"; i: number } | { kind: "notify" };
const keyOf = (it: Item, sources: Source[]) => (it.kind === "source" ? `s:${sources[it.i].name}` : it.kind);

function rotation(sources: Source[], clockPos: number): Item[] {
  const items: Item[] = sources.map((_, i) => ({ kind: "source", i }));
  items.splice(Math.min(Math.max(clockPos, 0), items.length), 0, { kind: "clock" });
  return items;
}

export function useIcon(id: string | undefined): IconPreview | null {
  const [icon, setIcon] = useState<IconPreview | null>(null);
  useEffect(() => {
    let alive = true;
    setIcon(null);
    const v = id?.trim();
    if (v && /^[A-Za-z0-9_-]{1,16}$/.test(v)) iconPreview(v).then((p) => alive && setIcon(p), () => {});
    return () => {
      alive = false;
    };
  }, [id]);
  return icon;
}

export function sourceText(src: Source, raw: string | null | undefined): string {
  return raw == null ? "--" : formatValue(raw, src.fmt, src.scale, src.dec);
}

function Preview({ item, name, following }: { item: Item; name: string; following: boolean }) {
  const { sources, settings, status } = useStore();
  const src = item.kind === "source" ? sources[item.i] : null;
  const icon = useIcon(item.kind === "source" ? src?.icon : item.kind === "notify" ? settings!.notify.icon : undefined);
  const font = status?.font ?? "3x5";
  let scene: Scene;
  if (item.kind === "clock") scene = { kind: "clock", font, h24: settings!.clock.h24, wday: settings!.clock.wday };
  else if (item.kind === "notify") scene = { kind: "text", text: settings!.notify.text || "Hello", color: settings!.notify.color, font, icon };
  else {
    const raw = status?.values[src!.name]?.value;
    scene = { kind: "text", text: sourceText(src!, raw), color: raw == null ? "#555555" : src!.color || "#FFFFFF", font: (src!.font || font) as typeof font, icon };
  }
  return (
    <div className="flex shrink-0 items-center gap-6 rounded-xl bg-sand px-[18px] py-4">
      <PixelClock scene={scene} scale={9} body="white" label={`Clock screen: ${name}`} />
      <div className="flex min-w-0 flex-col gap-1.5">
        <Eyebrow>{following ? "On the clock now" : "Preview"}</Eyebrow>
        <div className="truncate text-[18px] font-semibold">{name}</div>
        <div className="text-[13px] text-subtle">{following ? "Click a screen to preview it" : "Click it again to follow the clock"}</div>
      </div>
    </div>
  );
}

export function Playlist() {
  const { sources, settings, status, saveSources, saveSettings, edit } = useStore();
  const s = settings!;
  const rot = rotation(sources, s.clock.pos);
  const items: Item[] = [...rot, { kind: "notify" }];
  const [picked, setPicked] = useState<string | null>(null);
  const [modal, setModal] = useState<string | null>(null);
  const [drag, setDrag] = useState<{ from: number; to: number; dy: number } | null>(null);
  const listRef = useRef<HTMLDivElement>(null);

  const nameOf = (it: Item) => (it.kind === "clock" ? "Clock" : it.kind === "notify" ? "Notifications" : sources[it.i].name);
  const colorOf = (it: Item) => (it.kind === "clock" ? "#FFF1DC" : it.kind === "notify" ? s.notify.color : sources[it.i].color || "#FFFFFF");
  const durOf = (it: Item) => (it.kind === "clock" ? s.clock.dur : it.kind === "notify" ? s.notify.dur : sources[it.i].dur ?? 8);
  const isOn = (it: Item) => (it.kind === "clock" ? s.clock.on : it.kind === "notify" ? s.notify.on : !sources[it.i].off);

  // the clock reports what it shows; follow it until the user picks a row
  const deviceKey = status?.screen ? (status.screen === "clock" ? "clock" : `s:${status.screen}`) : null;
  const selKey = picked ?? deviceKey ?? keyOf(items[0], sources);
  const sel = items.find((it) => keyOf(it, sources) === selKey) ?? items[0];
  const following = picked === null;

  const setOn = (it: Item, on: boolean) => {
    if (it.kind === "clock") saveSettings({ ...s, clock: { ...s.clock, on } });
    else if (it.kind === "notify") saveSettings({ ...s, notify: { ...s.notify, on } });
    else saveSources(sources.map((x, j) => (j === it.i ? { ...x, off: !on || undefined } : x)));
  };

  /** Move rotation row `from` to `to` (indexes in `rot`): new apps.json order + clock position. */
  const move = (from: number, to: number) => {
    if (from === to || to < 0 || to >= rot.length) return;
    const order = [...rot];
    const [it] = order.splice(from, 1);
    order.splice(to, 0, it);
    const next = order.filter((x): x is { kind: "source"; i: number } => x.kind === "source").map((x) => sources[x.i]);
    const pos = order.findIndex((x) => x.kind === "clock");
    if (next.some((x, j) => x !== sources[j])) saveSources(next);
    if (pos !== s.clock.pos) saveSettings({ ...s, clock: { ...s.clock, pos } });
  };

  const startDrag = (e: React.PointerEvent, from: number) => {
    e.preventDefault();
    const y0 = e.clientY;
    const el = e.currentTarget as HTMLElement;
    el.setPointerCapture(e.pointerId);
    let to = from;
    const onMove = (ev: PointerEvent) => {
      const dy = ev.clientY - y0;
      to = Math.min(rot.length - 1, Math.max(0, from + Math.round(dy / 50)));
      setDrag({ from, to, dy });
    };
    const onUp = () => {
      el.removeEventListener("pointermove", onMove);
      el.removeEventListener("pointerup", onUp);
      el.removeEventListener("pointercancel", onUp);
      setDrag(null);
      move(from, to);
    };
    el.addEventListener("pointermove", onMove);
    el.addEventListener("pointerup", onUp);
    el.addEventListener("pointercancel", onUp);
    setDrag({ from, to, dy: 0 });
  };

  const onRowKey = (e: React.KeyboardEvent, idx: number) => {
    if (!e.altKey || (e.key !== "ArrowUp" && e.key !== "ArrowDown") || idx >= rot.length) return;
    e.preventDefault();
    const to = idx + (e.key === "ArrowUp" ? -1 : 1);
    move(idx, to);
    requestAnimationFrame(() => listRef.current?.querySelectorAll<HTMLElement>("[data-pick]")[Math.min(Math.max(to, 0), rot.length - 1)]?.focus());
  };

  const shift = (idx: number) => {
    if (!drag || idx >= rot.length) return 0;
    if (idx === drag.from) return drag.dy;
    if (drag.from < drag.to && idx > drag.from && idx <= drag.to) return -50;
    if (drag.from > drag.to && idx < drag.from && idx >= drag.to) return 50;
    return 0;
  };

  const onCount = items.filter(isOn).length;
  const modalItem = items.find((it) => keyOf(it, sources) === modal);

  return (
    <>
      <PageHeader title="Playlist" sub={`${onCount} of ${items.length} screens on · drag to reorder`}>
        <Button kind="primary" onClick={() => edit("new")}>+ Add screen</Button>
      </PageHeader>
      <Preview item={sel} name={nameOf(sel)} following={following} />
      <div ref={listRef} className="flex min-h-0 shrink flex-col overflow-y-auto rounded-xl border border-line">
        {items.map((it, idx) => {
          const k = keyOf(it, sources);
          const on = isOn(it);
          const movable = it.kind !== "notify";
          const color = colorOf(it);
          return (
            <div
              key={k}
              onKeyDown={(e) => onRowKey(e, idx)}
              className={cx("relative box-border flex h-[50px] shrink-0 items-center gap-2.5 border-b border-line-soft pr-3 pl-2 last:border-b-0", k === selKey ? "bg-select" : "bg-white", drag?.from === idx && "z-10 shadow-[0_6px_16px_rgba(28,26,23,0.14)]", drag && drag.from !== idx && "transition-transform duration-150")}
              style={{ transform: `translateY(${shift(idx)}px)` }}
            >
              <div
                onPointerDown={movable ? (e) => startDrag(e, idx) : undefined}
                className={cx("flex h-full w-4 items-center", movable ? "cursor-grab active:cursor-grabbing" : "invisible")}
                aria-hidden="true"
                title={movable ? "Drag to reorder (⌥↑ / ⌥↓)" : undefined}
              >
                <svg viewBox="0 0 24 24" width="16" height="16" fill="#B7AFA3"><circle cx="9" cy="6" r="1.6" /><circle cx="15" cy="6" r="1.6" /><circle cx="9" cy="12" r="1.6" /><circle cx="15" cy="12" r="1.6" /><circle cx="9" cy="18" r="1.6" /><circle cx="15" cy="18" r="1.6" /></svg>
              </div>
              <button
                type="button"
                data-pick
                onClick={() => setPicked(picked === k ? null : k)}
                className="flex h-11 min-w-0 grow items-center gap-3 border-0 bg-transparent p-0 text-left text-inherit"
              >
                <span className="flex size-7 shrink-0 items-center justify-center rounded-[7px] bg-ink">
                  <span className="size-2.5 rounded-[2px]" style={{ background: color, boxShadow: `0 0 6px ${color}`, opacity: on ? 1 : 0.35 }} />
                </span>
                <span className={cx("truncate text-[14px] font-semibold", !on && "text-subtle")}>{nameOf(it)}</span>
                <span className="shrink-0 text-[12px] text-subtle">{it.kind === "notify" ? `${durOf(it)} s · on demand` : `${durOf(it)} s`}</span>
              </button>
              <GearButton name={nameOf(it)} onClick={() => { setPicked(k); setModal(k); }} />
              <Switch on={on} onChange={(v) => setOn(it, v)} label={`Show ${nameOf(it)}`} />
            </div>
          );
        })}
      </div>
      {modalItem && <ItemSettings item={modalItem} name={nameOf(modalItem)} onClose={() => setModal(null)} />}
    </>
  );
}

function ItemSettings({ item, name, onClose }: { item: Item; name: string; onClose: () => void }) {
  const { settings, sources, saveSettings, saveSources, edit, flash } = useStore();
  const s = settings!;
  const src = item.kind === "source" ? sources[item.i] : null;
  const initialDur = item.kind === "clock" ? s.clock.dur : item.kind === "notify" ? s.notify.dur : src!.dur ?? 8;
  const [dur, setDur] = useState(String(initialDur));
  const [h24, setH24] = useState(s.clock.h24);
  const [wday, setWday] = useState(s.clock.wday);
  const [text, setText] = useState(s.notify.text);
  const [color, setColor] = useState(s.notify.color);
  const [icon, setIcon] = useState(s.notify.icon);
  const [confirm, setConfirm] = useState(false);
  const [sending, setSending] = useState(false);

  const d = Math.round(Number(dur));
  const durErr = !Number.isFinite(d) || d < 2 || d > 600 ? "2–600 seconds" : null;

  const next = (): Settings => {
    if (item.kind === "clock") return { ...s, clock: { ...s.clock, dur: d, h24, wday } };
    if (item.kind === "notify") return { ...s, notify: { ...s.notify, dur: d, text, color, icon: icon.trim() } };
    return s;
  };

  const save = async () => {
    if (durErr) return;
    const ok = item.kind === "source" ? await saveSources(sources.map((x, j) => (j === item.i ? { ...x, dur: d } : x))) : await saveSettings(next());
    if (ok) onClose();
  };

  const remove = async () => {
    if (!confirm) return setConfirm(true);
    if (item.kind === "source" && (await saveSources(sources.filter((_, j) => j !== item.i)))) onClose();
  };

  const test = async () => {
    setSending(true);
    try {
      await api.notify(text || "Hello", color, icon.trim() || null);
      await saveSettings(next());
    } catch (e) {
      flash(errText(e), true);
    } finally {
      setSending(false);
    }
  };

  return (
    <Modal
      title={`${name} settings`}
      onClose={onClose}
      onSubmit={save}
      footer={
        <>
          {item.kind === "source" ? (
            <Button kind="danger" onClick={remove}>{confirm ? `Remove ${name}? Click again` : "Remove from playlist"}</Button>
          ) : (
            <span />
          )}
          <div className="flex gap-2">
            <Button onClick={onClose}>Cancel</Button>
            <Button kind="primary" onClick={save} disabled={!!durErr}>Save</Button>
          </div>
        </>
      }
    >
      <div className="flex items-center justify-between gap-3">
        <label htmlFor="pl-dur" className="text-[14px] font-medium">Show for</label>
        <div className="flex items-center gap-2">
          <Input id="pl-dur" type="number" min={2} max={600} value={dur} onChange={(e) => setDur(e.target.value)} invalid={!!durErr} className="!w-[84px]" title={durErr ?? undefined} />
          <span className="text-[13px] text-subtle">s</span>
        </div>
      </div>
      {item.kind === "clock" && (
        <>
          <ToggleRow label="24-hour format" on={h24} onChange={setH24} />
          <ToggleRow label="Show weekday bar" on={wday} onChange={setWday} />
        </>
      )}
      {item.kind === "source" && (
        <>
          <div className="text-[13px] leading-[1.5] text-subtle">
            Polled every {src!.every ?? 300} s from <span className="font-mono text-[12px]">{hostOf(src!.url)}</span>.
          </div>
          <Button kind="link" className="self-start" onClick={() => { onClose(); edit(item.i); }}>Edit source…</Button>
        </>
      )}
      {item.kind === "notify" && (
        <>
          <div className="text-[13px] leading-[1.5] text-subtle">
            Notifications interrupt the playlist. Scripts send them with{" "}
            <span className="font-mono text-[12px] text-ink">curl -X POST 127.0.0.1:7765/notify -d '{"{"}"text":"Deploy OK"{"}"}'</span>
          </div>
          <Field label="Test message">{(id) => <Input id={id} value={text} onChange={(e) => setText(e.target.value)} />}</Field>
          <div className="grid grid-cols-[minmax(0,1fr)_120px] gap-3">
            <div className="flex flex-col gap-1.5">
              <div className="text-[13px] font-semibold">Color</div>
              <Swatches value={color} onChange={setColor} />
            </div>
            <Field label="Icon">{(id) => <Input id={id} mono value={icon} placeholder="e.g. 2422" onChange={(e) => setIcon(e.target.value)} />}</Field>
          </div>
          <Button className="self-start" onClick={test} disabled={sending}>{sending ? "Sending…" : "Send test to the clock"}</Button>
        </>
      )}
    </Modal>
  );
}

function ToggleRow({ label, on, onChange }: { label: string; on: boolean; onChange: (v: boolean) => void }) {
  return (
    <div className="flex items-center justify-between gap-3">
      <div className="text-[14px] font-medium">{label}</div>
      <Switch on={on} onChange={onChange} label={label} />
    </div>
  );
}

export const hostOf = (url: string) => url.replace(/^https?:\/\//, "").split("/")[0];
