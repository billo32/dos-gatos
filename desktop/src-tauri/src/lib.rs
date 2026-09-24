//! Dos GatOS — menu bar app: USB link to the TC001 clock, main window, launch at login.

mod api;
mod extract;
mod icons;
mod link;
pub mod logbuf;
mod settings;

use serde::Serialize;
use serde_json::{json, Value};
use std::path::PathBuf;
use std::sync::Arc;
use tauri::menu::{CheckMenuItem, Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Emitter, Manager, State, WindowEvent};
use tauri_plugin_autostart::{MacosLauncher, ManagerExt};
use tauri_plugin_dialog::DialogExt;

use settings::Settings;

use link::{Link, Status};

const DEFAULT_APPS: &str = include_str!("../../../agent/apps.json");

// ---------- commands (main window) ----------

#[derive(Serialize)]
struct TestResult {
    status: u16,
    ms: u64,
    bytes: usize,
    value: Option<String>,
    formatted: Option<String>,
    error: Option<String>,
    /// first 4 KB of the response body
    preview: String,
}

#[derive(Serialize)]
struct AppState {
    status: Status,
    settings: Settings,
    autostart: bool,
    mac_tz: Option<String>,
    apps_path: String,
    version: &'static str,
}

#[tauri::command]
fn get_state(app: AppHandle, link: State<Arc<Link>>) -> AppState {
    AppState {
        status: link.status(),
        settings: link.settings(),
        autostart: app.autolaunch().is_enabled().unwrap_or(false),
        mac_tz: settings::mac_timezone(),
        apps_path: link.apps_path.display().to_string(),
        version: env!("CARGO_PKG_VERSION"),
    }
}

#[tauri::command]
fn get_status(link: State<Arc<Link>>) -> Status {
    link.status()
}

#[tauri::command]
fn get_apps(link: State<Arc<Link>>) -> Result<Value, String> {
    link.read_apps()
}

#[tauri::command]
fn save_apps(apps: Value, link: State<Arc<Link>>) -> Result<bool, String> {
    link.inner().save_apps(&apps)
}

#[tauri::command]
fn save_settings(settings: Settings, app: AppHandle, link: State<Arc<Link>>) -> Result<bool, String> {
    let sent = link.save_settings(settings.clone())?;
    let _ = app.emit("settings", settings);
    Ok(sent)
}

#[tauri::command]
async fn test_source(app: Value, link: State<'_, Arc<Link>>) -> Result<TestResult, String> {
    let l = link.inner().clone();
    let a = app.clone();
    let t0 = std::time::Instant::now();
    let (status, result, body) = tauri::async_runtime::spawn_blocking(move || l.run_query(&a))
        .await
        .map_err(|e| e.to_string())?;
    let ms = t0.elapsed().as_millis() as u64;
    let (value, error) = match result {
        Ok(v) => (v, None),
        Err(e) => (None, Some(e)),
    };
    let formatted = value.as_deref().map(|v| extract::format_like_device(v, &app));
    let mut cut = body.len().min(4096);
    while !body.is_char_boundary(cut) {
        cut -= 1;
    }
    Ok(TestResult { status, ms, bytes: body.len(), value, formatted, error, preview: body[..cut].to_string() })
}

#[tauri::command]
async fn notify(text: String, color: String, icon: Option<String>, link: State<'_, Arc<Link>>) -> Result<(), String> {
    let l = link.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let dur = l.settings().notify.dur.max(1) * 1000;
        let mut msg = json!({"t": "notify", "text": text, "color": color, "dur": dur});
        if let Some(id) = icon.map(|s| s.trim().to_string()).filter(|s| !s.is_empty()) {
            l.ensure_icon(&id)?;
            msg["icon"] = json!(id);
        }
        l.send(&msg)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn icon_preview(id: String, link: State<'_, Arc<Link>>) -> Result<icons::IconPreview, String> {
    let l = link.inner().clone();
    let icon = tauri::async_runtime::spawn_blocking(move || l.icons.get(id.trim()))
        .await
        .map_err(|e| e.to_string())??;
    Ok(icons::preview(&icon))
}

#[tauri::command]
fn set_font(font: String, link: State<Arc<Link>>) -> Result<(), String> {
    link.set_font(&font)
}

#[tauri::command]
fn wifi_set(ssid: String, password: String, link: State<Arc<Link>>) -> Result<(), String> {
    let ssid = ssid.trim();
    if ssid.is_empty() {
        return Err("Enter the network name".into());
    }
    link.set_wifi(ssid, &password) // sent to the clock only, never stored on the Mac
}

#[tauri::command]
fn wifi_off(link: State<Arc<Link>>) -> Result<(), String> {
    link.set_wifi("", "")
}

#[tauri::command]
fn restart_clock(link: State<Arc<Link>>) -> Result<(), String> {
    link.restart_clock()
}

#[tauri::command]
fn reconnect(link: State<Arc<Link>>) {
    link.request_reconnect();
}

#[tauri::command]
fn list_timezones() -> Vec<String> {
    settings::list_timezones()
}

#[tauri::command]
fn log_tail() -> Vec<String> {
    logbuf::tail()
}

#[tauri::command]
fn log_clear() {
    logbuf::clear();
}

#[tauri::command]
async fn log_export(app: AppHandle) -> Result<Option<String>, String> {
    let name = format!("DosGatOS-{}.log", chrono::Local::now().format("%Y-%m-%d-%H%M"));
    let picked = app.dialog().file().set_file_name(&name).add_filter("Log", &["log", "txt"]).blocking_save_file();
    let Some(path) = picked.and_then(|p| p.into_path().ok()) else { return Ok(None) };
    let mut text = logbuf::tail().join("\n");
    text.push('\n');
    std::fs::write(&path, text).map_err(|e| e.to_string())?;
    Ok(Some(path.display().to_string()))
}

#[tauri::command]
fn reveal_apps(link: State<Arc<Link>>) {
    if cfg!(target_os = "macos") {
        let _ = std::process::Command::new("open").arg("-R").arg(&link.apps_path).spawn();
    } else if let Some(dir) = link.apps_path.parent() {
        open_path(dir);
    }
}

#[tauri::command]
fn open_icon_gallery() {
    open_path(std::path::Path::new("https://developer.lametric.com/icons"));
}

#[tauri::command]
fn set_autostart(enabled: bool, app: AppHandle) -> Result<(), String> {
    let al = app.autolaunch();
    let r = if enabled { al.enable() } else { al.disable() };
    r.map_err(|e| e.to_string())?;
    if let Some(item) = app.try_state::<CheckMenuItem<tauri::Wry>>() {
        let _ = item.set_checked(enabled);
    }
    let _ = app.emit("autostart", enabled);
    Ok(())
}

// ---------- helpers ----------

fn open_path(p: &std::path::Path) {
    let cmd = if cfg!(target_os = "macos") { "open" } else { "xdg-open" };
    let _ = std::process::Command::new(cmd).arg(p).spawn();
}

fn show_main(app: &AppHandle) {
    if let Some(w) = app.get_webview_window("main") {
        // while the window is open the app shows up in the Dock and Cmd‑Tab
        #[cfg(target_os = "macos")]
        let _ = app.set_activation_policy(tauri::ActivationPolicy::Regular);
        let _ = w.show();
        let _ = w.unminimize();
        let _ = w.set_focus();
    }
}

fn log_path(app: &AppHandle) -> PathBuf {
    #[cfg(target_os = "macos")]
    if let Ok(home) = app.path().home_dir() {
        return home.join("Library/Logs/DosGatOS/dosgatos.log");
    }
    app.path()
        .app_log_dir()
        .unwrap_or_else(|_| std::env::temp_dir())
        .join("dosgatos.log")
}

fn ago(secs: u64) -> String {
    if secs < 60 { format!("{secs} s") } else { format!("{} min", secs / 60) }
}

fn status_line(s: &Status) -> String {
    if s.connected && s.fw.is_some() {
        match s.last_rx_ago {
            Some(a) => format!("TC001 · USB · replied {} ago", ago(a)),
            None => "TC001 · USB".into(),
        }
    } else if s.connected {
        "TC001 · USB · waiting for the clock…".into()
    } else {
        "TC001 · not connected".into()
    }
}

/// Remove launchers of the old agents (Python LaunchAgent, "TC001 Agent" login item).
/// Returns true if the old login item was on, so the new app takes it over.
fn migrate_old_agents(app: &AppHandle) -> bool {
    let Ok(home) = app.path().home_dir() else { return false };
    let dir = home.join("Library/LaunchAgents");
    let py = dir.join("dev.tls1.tc001-agent.plist");
    if py.exists() {
        if let Some(uid) = std::process::Command::new("id").arg("-u").output().ok().map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string()) {
            let _ = std::process::Command::new("launchctl").args(["bootout", &format!("gui/{uid}/dev.tls1.tc001-agent")]).status();
        }
        let _ = std::fs::remove_file(&py);
        info!("removed the old Python agent LaunchAgent");
    }
    let old = dir.join("TC001 Agent.plist");
    if old.exists() {
        let _ = std::fs::remove_file(&old);
        info!("removed the old TC001 Agent login item");
        return true;
    }
    false
}

// ---------- app ----------

const OLD_CONFIG_DIR: &str = "dev.tls1.tc001";

/// First launch under the new bundle id: bring over apps.json and the icon cache of "TC001 Agent".
fn migrate_config(cfg_dir: &std::path::Path) {
    let Some(parent) = cfg_dir.parent() else { return };
    let old = parent.join(OLD_CONFIG_DIR);
    let apps = cfg_dir.join("apps.json");
    if apps.exists() || !old.join("apps.json").exists() {
        return;
    }
    if std::fs::copy(old.join("apps.json"), &apps).is_ok() {
        info!("imported apps.json from TC001 Agent");
    }
    if let Ok(rd) = std::fs::read_dir(old.join("icons")) {
        let _ = std::fs::create_dir_all(cfg_dir.join("icons"));
        for e in rd.flatten() {
            let _ = std::fs::copy(e.path(), cfg_dir.join("icons").join(e.file_name()));
        }
    }
}

fn tray_icon(connected: bool) -> tauri::image::Image<'static> {
    let bytes: &'static [u8] = if connected { include_bytes!("../icons/tray.png") } else { include_bytes!("../icons/tray-off.png") };
    tauri::image::Image::from_bytes(bytes).expect("tray icon")
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| show_main(app)))
        .plugin(tauri_plugin_autostart::init(MacosLauncher::LaunchAgent, None))
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            get_state,
            get_status,
            get_apps,
            save_apps,
            save_settings,
            test_source,
            notify,
            icon_preview,
            set_font,
            wifi_set,
            wifi_off,
            restart_clock,
            reconnect,
            list_timezones,
            log_tail,
            log_clear,
            log_export,
            reveal_apps,
            open_icon_gallery,
            set_autostart
        ])
        .setup(|app| {
            // menu bar only; the Dock icon appears while the window is open
            #[cfg(target_os = "macos")]
            app.set_activation_policy(tauri::ActivationPolicy::Accessory);

            let handle = app.handle().clone();
            logbuf::init(&log_path(&handle));
            let h = handle.clone();
            logbuf::on_line(move |line| {
                let _ = h.emit("log", line.to_string());
            });
            info!("Dos GatOS {} started", env!("CARGO_PKG_VERSION"));

            let cfg_dir = app.path().app_config_dir()?;
            std::fs::create_dir_all(&cfg_dir)?;
            migrate_config(&cfg_dir);
            let apps_path = cfg_dir.join("apps.json");
            if !apps_path.exists() {
                std::fs::write(&apps_path, DEFAULT_APPS)?;
                info!("created {}", apps_path.display());
            }

            // launch at login: on by default, and taken over from the old agents
            let took_over = migrate_old_agents(&handle);
            let marker = cfg_dir.join(".autostart-initialized");
            if !marker.exists() || took_over {
                if let Err(e) = app.autolaunch().enable() {
                    warn!("autostart: {e}");
                }
                let _ = std::fs::write(&marker, "");
            }

            let link = Link::new(apps_path, cfg_dir.join("settings.json"), cfg_dir.join("icons"));
            app.manage(link.clone());

            // ---- menu bar ----
            let status_i = MenuItem::with_id(app, "status", "TC001 · not connected", false, None::<&str>)?;
            let open_i = MenuItem::with_id(app, "open", "Open Dos GatOS…", true, Some("CmdOrCtrl+,"))?;
            let autostart_on = app.autolaunch().is_enabled().unwrap_or(false);
            let auto_i = CheckMenuItem::with_id(app, "autostart", "Launch at login", true, autostart_on, None::<&str>)?;
            let quit_i = MenuItem::with_id(app, "quit", "Quit Dos GatOS", true, Some("CmdOrCtrl+Q"))?;
            let menu = Menu::with_items(
                app,
                &[&status_i, &PredefinedMenuItem::separator(app)?, &open_i, &auto_i, &PredefinedMenuItem::separator(app)?, &quit_i],
            )?;
            app.manage(auto_i.clone());

            let tray = TrayIconBuilder::with_id("main")
                .icon(tray_icon(false))
                .icon_as_template(true)
                .tooltip("Dos GatOS")
                .menu(&menu)
                .show_menu_on_left_click(true)
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "open" => show_main(app),
                    "autostart" => {
                        let on = !app.autolaunch().is_enabled().unwrap_or(false);
                        let _ = set_autostart(on, app.clone());
                    }
                    "quit" => app.exit(0),
                    _ => {}
                })
                .build(app)?;

            // status → menu bar and the window
            let h = handle.clone();
            let was_connected = std::sync::Mutex::new(None::<bool>);
            link.on_change(move |s| {
                let line = status_line(s);
                let _ = status_i.set_text(&line);
                let _ = tray.set_tooltip(Some(&line));
                let up = s.connected && s.fw.is_some();
                let mut prev = was_connected.lock().unwrap();
                if *prev != Some(up) {
                    let _ = tray.set_icon(Some(tray_icon(up)));
                    let _ = tray.set_icon_as_template(true);
                    *prev = Some(up);
                }
                let _ = h.emit("status", s.clone());
            });

            api::start(link.clone());
            link.start();
            Ok(())
        })
        .on_window_event(|window, event| {
            // closing the window only hides it — the app keeps running in the menu bar
            if let WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                let _ = window.hide();
                #[cfg(target_os = "macos")]
                let _ = window.app_handle().set_activation_policy(tauri::ActivationPolicy::Accessory);
            }
        })
        .build(tauri::generate_context!())
        .expect("error while building Dos GatOS")
        .run(|_app, event| match event {
            tauri::RunEvent::ExitRequested { api, code, .. } if code.is_none() => {
                api.prevent_exit(); // keep running in the menu bar when the window is hidden
            }
            #[cfg(target_os = "macos")]
            tauri::RunEvent::Reopen { .. } => show_main(_app),
            _ => {}
        });
}
