//! App settings (settings.json next to apps.json): playlist clock screen, notifications screen,
//! brightness, time zone. Sources and their playlist flags (`dur`, `off`) stay in apps.json.
//! The Wi‑Fi password is never stored here — it goes straight to the clock.

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::path::{Path, PathBuf};

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct ClockScreen {
    pub on: bool,
    /// seconds on screen
    pub dur: u32,
    /// position among playlist screens (0 = first)
    pub pos: u32,
    pub h24: bool,
    pub wday: bool,
}

impl Default for ClockScreen {
    fn default() -> Self {
        Self { on: true, dur: 10, pos: 0, h24: true, wday: true }
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct NotifyScreen {
    /// accept notifications from the local API (curl 127.0.0.1:7765/notify)
    pub on: bool,
    /// seconds on screen
    pub dur: u32,
    // last test message from the settings window
    pub text: String,
    pub color: String,
    pub icon: String,
}

impl Default for NotifyScreen {
    fn default() -> Self {
        Self { on: true, dur: 6, text: "Hello".into(), color: "#4DA3FF".into(), icon: String::new() }
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Settings {
    pub clock: ClockScreen,
    pub notify: NotifyScreen,
    /// brightness, percent 0..100
    pub brightness: u8,
    /// IANA zone (e.g. "Europe/Madrid"); None = follow the Mac
    pub tz: Option<String>,
}

impl Default for Settings {
    fn default() -> Self {
        Self { clock: ClockScreen::default(), notify: NotifyScreen::default(), brightness: 12, tz: None }
    }
}

impl Settings {
    pub fn load(path: &Path) -> Self {
        std::fs::read_to_string(path)
            .ok()
            .and_then(|t| serde_json::from_str(&t).ok())
            .unwrap_or_default()
    }

    pub fn save(&self, path: &Path) -> Result<(), String> {
        let text = serde_json::to_string_pretty(self).map_err(|e| e.to_string())? + "\n";
        write_atomic(path, text.as_bytes())
    }

    /// `settings` message for the clock (playlist position/duration and look of the clock screen).
    /// `clock.pos` counts every source in apps.json; the clock only knows the enabled ones,
    /// so the position is recomputed against `apps`.
    pub fn clock_msg(&self, apps: &Value) -> Value {
        let c = &self.clock;
        let pos = apps
            .as_array()
            .map(|a| {
                a.iter()
                    .take(c.pos as usize)
                    .filter(|x| !x.get("off").and_then(Value::as_bool).unwrap_or(false))
                    .filter(|x| x.get("url").and_then(Value::as_str).is_some_and(|u| !u.is_empty()))
                    .count()
            })
            .unwrap_or(0);
        json!({"t": "settings", "clock": {"on": c.on, "dur": c.dur, "pos": pos, "h24": c.h24, "wday": c.wday}})
    }

    /// Device brightness 1..255 from percent.
    pub fn device_brightness(&self) -> u8 {
        pct_to_device(self.brightness)
    }
}

pub fn pct_to_device(pct: u8) -> u8 {
    ((pct.min(100) as u32 * 255 + 50) / 100).max(1) as u8
}

pub fn write_atomic(path: &Path, data: &[u8]) -> Result<(), String> {
    let tmp: PathBuf = path.with_extension("tmp");
    std::fs::write(&tmp, data).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, path).map_err(|e| e.to_string())
}

// ---------- time zones ----------

const ZONEINFO: &str = "/usr/share/zoneinfo";
const REGIONS: &[&str] = &["Africa", "America", "Antarctica", "Asia", "Atlantic", "Australia", "Europe", "Indian", "Pacific"];

/// IANA name of the Mac's zone, from the /etc/localtime symlink.
pub fn mac_timezone() -> Option<String> {
    let target = std::fs::read_link("/etc/localtime").ok()?;
    let s = target.to_string_lossy();
    let i = s.find("zoneinfo/")?;
    Some(s[i + "zoneinfo/".len()..].to_string())
}

pub fn valid_tz(tz: &str) -> bool {
    !tz.is_empty() && !tz.contains("..") && !tz.starts_with('/') && Path::new(ZONEINFO).join(tz).is_file()
}

/// POSIX TZ rule for an IANA zone (footer of its TZif file).
pub fn tz_posix(tz: &str) -> Option<String> {
    if !valid_tz(tz) {
        return None;
    }
    crate::link::tz_posix_from(&Path::new(ZONEINFO).join(tz))
}

pub fn list_timezones() -> Vec<String> {
    let mut out = vec!["UTC".to_string()];
    for r in REGIONS {
        walk(&Path::new(ZONEINFO).join(r), r, &mut out);
    }
    out.sort();
    out.dedup();
    out
}

fn walk(dir: &Path, prefix: &str, out: &mut Vec<String>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        let p = e.path();
        if p.is_dir() {
            walk(&p, &format!("{prefix}/{name}"), out);
        } else if name.chars().next().is_some_and(|c| c.is_ascii_uppercase()) {
            out.push(format!("{prefix}/{name}"));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_and_partial_json() {
        let s: Settings = serde_json::from_str(r#"{"clock":{"dur":15}}"#).unwrap();
        assert_eq!(s.clock.dur, 15);
        assert!(s.clock.on && s.clock.h24);
        assert_eq!(s.notify, NotifyScreen::default());
    }

    #[test]
    fn brightness_mapping() {
        assert_eq!(pct_to_device(0), 1);
        assert_eq!(pct_to_device(100), 255);
        assert_eq!(pct_to_device(50), 128);
    }

    #[test]
    fn clock_message() {
        let mut s = Settings::default();
        s.clock.pos = 2;
        let apps = json!([{"name": "a", "url": "http://x"}, {"name": "b", "url": "http://y", "off": true}, {"name": "c", "url": "http://z"}]);
        let m = s.clock_msg(&apps);
        assert_eq!(m["t"], "settings");
        assert_eq!(m["clock"]["dur"], 10);
        assert_eq!(m["clock"]["pos"], 1, "disabled sources don't count");
    }

    #[test]
    fn tz_validation() {
        assert!(!valid_tz("../etc/passwd"));
        assert!(!valid_tz("/etc/passwd"));
        if Path::new("/usr/share/zoneinfo/Europe/Madrid").exists() {
            assert_eq!(tz_posix("Europe/Madrid").as_deref(), Some("CET-1CEST,M3.5.0,M10.5.0/3"));
            assert!(list_timezones().contains(&"Europe/Madrid".to_string()));
        }
    }

    #[test]
    fn roundtrip_file() {
        let dir = std::env::temp_dir().join(format!("dg-settings-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("settings.json");
        let mut s = Settings::default();
        s.tz = Some("Europe/Madrid".into());
        s.save(&p).unwrap();
        assert_eq!(Settings::load(&p), s);
        let _ = std::fs::remove_dir_all(dir);
    }
}
