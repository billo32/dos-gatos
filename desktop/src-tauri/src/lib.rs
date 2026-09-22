//! TC001 Agent — приложение в трее: связь с часами по USB, окно настроек apps, автозапуск.

mod api;
mod extract;
mod link;
pub mod logbuf;

use serde::Serialize;
use serde_json::{json, Value};
use std::path::PathBuf;
use std::sync::Arc;
use tauri::menu::{CheckMenuItem, Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Emitter, Manager, State, WindowEvent};
use tauri_plugin_autostart::{MacosLauncher, ManagerExt};

use link::{Link, Status};

const DEFAULT_APPS: &str = include_str!("../../../agent/apps.json");

// ---------- commands (окно настроек) ----------

#[derive(Serialize)]
struct TestResult {
    status: u16,
    value: Option<String>,
    formatted: Option<String>,
    error: Option<String>,
    preview: String,
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
    link.save_apps(&apps)
}

#[tauri::command]
async fn test_source(app: Value, link: State<'_, Arc<Link>>) -> Result<TestResult, String> {
    let l = link.inner().clone();
    let a = app.clone();
    let (status, result, preview) = tauri::async_runtime::spawn_blocking(move || l.run_query(&a))
        .await
        .map_err(|e| e.to_string())?;
    let (value, error) = match result {
        Ok(v) => (v, None),
        Err(e) => (None, Some(e)),
    };
    let formatted = value.as_deref().map(|v| extract::format_like_device(v, &app));
    Ok(TestResult { status, value, formatted, error, preview })
}

#[tauri::command]
fn set_brightness(v: u8, link: State<Arc<Link>>) -> Result<(), String> {
    link.send(&json!({"t": "bright", "v": v.max(1)}))
}

#[tauri::command]
fn notify(text: String, color: String, link: State<Arc<Link>>) -> Result<(), String> {
    link.send(&json!({"t": "notify", "text": text, "color": color, "dur": 6000}))
}

#[tauri::command]
fn reconnect(link: State<Arc<Link>>) {
    link.request_reconnect();
}

#[tauri::command]
fn get_log() -> Vec<String> {
    logbuf::tail()
}

#[tauri::command]
fn open_log() {
    if let Some(p) = logbuf::path() {
        open_path(&p);
    }
}

#[tauri::command]
fn open_config_dir(link: State<Arc<Link>>) {
    if let Some(dir) = link.apps_path.parent() {
        open_path(dir);
    }
}

#[tauri::command]
fn get_autostart(app: AppHandle) -> bool {
    app.autolaunch().is_enabled().unwrap_or(false)
}

#[tauri::command]
fn set_autostart(enabled: bool, app: AppHandle) -> Result<(), String> {
    let al = app.autolaunch();
    let r = if enabled { al.enable() } else { al.disable() };
    r.map_err(|e| e.to_string())?;
    if let Some(item) = app.try_state::<CheckMenuItem<tauri::Wry>>() {
        let _ = item.set_checked(enabled);
    }
    Ok(())
}

// ---------- helpers ----------

fn open_path(p: &std::path::Path) {
    let cmd = if cfg!(target_os = "macos") { "open" } else { "xdg-open" };
    let _ = std::process::Command::new(cmd).arg(p).spawn();
}

fn show_settings(app: &AppHandle) {
    if let Some(w) = app.get_webview_window("settings") {
        let _ = w.show();
        let _ = w.unminimize();
        let _ = w.set_focus();
    }
}

fn log_path(app: &AppHandle) -> PathBuf {
    #[cfg(target_os = "macos")]
    if let Ok(home) = app.path().home_dir() {
        return home.join("Library/Logs/tc001-agent.log");
    }
    app.path()
        .app_log_dir()
        .unwrap_or_else(|_| std::env::temp_dir())
        .join("tc001-agent.log")
}

fn status_line(s: &Status) -> String {
    if let Some(e) = &s.api_error {
        if !s.connected {
            return format!("⚠ {}", e.chars().take(60).collect::<String>());
        }
    }
    if s.connected && s.fw.is_some() {
        format!("● TC001 на связи · fw {}", s.fw.as_deref().unwrap_or("?"))
    } else if s.connected {
        "◐ Порт открыт, жду ответа часов…".into()
    } else {
        "○ Нет связи с TC001".into()
    }
}

fn values_line(s: &Status) -> String {
    if s.values.is_empty() {
        return "Данных пока нет".into();
    }
    s.values
        .iter()
        .map(|(k, v)| format!("{k}: {}", v.value.as_deref().unwrap_or("—")))
        .collect::<Vec<_>>()
        .join(" · ")
}

// ---------- app ----------

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| show_settings(app)))
        .plugin(tauri_plugin_autostart::init(MacosLauncher::LaunchAgent, None))
        .invoke_handler(tauri::generate_handler![
            get_status,
            get_apps,
            save_apps,
            test_source,
            set_brightness,
            notify,
            reconnect,
            get_log,
            open_log,
            open_config_dir,
            get_autostart,
            set_autostart
        ])
        .setup(|app| {
            // только иконка в трее, без Dock
            #[cfg(target_os = "macos")]
            app.set_activation_policy(tauri::ActivationPolicy::Accessory);

            let handle = app.handle().clone();
            logbuf::init(&log_path(&handle));
            info!("TC001 Agent {} starting", env!("CARGO_PKG_VERSION"));

            let cfg_dir = app.path().app_config_dir()?;
            std::fs::create_dir_all(&cfg_dir)?;
            let apps_path = cfg_dir.join("apps.json");
            if !apps_path.exists() {
                std::fs::write(&apps_path, DEFAULT_APPS)?;
                info!("создан {}", apps_path.display());
            }

            // при первом запуске включить автозапуск
            let marker = cfg_dir.join(".autostart-initialized");
            if !marker.exists() {
                if let Err(e) = app.autolaunch().enable() {
                    warn!("autostart: {e}");
                }
                let _ = std::fs::write(&marker, "");
            }

            let link = Link::new(apps_path);
            app.manage(link.clone());

            // ---- tray ----
            let status_i = MenuItem::with_id(app, "status", "○ Нет связи с TC001", false, None::<&str>)?;
            let values_i = MenuItem::with_id(app, "values", "Данных пока нет", false, None::<&str>)?;
            let settings_i = MenuItem::with_id(app, "settings", "Настройки…", true, None::<&str>)?;
            let reconnect_i = MenuItem::with_id(app, "reconnect", "Переподключить", true, None::<&str>)?;
            let log_i = MenuItem::with_id(app, "log", "Открыть лог", true, None::<&str>)?;
            let autostart_on = app.autolaunch().is_enabled().unwrap_or(false);
            let auto_i = CheckMenuItem::with_id(app, "autostart", "Запускать при входе", true, autostart_on, None::<&str>)?;
            let quit_i = MenuItem::with_id(app, "quit", "Выход", true, None::<&str>)?;
            let menu = Menu::with_items(
                app,
                &[
                    &status_i,
                    &values_i,
                    &PredefinedMenuItem::separator(app)?,
                    &settings_i,
                    &reconnect_i,
                    &log_i,
                    &auto_i,
                    &PredefinedMenuItem::separator(app)?,
                    &quit_i,
                ],
            )?;
            app.manage(auto_i.clone());

            let tray = TrayIconBuilder::with_id("main")
                .icon(tauri::image::Image::from_bytes(include_bytes!("../icons/tray.png"))?)
                .icon_as_template(true)
                .tooltip("TC001")
                .menu(&menu)
                .show_menu_on_left_click(true)
                .on_menu_event(|app, event| {
                    let link = app.state::<Arc<Link>>();
                    match event.id.as_ref() {
                        "settings" => show_settings(app),
                        "reconnect" => link.request_reconnect(),
                        "log" => open_log(),
                        "autostart" => {
                            let on = !app.autolaunch().is_enabled().unwrap_or(false);
                            let _ = set_autostart(on, app.clone());
                            let _ = app.emit("autostart", on);
                        }
                        "quit" => app.exit(0),
                        _ => {}
                    }
                })
                .build(app)?;

            // статус → меню трея, подсказка и окно настроек
            let h = handle.clone();
            link.on_change(move |s| {
                let line = status_line(s);
                let _ = status_i.set_text(&line);
                let _ = values_i.set_text(values_line(s));
                let _ = tray.set_tooltip(Some(&line));
                let _ = h.emit("status", s.clone());
            });

            api::start(link.clone());
            link.start();
            Ok(())
        })
        .on_window_event(|window, event| {
            // закрытие окна настроек только прячет его — агент продолжает работать в трее
            if let WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                let _ = window.hide();
            }
        })
        .build(tauri::generate_context!())
        .expect("error while building TC001 Agent")
        .run(|_app, event| {
            if let tauri::RunEvent::ExitRequested { api, code, .. } = event {
                if code.is_none() {
                    api.prevent_exit(); // не выходить, когда окно спрятано
                }
            }
        });
}
