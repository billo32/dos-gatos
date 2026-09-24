import { useMemo, useState } from "react";
import { api, errText, type FontId, type Source, type TestResult } from "../api";
import { useStore } from "../store";
import { PixelClock } from "../clock/PixelClock";
import { formatValue } from "../clock/render";
import { Button, Field, Input, Modal, Select, Swatches } from "../components/ui";
import { useIcon } from "./Playlist";

interface Form {
  name: string; every: string; url: string; path: string; fmt: string; color: string;
  find: string; keep: string; re: string; scale: string; dec: string; icon: string; font: string;
}

const toForm = (s?: Source): Form => ({
  name: s?.name ?? "",
  every: String(s?.every ?? 600),
  url: s?.url ?? "",
  path: s?.path ?? "",
  fmt: s?.fmt ?? "{v}",
  color: (s?.color ?? "#FFF1DC").toUpperCase(),
  find: s?.find ?? "",
  keep: s?.keep != null ? String(s.keep) : "",
  re: s?.re ?? "",
  scale: s?.scale != null && s.scale !== 1 ? String(s.scale) : "",
  dec: s?.dec != null && s.dec >= 0 ? String(s.dec) : "",
  icon: s?.icon ?? "",
  font: s?.font ?? "",
});

/** Form → apps.json entry. Unknown keys of the original entry are kept; empty optional fields are dropped. */
function toSource(f: Form, base?: Source): Source {
  const out: Source = { ...(base ?? ({} as Source)), name: f.name.trim(), url: f.url.trim(), every: Math.round(Number(f.every)) };
  const opt = <K extends keyof Source>(k: K, v: Source[K] | undefined) => {
    if (v === undefined || v === "") delete out[k];
    else out[k] = v;
  };
  opt("path", f.path.trim() || undefined);
  opt("fmt", f.fmt && f.fmt !== "{v}" ? f.fmt : undefined);
  opt("color", f.color);
  opt("find", f.find || undefined);
  opt("keep", f.keep.trim() ? Math.round(Number(f.keep)) : undefined);
  opt("re", f.re || undefined);
  opt("scale", f.scale.trim() && Number(f.scale) !== 1 ? Number(f.scale) : undefined);
  opt("dec", f.dec.trim() ? Math.round(Number(f.dec)) : undefined);
  opt("icon", f.icon.trim() || undefined);
  opt("font", (f.font || undefined) as FontId | undefined);
  return out;
}

function validate(f: Form, others: Source[]): Partial<Record<keyof Form, string>> {
  const e: Partial<Record<keyof Form, string>> = {};
  if (!f.name.trim()) e.name = "Required";
  else if (others.some((o) => o.name === f.name.trim())) e.name = "Already used";
  if (!/^https?:\/\/\S+$/i.test(f.url.trim())) e.url = "Must start with http:// or https://";
  const ev = Number(f.every);
  if (!Number.isFinite(ev) || ev < 5) e.every = "At least 5 s";
  if (f.keep.trim() && !(Number(f.keep) >= 1)) e.keep = "A positive number";
  if (f.scale.trim() && !Number.isFinite(Number(f.scale))) e.scale = "A number";
  if (f.dec.trim() && !(Number(f.dec) >= 0 && Number(f.dec) <= 6)) e.dec = "0–6, empty = as is";
  if (f.icon.trim() && !/^[A-Za-z0-9_-]{1,16}$/.test(f.icon.trim())) e.icon = "A LaMetric icon ID, e.g. 2422";
  if (f.re) {
    try {
      new RegExp(f.re);
    } catch (err) {
      e.re = errText(err).replace(/^Invalid regular expression: /, "");
    }
  }
  return e;
}

/** Line of the response to highlight: where the extracted value (or the Find text) is. */
function hiLine(lines: string[], f: Form, value: string | null): number {
  const needles = [f.find, value, f.path.split(".").filter((p) => !/^\d+$/.test(p)).pop()].filter((x): x is string => !!x);
  for (const n of needles) {
    const i = lines.findIndex((l) => l.includes(n));
    if (i >= 0) return i;
  }
  return -1;
}

export function SourceEditor() {
  const { editing, sources, status, saveSources, edit, flash } = useStore();
  const idx = typeof editing === "number" ? editing : -1;
  const base = idx >= 0 ? sources[idx] : undefined;
  const [f, setF] = useState<Form>(() => toForm(base));
  const [adv, setAdv] = useState(() => !!(base && (base.find || base.re || base.keep != null || (base.scale ?? 1) !== 1 || base.icon || base.font || (base.dec ?? -1) >= 0)));
  const [touched, setTouched] = useState(false);
  const [test, setTest] = useState<TestResult | null>(null);
  const [testing, setTesting] = useState(false);
  const [confirm, setConfirm] = useState(false);
  const errors = useMemo(() => validate(f, sources.filter((_, i) => i !== idx)), [f, sources, idx]);
  const bad = Object.keys(errors).length > 0;
  const set = (k: keyof Form) => (e: React.ChangeEvent<HTMLInputElement | HTMLSelectElement>) => setF({ ...f, [k]: e.target.value });
  const err = (k: keyof Form) => (touched || k === "re" ? errors[k] : undefined);

  const close = () => edit(null);
  const icon = useIcon(f.icon);
  const raw = test ? test.value : base ? status?.values[base.name]?.value ?? null : null;
  const text = raw == null ? "--" : formatValue(raw, f.fmt, f.scale.trim() ? Number(f.scale) : 1, f.dec.trim() ? Number(f.dec) : -1);
  const font = (f.font || status?.font || "3x5") as FontId;

  const save = async () => {
    setTouched(true);
    if (bad) return;
    const src = toSource(f, base);
    const next = idx >= 0 ? sources.map((s, i) => (i === idx ? src : s)) : [...sources, { ...src, dur: src.dur ?? 8 }];
    if (await saveSources(next)) {
      flash(idx >= 0 ? `${src.name} saved` : `${src.name} added to the playlist`);
      close();
    }
  };

  const del = async () => {
    if (!confirm) return setConfirm(true);
    if (await saveSources(sources.filter((_, i) => i !== idx))) close();
  };

  const runTest = async () => {
    const e = validate(f, []);
    if (e.url || e.re) {
      setTouched(true);
      return;
    }
    setTesting(true);
    try {
      setTest(await api.testSource(toSource(f, base)));
    } catch (e) {
      flash(errText(e), true);
    } finally {
      setTesting(false);
    }
  };

  const lines = test ? test.preview.split("\n") : [];
  const hi = test ? hiLine(lines, f, test.value) : -1;

  return (
    <Modal
      title={idx >= 0 ? base!.name : "New source"}
      width={520}
      onClose={close}
      onSubmit={save}
      footer={
        <>
          {idx >= 0 ? <Button kind="danger" onClick={del}>{confirm ? "Delete? Click again" : "Delete"}</Button> : <span />}
          <div className="flex gap-2">
            <Button onClick={runTest} disabled={testing}>{testing ? "Testing…" : "Test"}</Button>
            <Button kind="primary" onClick={save} disabled={touched && bad}>Save &amp; send to clock</Button>
          </div>
        </>
      }
    >
      <div className="flex h-[110px] shrink-0 items-center justify-center rounded-xl bg-sand">
        <PixelClock scene={{ kind: "text", text, color: raw == null ? "#555555" : f.color, font, icon }} scale={7} body="white" label="Source preview" />
      </div>
      <div className="grid grid-cols-[minmax(0,1fr)_110px] gap-3">
        <Field label="Name" error={err("name")}>{(id) => <Input id={id} value={f.name} onChange={set("name")} invalid={!!err("name")} />}</Field>
        <Field label="Refresh, s" error={err("every")}>{(id) => <Input id={id} type="number" min={5} value={f.every} onChange={set("every")} invalid={!!err("every")} />}</Field>
      </div>
      <Field label="URL" error={err("url")}>{(id) => <Input id={id} mono value={f.url} onChange={set("url")} invalid={!!err("url")} placeholder="https://" />}</Field>
      <Field label="JSON path">{(id) => <Input id={id} mono value={f.path} onChange={set("path")} placeholder="e.g. current.temperature_2m" />}</Field>
      <div className="grid grid-cols-[140px_minmax(0,1fr)] gap-3">
        <Field label="Format">{(id) => <Input id={id} mono value={f.fmt} onChange={set("fmt")} />}</Field>
        <div className="flex min-w-0 flex-col gap-1.5">
          <div className="text-[13px] font-semibold">Color</div>
          <Swatches value={f.color} onChange={(color) => setF({ ...f, color })} />
        </div>
      </div>
      <Button kind="link" className="self-start" aria-expanded={adv} onClick={() => setAdv(!adv)}>{adv ? "Hide advanced" : "Advanced: find, regex, math, icon"}</Button>
      {adv && (
        <div className="grid grid-cols-2 gap-3 rounded-xl bg-milk p-3.5">
          <Field label="Find">{(id) => <Input id={id} mono value={f.find} onChange={set("find")} />}</Field>
          <Field label="Keep, chars" error={err("keep")}>{(id) => <Input id={id} type="number" min={1} value={f.keep} placeholder="128" onChange={set("keep")} />}</Field>
          <Field label="Regex" error={err("re")}>{(id) => <Input id={id} mono value={f.re} onChange={set("re")} invalid={!!errors.re} />}</Field>
          <Field label="Multiply by" error={err("scale")}>{(id) => <Input id={id} type="number" step="any" value={f.scale} placeholder="1" onChange={set("scale")} />}</Field>
          <Field label="Decimals" error={err("dec")}>{(id) => <Input id={id} type="number" min={0} max={6} value={f.dec} placeholder="as is" onChange={set("dec")} />}</Field>
          <Field label="Icon (LaMetric ID)" error={err("icon")} hint={<button type="button" className="border-0 bg-transparent p-0 text-[12px] text-ginger-text" onClick={() => api.openIconGallery()}>Browse icons</button>}>
            {(id) => <Input id={id} mono value={f.icon} onChange={set("icon")} placeholder="e.g. 2422" />}
          </Field>
          <Field label="Font">
            {(id) => (
              <Select id={id} value={f.font} onChange={set("font")}>
                <option value="">Device default</option>
                <option value="3x5">Blocky 3×5</option>
                <option value="4x6">Classic 4×6</option>
                <option value="5x7">Large 5×7</option>
              </Select>
            )}
          </Field>
        </div>
      )}
      {test && (
        <div className="flex flex-col gap-2">
          <div className="flex items-center justify-between font-mono text-[12px] text-subtle">
            <span>{test.status ? `${test.status} · ${test.ms} ms · ${fmtBytes(test.bytes)}` : `failed · ${test.ms} ms`}</span>
            <span className={test.value == null ? "text-danger" : "text-ok-text"}>
              {test.error ? test.error : test.value == null ? "nothing matched" : <>value {test.value} → <b className="font-semibold text-ink">{test.formatted}</b></>}
            </span>
          </div>
          {lines.length > 0 && lines[0] !== "" && (
            <pre className="m-0 max-h-[180px] overflow-auto rounded-[10px] bg-ink py-2 font-mono text-[11.5px] leading-[1.55] text-[#E9E3D8]">
              {lines.slice(0, 200).map((l, i) => (
                <div key={i} className={i === hi ? "bg-[rgba(240,100,45,0.28)] px-3" : "px-3"}>{l || " "}</div>
              ))}
            </pre>
          )}
        </div>
      )}
    </Modal>
  );
}

const fmtBytes = (n: number) => (n >= 1024 ? `${(n / 1024).toFixed(1)} KB` : `${n} B`);
