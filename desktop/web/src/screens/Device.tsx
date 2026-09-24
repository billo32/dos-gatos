import { useEffect, useRef, useState } from "react";
import { api, errText, type FontId } from "../api";
import { useStore } from "../store";
import { Button, Field, Input, Modal, PageHeader, Select, SettingsGroup, SettingsRow, Switch } from "../components/ui";
import { ago, linkState } from "../App";

const FONTS: [FontId, string][] = [
  ["3x5", "Blocky 3×5"],
  ["4x6", "Classic 4×6"],
  ["5x7", "Large 5×7"],
];

export function Device() {
  const { status, settings, autostart, macTz, version, saveSettings, setAutostart, flash } = useStore();
  const s = settings!;
  const up = linkState(status) === "usb";
  const [zones, setZones] = useState<string[]>([]);
  const [wifiOpen, setWifiOpen] = useState(false);
  const [bright, setBright] = useState(s.brightness);
  const timer = useRef<number | undefined>(undefined);

  useEffect(() => {
    api.listTimezones().then(setZones, () => {});
  }, []);
  useEffect(() => setBright(s.brightness), [s.brightness]);

  const onBright = (v: number) => {
    setBright(v);
    clearTimeout(timer.current);
    timer.current = window.setTimeout(() => saveSettings({ ...useStore.getState().settings!, brightness: v }), 150);
  };

  const run = (f: () => Promise<unknown>, ok?: string) => f().then(() => ok && flash(ok), (e) => flash(errText(e), true));
  const wifi = status?.wifi;
  const wifiOn = !!wifi?.ssid;
  const wifiHint = !up
    ? "Connect the clock to see its Wi‑Fi state"
    : wifiOn
      ? `On · ${wifi!.ssid}${wifi!.state === "connected" ? " · connected" : ""} · takes over when USB is gone`
      : "Off · clock works only while the Mac is connected";

  return (
    <>
      <PageHeader title="Device" sub="Settings for this clock" />
      {!up && (
        <div role="status" className="shrink-0 rounded-xl border border-[#F3D9A6] bg-[#FFF7E6] px-4 py-2.5 text-[13px] text-[#6B4A0E]">
          Clock not connected — changes will apply when it reconnects
        </div>
      )}
      <div className="grid min-h-0 grow grid-cols-2 content-start gap-5 overflow-auto">
        <div className="flex flex-col gap-5">
          <SettingsGroup title="Display">
            <SettingsRow title="Brightness" htmlFor="d-br">
              <input id="d-br" type="range" min={0} max={100} value={bright} onChange={(e) => onBright(Number(e.target.value))} className="m-0 w-[170px] accent-toggle-on" aria-valuetext={`${bright}%`} />
            </SettingsRow>
            <SettingsRow title="Default font" htmlFor="d-font" hint={status?.font ? undefined : "Shown after the clock connects"} disabled={!up}>
              <Select id="d-font" className="!h-9 !w-[170px]" disabled={!up} value={status?.font ?? "3x5"} onChange={(e) => run(() => api.setFont(e.target.value as FontId))}>
                {FONTS.map(([id, label]) => <option key={id} value={id}>{label}</option>)}
              </Select>
            </SettingsRow>
          </SettingsGroup>
          <SettingsGroup title="Time">
            <SettingsRow title="Time zone" htmlFor="d-tz" hint={s.tz ? undefined : "Follows this Mac"}>
              <Select id="d-tz" className="!h-9 !w-[170px]" value={s.tz ?? ""} onChange={(e) => saveSettings({ ...s, tz: e.target.value || null })}>
                <option value="">{macTz ?? "Same as the Mac"}</option>
                {zones.filter((z) => z !== macTz).map((z) => <option key={z} value={z}>{z}</option>)}
              </Select>
            </SettingsRow>
            <SettingsRow title="24-hour clock">
              <Switch on={s.clock.h24} onChange={(h24) => saveSettings({ ...s, clock: { ...s.clock, h24 } })} label="24-hour clock" />
            </SettingsRow>
          </SettingsGroup>
        </div>
        <div className="flex flex-col gap-5">
          <SettingsGroup title="Connection">
            <SettingsRow title="USB" hint={status?.port ? `${status.port}${up ? ` · replied ${ago(status.last_rx_ago)}` : " · waiting for the clock"}` : "No clock found"}>
              <Button small onClick={() => run(() => api.reconnect(), "Reconnecting…")}>Reconnect</Button>
            </SettingsRow>
            <SettingsRow title="Wi‑Fi fallback" hint={wifiHint} disabled={!up}>
              <Button small disabled={!up} onClick={() => setWifiOpen(true)}>{wifiOn ? "Change…" : "Set up…"}</Button>
            </SettingsRow>
            <SettingsRow title="Launch at login">
              <Switch on={autostart} onChange={setAutostart} label="Launch at login" />
            </SettingsRow>
          </SettingsGroup>
          <SettingsGroup title="Firmware">
            <SettingsRow title={status?.fw ? `Version ${status.fw}` : "Version unknown"} hint={`Dos GatOS app ${version}`} />
            <SettingsRow title="Restart the clock" disabled={!up}>
              <Button small disabled={!up} onClick={() => run(() => api.restartClock(), "Restarting the clock…")}>Restart</Button>
            </SettingsRow>
          </SettingsGroup>
        </div>
      </div>
      {wifiOpen && <WifiModal on={wifiOn} ssid={wifi?.ssid ?? ""} onClose={() => setWifiOpen(false)} />}
    </>
  );
}

function WifiModal({ on, ssid: ssid0, onClose }: { on: boolean; ssid: string; onClose: () => void }) {
  const flash = useStore((s) => s.flash);
  const [ssid, setSsid] = useState(ssid0);
  const [pass, setPass] = useState(""); // never pre-filled, never stored on the Mac
  const [err, setErr] = useState<string | null>(null);
  const save = async () => {
    if (!ssid.trim()) return setErr("Enter the network name");
    try {
      await api.wifiSet(ssid.trim(), pass);
      flash("Wi‑Fi settings sent to the clock");
      onClose();
    } catch (e) {
      setErr(errText(e));
    }
  };
  const off = async () => {
    try {
      await api.wifiOff();
      flash("Wi‑Fi turned off on the clock");
      onClose();
    } catch (e) {
      setErr(errText(e));
    }
  };
  return (
    <Modal
      title="Wi‑Fi fallback"
      onClose={onClose}
      onSubmit={save}
      footer={
        <>
          {on ? <Button kind="danger" onClick={off}>Turn Wi‑Fi off</Button> : <span />}
          <div className="flex gap-2">
            <Button onClick={onClose}>Cancel</Button>
            <Button kind="primary" onClick={save}>Save to clock</Button>
          </div>
        </>
      }
    >
      <div className="text-[14px] leading-[1.55] text-muted">
        When the USB cable is unplugged or the Mac is asleep, the clock joins Wi‑Fi and fetches sources itself. USB always wins when it’s available.
      </div>
      <Field label="Network (SSID)" error={err}>{(id) => <Input id={id} autoComplete="off" value={ssid} onChange={(e) => { setSsid(e.target.value); setErr(null); }} />}</Field>
      <Field label="Password" hint="Stored only on the clock. In Wi‑Fi mode HTTPS certificates aren’t verified, so keep secrets out of source URLs.">
        {(id) => <Input id={id} type="password" autoComplete="off" value={pass} onChange={(e) => setPass(e.target.value)} />}
      </Field>
    </Modal>
  );
}
