//! USB-serial link to the clock: NDJSON protocol, HTTP proxy for the clock's requests,
//! time/config sync, icon upload, Wi-Fi and font settings.

use serde::Serialize;
use serde_json::{json, Value};
use serialport::{SerialPort, SerialPortType};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::io::{ErrorKind, Read, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use crate::extract::extract;
use crate::icons::{self, IconStore};
use crate::settings::{self, Settings};
use crate::{error, info, warn};

pub const BAUD: u32 = 460_800; // CH340 on macOS can't do 921600 reliably
const KNOWN_USB_IDS: &[(u16, u16)] = &[(0x1A86, 0x7523), (0x1A86, 0x55D4), (0x10C4, 0xEA60)];
const PING_EVERY: Duration = Duration::from_secs(3);
const TIME_SYNC_EVERY: Duration = Duration::from_secs(3600);
const MAX_BODY_OUT: usize = 1024;
const MAX_BODY_IN: usize = 2 * 1024 * 1024;
pub const FONTS: &[&str] = &["5x7", "4x6", "3x5"];

#[derive(Serialize, Clone, Default)]
pub struct AppValue {
    pub value: Option<String>,
    pub status: u16,
    pub at: String,
    /// why the last poll gave no value (network error, HTTP status, nothing matched)
    pub error: Option<String>,
}

#[derive(Serialize, Clone, Default)]
pub struct Status {
    pub connected: bool,
    pub port: Option<String>,
    pub fw: Option<String>,
    pub last_rx_ago: Option<u64>,
    pub rtc: Option<String>,
    pub values: BTreeMap<String, AppValue>,
    pub api_error: Option<String>,
    /// bumped on every apps.json save — the settings window reloads the list
    pub apps_rev: u64,
    /// default font on the clock (from hello)
    pub font: Option<String>,
    /// {ssid, state: off|connecting|connected, ip?, rssi?} as reported by the clock
    pub wifi: Option<Value>,
    /// name of the screen the clock is showing ("clock" or a source name), reported by fw ≥ 0.5
    pub screen: Option<String>,
}

type Listener = Box<dyn Fn(&Status) + Send + Sync>;

pub struct Link {
    pub apps_path: PathBuf,
    pub settings_path: PathBuf,
    settings: Mutex<Settings>,
    writer: Mutex<Option<Box<dyn SerialPort>>>,
    status: Mutex<Status>,
    last_rx: Mutex<Option<Instant>>,
    last_time_sync: Mutex<Option<Instant>>,
    url_names: Mutex<HashMap<String, String>>,
    icons_sent: Mutex<HashSet<String>>,
    reconnect: AtomicBool,
    listener: Mutex<Option<Listener>>,
    http: reqwest::blocking::Client,
    pub icons: IconStore,
}

/// POSIX TZ rule of the Mac's local zone, e.g. "CET-1CEST,M3.5.0,M10.5.0/3".
/// Taken from the footer of the TZif file behind /etc/localtime; the clock uses it for NTP time in Wi-Fi mode.
pub fn local_tz_posix() -> Option<String> {
    tz_posix_from(std::path::Path::new("/etc/localtime"))
}

pub fn tz_posix_from(path: &std::path::Path) -> Option<String> {
    let bytes = std::fs::read(path).ok()?;
    if !bytes.starts_with(b"TZif") || bytes.get(4).copied().unwrap_or(0) < b'2' {
        return None;
    }
    // footer: "\n<POSIX TZ>\n" at the very end of a v2+ file
    let tail = String::from_utf8_lossy(&bytes[bytes.len().saturating_sub(128)..]).into_owned();
    let trimmed = tail.trim_end_matches('\n');
    let footer = &trimmed[trimmed.rfind('\n')? + 1..];
    let ok = !footer.is_empty() && footer.len() < 64 && footer.chars().all(|c| c.is_ascii_graphic());
    ok.then(|| footer.to_string())
}

impl Link {
    pub fn new(apps_path: PathBuf, settings_path: PathBuf, icon_dir: PathBuf) -> Arc<Self> {
        let http = reqwest::blocking::Client::builder()
            .user_agent("dos-gatos-agent/0.4")
            .timeout(Duration::from_secs(20))
            .build()
            .expect("http client");
        let link = Arc::new(Self {
            apps_path,
            settings: Mutex::new(Settings::load(&settings_path)),
            settings_path,
            writer: Mutex::new(None),
            status: Mutex::new(Status::default()),
            last_rx: Mutex::new(None),
            last_time_sync: Mutex::new(None),
            url_names: Mutex::new(HashMap::new()),
            icons_sent: Mutex::new(HashSet::new()),
            reconnect: AtomicBool::new(false),
            listener: Mutex::new(None),
            http,
            icons: IconStore::new(icon_dir),
        });
        if let Ok(apps) = link.read_apps() {
            link.remember_names(&apps);
        }
        link
    }

    pub fn on_change(&self, f: impl Fn(&Status) + Send + Sync + 'static) {
        *self.listener.lock().unwrap() = Some(Box::new(f));
    }

    pub fn status(&self) -> Status {
        let mut s = self.status.lock().unwrap().clone();
        s.last_rx_ago = self.last_rx.lock().unwrap().map(|t| t.elapsed().as_secs());
        s
    }

    fn update(&self, f: impl FnOnce(&mut Status)) {
        f(&mut self.status.lock().unwrap());
        let snapshot = self.status();
        if let Some(l) = self.listener.lock().unwrap().as_ref() {
            l(&snapshot);
        }
    }

    pub fn set_api_error(&self, e: Option<String>) {
        self.update(|s| s.api_error = e);
    }

    pub fn settings(&self) -> Settings {
        self.settings.lock().unwrap().clone()
    }

    /// Save settings and push what changed to the clock. Returns whether the clock got it now
    /// (if not, everything is re-sent on the next hello).
    pub fn save_settings(&self, new: Settings) -> Result<bool, String> {
        if let Some(tz) = &new.tz {
            if !settings::valid_tz(tz) {
                return Err(format!("unknown time zone {tz}"));
            }
        }
        let old = std::mem::replace(&mut *self.settings.lock().unwrap(), new.clone());
        new.save(&self.settings_path)?;
        let mut sent = true;
        if old.clock != new.clock {
            sent &= self.send(&new.clock_msg(&self.read_apps().unwrap_or_default())).is_ok();
        }
        if old.brightness != new.brightness {
            sent &= self.send(&json!({"t": "bright", "v": new.device_brightness()})).is_ok();
        }
        if old.tz != new.tz {
            info!("time zone: {}", new.tz.as_deref().unwrap_or("same as the Mac"));
            sent &= self.send_time();
        }
        Ok(sent)
    }

    pub fn restart_clock(&self) -> Result<(), String> {
        self.send(&json!({"t": "settings", "restart": true}))?;
        info!("restart requested");
        Ok(())
    }

    pub fn request_reconnect(&self) {
        self.reconnect.store(true, Ordering::SeqCst);
    }

    // ---------- apps.json ----------
    pub fn read_apps(&self) -> Result<Value, String> {
        let text = std::fs::read_to_string(&self.apps_path).map_err(|e| e.to_string())?;
        let v: Value = serde_json::from_str(&text).map_err(|e| format!("apps.json: {e}"))?;
        if !v.is_array() {
            return Err("apps.json must be an array".into());
        }
        Ok(v)
    }

    fn remember_names(&self, apps: &Value) {
        let mut m = self.url_names.lock().unwrap();
        m.clear();
        for a in apps.as_array().into_iter().flatten() {
            if let (Some(u), Some(n)) = (a.get("url").and_then(Value::as_str), a.get("name").and_then(Value::as_str)) {
                m.insert(u.to_string(), n.to_string());
            }
        }
    }

    /// Save the config and push it to the clock right away (if connected).
    pub fn save_apps(self: &Arc<Self>, apps: &Value) -> Result<bool, String> {
        if !apps.is_array() {
            return Err("expected an array of apps".into());
        }
        let text = serde_json::to_string_pretty(apps).map_err(|e| e.to_string())?;
        settings::write_atomic(&self.apps_path, (text + "\n").as_bytes())?;
        self.remember_names(apps);
        let names: Vec<String> = self.url_names.lock().unwrap().values().cloned().collect();
        self.update(|s| {
            s.values.retain(|k, _| names.contains(k));
            s.apps_rev += 1;
        });
        match self.send(&json!({"t": "apps", "apps": apps})) {
            Ok(()) => {
                let _ = self.send(&self.settings().clock_msg(apps)); // clock position depends on enabled sources
                info!("apps saved and sent to the clock ({})", apps.as_array().map_or(0, |a| a.len()));
                self.push_icons_async(icon_ids(apps));
                Ok(true)
            }
            Err(_) => {
                info!("apps saved; the clock is not connected, they'll be sent on connect");
                Ok(false)
            }
        }
    }

    // ---------- icons ----------
    /// Make sure the clock has this icon (download from LaMetric if needed).
    pub fn ensure_icon(&self, id: &str) -> Result<(), String> {
        if self.icons_sent.lock().unwrap().contains(id) {
            return Ok(());
        }
        let icon = self.icons.get(id)?;
        self.send(&json!({"t": "icon", "id": id, "n": icon.frames.len(), "d": icon.delays, "px": icons::to_hex(&icon)}))?;
        self.icons_sent.lock().unwrap().insert(id.to_string());
        info!("icon {id} sent to the clock ({} frame(s))", icon.frames.len());
        Ok(())
    }

    fn push_icons_async(self: &Arc<Self>, ids: Vec<String>) {
        if ids.is_empty() {
            return;
        }
        let me = Arc::clone(self);
        thread::spawn(move || {
            for id in ids {
                if let Err(e) = me.ensure_icon(&id) {
                    warn!("icon {id}: {e}");
                }
            }
        });
    }

    // ---------- settings on the clock ----------
    pub fn set_font(&self, font: &str) -> Result<(), String> {
        if !FONTS.contains(&font) {
            return Err(format!("unknown font {font}"));
        }
        self.send(&json!({"t": "settings", "font": font}))
    }

    pub fn set_wifi(&self, ssid: &str, pass: &str) -> Result<(), String> {
        self.send(&json!({"t": "wifi", "ssid": ssid, "pass": pass}))?;
        info!("{}", if ssid.is_empty() { "Wi-Fi turned off on the clock".to_string() } else { format!("Wi-Fi settings sent: {ssid}") });
        Ok(())
    }

    // ---------- io ----------
    pub fn send(&self, msg: &Value) -> Result<(), String> {
        let mut line = serde_json::to_vec(msg).map_err(|e| e.to_string())?;
        line.push(b'\n');
        let mut w = self.writer.lock().unwrap();
        let port = w.as_mut().ok_or("clock not connected")?;
        port.write_all(&line).map_err(|e| e.to_string())?;
        Ok(())
    }

    fn send_time(&self) -> bool {
        let tz = chrono::Local::now().offset().local_minus_utc();
        let epoch = chrono::Utc::now().timestamp();
        let mut msg = json!({"t": "time", "epoch": epoch, "tz": tz});
        let zone = self.settings.lock().unwrap().tz.clone();
        if let Some(p) = zone.as_deref().and_then(settings::tz_posix).or_else(local_tz_posix) {
            msg["tzp"] = json!(p);
        }
        let ok = self.send(&msg).is_ok();
        if ok {
            *self.last_time_sync.lock().unwrap() = Some(Instant::now());
        }
        ok
    }

    // ---------- protocol ----------
    fn handle(self: &Arc<Self>, raw: &str) {
        // Plain-text output from the clock = its crash handler (panic backtrace, brownout, watchdog)
        const CRASH: &[&str] = &["Guru Meditation", "Backtrace", "panic", "Brownout", "abort()", "Rebooting", "assert", "wdt", "Stack canary"];
        let json_at = raw.find('{');
        if json_at != Some(0) && CRASH.iter().any(|k| raw.contains(k)) {
            warn!("device crash output: {}", raw.chars().take(300).collect::<String>());
        }
        // ROM bootloader output without a newline may be glued to the start of the JSON
        let line = match json_at {
            Some(i) => &raw[i..],
            None => return,
        };
        let msg: Value = match serde_json::from_str(line) {
            Ok(v) => v,
            Err(_) => return,
        };
        *self.last_rx.lock().unwrap() = Some(Instant::now());
        match msg.get("t").and_then(Value::as_str).unwrap_or("") {
            "hello" => {
                let fw = msg.get("fw").and_then(Value::as_str).unwrap_or("?").to_string();
                let rst = msg.get("rst").and_then(Value::as_str).unwrap_or("?");
                info!("device hello: fw={fw} apps={} reset={rst} heap={}", msg.get("apps").unwrap_or(&Value::Null), msg.get("heap").unwrap_or(&Value::Null));
                if matches!(rst, "panic" | "int_wdt" | "task_wdt" | "wdt" | "brownout") {
                    warn!("the clock restarted after a {rst} — see 'device crash output' lines above");
                }
                // hello = часы (пере)загрузились: могли быть перепрошиты или стёрты, иконки отправляем заново
                self.icons_sent.lock().unwrap().clear();
                let font = msg.get("font").and_then(Value::as_str).map(str::to_string);
                let wifi = msg.get("wifi").cloned();
                let screen = msg.get("scr").and_then(Value::as_str).map(str::to_string);
                self.update(|s| {
                    s.fw = Some(fw);
                    s.screen = screen;
                    if font.is_some() {
                        s.font = font;
                    }
                    if wifi.is_some() {
                        s.wifi = wifi;
                    }
                });
                self.send_time();
                let st = self.settings();
                let _ = self.send(&st.clock_msg(&self.read_apps().unwrap_or_default()));
                let _ = self.send(&json!({"t": "bright", "v": st.device_brightness()}));
                match self.read_apps() {
                    Ok(apps) => {
                        self.remember_names(&apps);
                        let _ = self.send(&json!({"t": "apps", "apps": apps}));
                        info!("pushed {} apps", apps.as_array().map_or(0, |a| a.len()));
                        self.push_icons_async(icon_ids(&apps));
                    }
                    Err(e) => warn!("apps.json: {e}"),
                }
            }
            "wifi" => {
                let mut w = msg.clone();
                if let Some(o) = w.as_object_mut() {
                    o.remove("t");
                }
                info!("device wifi: {}", w);
                self.update(|s| s.wifi = Some(w));
            }
            "scr" => {
                let name = msg.get("name").and_then(Value::as_str).map(str::to_string);
                self.update(|s| s.screen = name);
            }
            "req" => {
                let me = Arc::clone(self);
                thread::spawn(move || me.proxy(msg));
            }
            "log" => {
                let m = msg.get("m").and_then(Value::as_str).unwrap_or("").to_string();
                info!("device: {m}");
                if m.starts_with("rtc") {
                    self.update(|s| s.rtc = Some(m));
                }
            }
            "btn" => info!("button: {}", msg.get("b").and_then(Value::as_str).unwrap_or("?")),
            _ => {}
        }
    }

    fn fetch(&self, url: &str) -> Result<(u16, String), String> {
        let resp = self.http.get(url).send().map_err(|e| e.to_string())?;
        let status = resp.status().as_u16();
        let mut buf = Vec::new();
        resp.take(MAX_BODY_IN as u64).read_to_end(&mut buf).map_err(|e| e.to_string())?;
        Ok((status, String::from_utf8_lossy(&buf).into_owned()))
    }

    /// Run a request with the extraction options from `app` (same format as req / apps.json).
    /// Returns (HTTP status, extracted value, response body).
    pub fn run_query(&self, app: &Value) -> (u16, Result<Option<String>, String>, String) {
        let url = app.get("url").and_then(Value::as_str).unwrap_or("");
        match self.fetch(url) {
            Err(e) => (0, Err(e), String::new()),
            Ok((status, text)) => {
                if !(200..300).contains(&status) {
                    return (status, Ok(None), text);
                }
                let keep = app.get("keep").and_then(Value::as_u64).unwrap_or(128) as usize;
                let r = extract(
                    &text,
                    app.get("path").and_then(Value::as_str),
                    app.get("find").and_then(Value::as_str),
                    keep,
                    app.get("re").and_then(Value::as_str),
                );
                (status, r, text)
            }
        }
    }

    fn proxy(&self, msg: Value) {
        let id = msg.get("id").cloned().unwrap_or(Value::Null);
        let url = msg.get("url").and_then(Value::as_str).unwrap_or("").to_string();
        let t0 = Instant::now();
        let (status, result, _) = self.run_query(&msg);
        let ms = t0.elapsed().as_millis();
        let mut err = None;
        let body = match result {
            Ok(Some(v)) => Some(v.chars().take(MAX_BODY_OUT).collect::<String>()),
            Ok(None) => {
                err = Some(if (200..300).contains(&status) { "nothing matched".to_string() } else { format!("HTTP {status}") });
                None
            }
            Err(e) => {
                error!("req {id} {url}: {e}");
                err = Some(e);
                None
            }
        };
        let _ = self.send(&json!({"t": "resp", "id": id, "status": status, "body": body}));
        let name = self.url_names.lock().unwrap().get(&url).cloned();
        let label = name.clone().unwrap_or_else(|| url.chars().take(60).collect());
        match (&body, &err) {
            (Some(v), _) => info!("{label} → {v} · {ms} ms"),
            (None, Some(e)) if (200..300).contains(&status) => warn!("{label} · {e} · {ms} ms"),
            (None, Some(e)) => warn!("{label} · {e}"),
            _ => {}
        }
        if let Some(name) = name {
            let at = chrono::Local::now().format("%H:%M:%S").to_string();
            self.update(|s| {
                s.values.insert(name, AppValue { value: body, status, at, error: err });
            });
        }
    }

    // ---------- loops ----------
    fn find_port() -> Option<String> {
        // manual override (debugging, emulator): TC001_PORT=/dev/cu.usbserial-10
        if let Ok(p) = std::env::var("TC001_PORT") {
            return Some(p);
        }
        let ports = serialport::available_ports().ok()?;
        let mut found: Vec<String> = ports
            .into_iter()
            .filter(|p| matches!(&p.port_type, SerialPortType::UsbPort(u) if KNOWN_USB_IDS.contains(&(u.vid, u.pid))))
            .map(|p| p.port_name)
            .collect();
        // on macOS every port shows up twice, /dev/tty.* and /dev/cu.* — we want cu
        found.sort_by_key(|n| !n.contains("/cu."));
        found.into_iter().next()
    }

    fn open(port: &str) -> serialport::Result<Box<dyn SerialPort>> {
        let mut p = serialport::new(port, BAUD)
            .timeout(Duration::from_millis(200))
            .dtr_on_open(false) // DTR/RTS reset the ESP32 through the auto-reset circuit
            .open()?;
        let _ = p.write_request_to_send(false);
        let _ = p.write_data_terminal_ready(false);
        Ok(p)
    }

    pub fn start(self: &Arc<Self>) {
        let me = Arc::clone(self);
        thread::spawn(move || me.pinger());
        let me = Arc::clone(self);
        thread::spawn(move || me.run());
    }

    fn pinger(&self) {
        loop {
            thread::sleep(PING_EVERY);
            if self.send(&json!({"t": "ping"})).is_err() {
                continue;
            }
            let due = self.last_time_sync.lock().unwrap().map_or(true, |t| t.elapsed() > TIME_SYNC_EVERY);
            if due && self.status.lock().unwrap().fw.is_some() {
                self.send_time(); // also catches DST changes
            }
            self.update(|_| {}); // refresh "last reply N s ago" in the UI
        }
    }

    fn run(self: &Arc<Self>) {
        let mut waiting_logged = false;
        loop {
            self.reconnect.store(false, Ordering::SeqCst);
            let Some(name) = Self::find_port() else {
                if !waiting_logged {
                    info!("waiting for TC001 (CH340) ...");
                    waiting_logged = true;
                }
                thread::sleep(Duration::from_secs(2));
                continue;
            };
            waiting_logged = false;
            let mut reader = match Self::open(&name) {
                Ok(p) => p,
                Err(e) => {
                    warn!("open {name}: {e}");
                    thread::sleep(Duration::from_secs(2));
                    continue;
                }
            };
            let writer = match reader.try_clone() {
                Ok(w) => w,
                Err(e) => {
                    warn!("clone {name}: {e}");
                    thread::sleep(Duration::from_secs(2));
                    continue;
                }
            };
            info!("connected: {name}");
            *self.writer.lock().unwrap() = Some(writer);
            self.icons_sent.lock().unwrap().clear();
            self.update(|s| {
                s.connected = true;
                s.port = Some(name.clone());
            });
            let _ = self.send(&json!({"t": "hello?"}));

            let mut buf: Vec<u8> = Vec::new();
            let mut chunk = [0u8; 4096];
            loop {
                if self.reconnect.load(Ordering::SeqCst) {
                    info!("reconnecting on request");
                    break;
                }
                match reader.read(&mut chunk) {
                    Ok(0) => continue,
                    Ok(n) => {
                        buf.extend_from_slice(&chunk[..n]);
                        while let Some(pos) = buf.iter().position(|&b| b == b'\n') {
                            let line: Vec<u8> = buf.drain(..=pos).collect();
                            let text = String::from_utf8_lossy(&line);
                            let text = text.trim();
                            if !text.is_empty() {
                                self.handle(text);
                            }
                        }
                        if buf.len() > 65536 {
                            buf.clear();
                        }
                    }
                    Err(e) if e.kind() == ErrorKind::TimedOut || e.kind() == ErrorKind::Interrupted => continue,
                    Err(e) => {
                        error!("link lost: {e}");
                        break;
                    }
                }
            }
            *self.writer.lock().unwrap() = None;
            *self.last_time_sync.lock().unwrap() = None;
            self.update(|s| {
                s.connected = false;
                s.fw = None;
                s.screen = None;
            });
            thread::sleep(Duration::from_secs(1));
        }
    }
}

/// Distinct icon ids referenced by apps.
pub fn icon_ids(apps: &Value) -> Vec<String> {
    let mut ids: Vec<String> = apps
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|a| a.get("icon").and_then(Value::as_str))
        .filter(|id| icons::valid_id(id))
        .map(str::to_string)
        .collect();
    ids.sort();
    ids.dedup();
    ids
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn icon_ids_dedup_and_validate() {
        let apps = json!([{"icon": "2422"}, {"icon": "2422"}, {"icon": "../bad"}, {"name": "x"}, {"icon": "87"}]);
        assert_eq!(icon_ids(&apps), vec!["2422", "87"]);
    }

    #[test]
    fn tz_posix_madrid() {
        let p = std::path::Path::new("/usr/share/zoneinfo/Europe/Madrid");
        if p.exists() {
            assert_eq!(tz_posix_from(p).as_deref(), Some("CET-1CEST,M3.5.0,M10.5.0/3"));
        }
    }

    #[test]
    fn tz_posix_footer_if_available() {
        // /etc/localtime may be missing in CI containers; when present it must parse to a sane rule
        if let Some(p) = local_tz_posix() {
            assert!(!p.is_empty() && p.len() < 64, "{p}");
        }
    }
}
