//! Связь с часами по USB-serial: NDJSON-протокол, HTTP-прокси для запросов часов,
//! синхронизация времени и конфига apps. Порт agent.py на Rust.

use serde::Serialize;
use serde_json::{json, Value};
use serialport::{SerialPort, SerialPortType};
use std::collections::{BTreeMap, HashMap};
use std::io::{ErrorKind, Read, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use crate::extract::extract;
use crate::{info, warn};

pub const BAUD: u32 = 460_800; // CH340 на macOS не держит 921600
const KNOWN_USB_IDS: &[(u16, u16)] = &[(0x1A86, 0x7523), (0x1A86, 0x55D4), (0x10C4, 0xEA60)];
const PING_EVERY: Duration = Duration::from_secs(3);
const TIME_SYNC_EVERY: Duration = Duration::from_secs(3600);
const MAX_BODY_OUT: usize = 1024;
const MAX_BODY_IN: usize = 2 * 1024 * 1024;

#[derive(Serialize, Clone, Default)]
pub struct AppValue {
    pub value: Option<String>,
    pub status: u16,
    pub at: String,
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
    /// растёт при каждом сохранении apps.json — окно настроек перечитывает список
    pub apps_rev: u64,
}

type Listener = Box<dyn Fn(&Status) + Send + Sync>;

pub struct Link {
    pub apps_path: PathBuf,
    writer: Mutex<Option<Box<dyn SerialPort>>>,
    status: Mutex<Status>,
    last_rx: Mutex<Option<Instant>>,
    last_time_sync: Mutex<Option<Instant>>,
    url_names: Mutex<HashMap<String, String>>,
    reconnect: AtomicBool,
    listener: Mutex<Option<Listener>>,
    http: reqwest::blocking::Client,
}

impl Link {
    pub fn new(apps_path: PathBuf) -> Arc<Self> {
        let http = reqwest::blocking::Client::builder()
            .user_agent("tc001-agent/0.1")
            .timeout(Duration::from_secs(20))
            .build()
            .expect("http client");
        let link = Arc::new(Self {
            apps_path,
            writer: Mutex::new(None),
            status: Mutex::new(Status::default()),
            last_rx: Mutex::new(None),
            last_time_sync: Mutex::new(None),
            url_names: Mutex::new(HashMap::new()),
            reconnect: AtomicBool::new(false),
            listener: Mutex::new(None),
            http,
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

    pub fn request_reconnect(&self) {
        self.reconnect.store(true, Ordering::SeqCst);
    }

    // ---------- apps.json ----------
    pub fn read_apps(&self) -> Result<Value, String> {
        let text = std::fs::read_to_string(&self.apps_path).map_err(|e| e.to_string())?;
        let v: Value = serde_json::from_str(&text).map_err(|e| format!("apps.json: {e}"))?;
        if !v.is_array() {
            return Err("apps.json должен быть массивом".into());
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

    /// Сохранить конфиг и сразу отправить на часы (если подключены).
    pub fn save_apps(&self, apps: &Value) -> Result<bool, String> {
        if !apps.is_array() {
            return Err("ожидается массив apps".into());
        }
        let text = serde_json::to_string_pretty(apps).map_err(|e| e.to_string())?;
        std::fs::write(&self.apps_path, text + "\n").map_err(|e| e.to_string())?;
        self.remember_names(apps);
        let names: Vec<String> = self.url_names.lock().unwrap().values().cloned().collect();
        self.update(|s| {
            s.values.retain(|k, _| names.contains(k));
            s.apps_rev += 1;
        });
        match self.send(&json!({"t": "apps", "apps": apps})) {
            Ok(()) => {
                info!("apps сохранены и отправлены на часы ({})", apps.as_array().map_or(0, |a| a.len()));
                Ok(true)
            }
            Err(_) => {
                info!("apps сохранены; часы не подключены — отправятся при подключении");
                Ok(false)
            }
        }
    }

    // ---------- io ----------
    pub fn send(&self, msg: &Value) -> Result<(), String> {
        let mut line = serde_json::to_vec(msg).map_err(|e| e.to_string())?;
        line.push(b'\n');
        let mut w = self.writer.lock().unwrap();
        let port = w.as_mut().ok_or("часы не подключены")?;
        port.write_all(&line).map_err(|e| e.to_string())?;
        Ok(())
    }

    fn send_time(&self) {
        let tz = chrono::Local::now().offset().local_minus_utc();
        let epoch = chrono::Utc::now().timestamp();
        if self.send(&json!({"t": "time", "epoch": epoch, "tz": tz})).is_ok() {
            *self.last_time_sync.lock().unwrap() = Some(Instant::now());
        }
    }

    // ---------- protocol ----------
    fn handle(self: &Arc<Self>, raw: &str) {
        // вывод ROM-бутлоадера без перевода строки может приклеиться к началу JSON
        let line = match raw.find('{') {
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
                info!("device hello: fw={fw} apps={}", msg.get("apps").unwrap_or(&Value::Null));
                self.update(|s| s.fw = Some(fw));
                self.send_time();
                match self.read_apps() {
                    Ok(apps) => {
                        self.remember_names(&apps);
                        let _ = self.send(&json!({"t": "apps", "apps": apps}));
                        info!("pushed {} apps", apps.as_array().map_or(0, |a| a.len()));
                    }
                    Err(e) => warn!("apps.json: {e}"),
                }
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

    /// Выполнить запрос с параметрами извлечения из `app` (формат как у req / apps.json).
    pub fn run_query(&self, app: &Value) -> (u16, Result<Option<String>, String>, String) {
        let url = app.get("url").and_then(Value::as_str).unwrap_or("");
        match self.fetch(url) {
            Err(e) => (0, Err(e), String::new()),
            Ok((status, text)) => {
                let preview: String = text.chars().take(400).collect();
                if !(200..300).contains(&status) {
                    return (status, Ok(None), preview);
                }
                let keep = app.get("keep").and_then(Value::as_u64).unwrap_or(128) as usize;
                let r = extract(
                    &text,
                    app.get("path").and_then(Value::as_str),
                    app.get("find").and_then(Value::as_str),
                    keep,
                    app.get("re").and_then(Value::as_str),
                );
                (status, r, preview)
            }
        }
    }

    fn proxy(&self, msg: Value) {
        let id = msg.get("id").cloned().unwrap_or(Value::Null);
        let url = msg.get("url").and_then(Value::as_str).unwrap_or("").to_string();
        let (status, result, _) = self.run_query(&msg);
        let body = match result {
            Ok(Some(v)) => Some(v.chars().take(MAX_BODY_OUT).collect::<String>()),
            Ok(None) => None,
            Err(e) => {
                warn!("req {id} {url}: {e}");
                None
            }
        };
        info!("req {id} {} -> {status} {:?}", url.chars().take(80).collect::<String>(), body.as_deref().unwrap_or(""));
        let _ = self.send(&json!({"t": "resp", "id": id, "status": status, "body": body}));
        let name = self.url_names.lock().unwrap().get(&url).cloned();
        if let Some(name) = name {
            let at = chrono::Local::now().format("%H:%M:%S").to_string();
            self.update(|s| {
                s.values.insert(name, AppValue { value: body, status, at });
            });
        }
    }

    // ---------- loops ----------
    fn find_port() -> Option<String> {
        // ручное указание порта (отладка, эмулятор): TC001_PORT=/dev/cu.usbserial-10
        if let Ok(p) = std::env::var("TC001_PORT") {
            return Some(p);
        }
        let ports = serialport::available_ports().ok()?;
        let mut found: Vec<String> = ports
            .into_iter()
            .filter(|p| matches!(&p.port_type, SerialPortType::UsbPort(u) if KNOWN_USB_IDS.contains(&(u.vid, u.pid))))
            .map(|p| p.port_name)
            .collect();
        // на macOS каждый порт виден дважды: /dev/tty.* и /dev/cu.* — нужен cu
        found.sort_by_key(|n| !n.contains("/cu."));
        found.into_iter().next()
    }

    fn open(port: &str) -> serialport::Result<Box<dyn SerialPort>> {
        let mut p = serialport::new(port, BAUD)
            .timeout(Duration::from_millis(200))
            .dtr_on_open(false) // DTR/RTS через auto-reset схему сбрасывают ESP32
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
                self.send_time(); // заодно ловит переход на летнее/зимнее время
            }
            // обновить «последний ответ N с назад» в UI
            self.update(|_| {});
        }
    }

    fn run(self: &Arc<Self>) {
        let mut waiting_logged = false;
        loop {
            self.reconnect.store(false, Ordering::SeqCst);
            let Some(name) = Self::find_port() else {
                if !waiting_logged {
                    info!("жду TC001 (CH340) ...");
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
            self.update(|s| {
                s.connected = true;
                s.port = Some(name.clone());
            });
            let _ = self.send(&json!({"t": "hello?"}));

            let mut buf: Vec<u8> = Vec::new();
            let mut chunk = [0u8; 4096];
            loop {
                if self.reconnect.load(Ordering::SeqCst) {
                    info!("переподключение по запросу");
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
                        warn!("link lost: {e}");
                        break;
                    }
                }
            }
            *self.writer.lock().unwrap() = None;
            *self.last_time_sync.lock().unwrap() = None;
            self.update(|s| {
                s.connected = false;
                s.fw = None;
            });
            thread::sleep(Duration::from_secs(1));
        }
    }
}
