//! Лог в файл (~/Library/Logs/DosGatOS/dosgatos.log) + кольцевой буфер для окна настроек.

use std::collections::VecDeque;
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

const RING: usize = 1000;
const MAX_FILE: u64 = 2 * 1024 * 1024;

struct Logger {
    path: PathBuf,
    file: Mutex<Option<File>>,
    ring: Mutex<VecDeque<String>>,
}

static LOGGER: OnceLock<Logger> = OnceLock::new();

pub fn init(path: &Path) {
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    rotate(path);
    let file = OpenOptions::new().create(true).append(true).open(path).ok();
    let _ = LOGGER.set(Logger {
        path: path.to_path_buf(),
        file: Mutex::new(file),
        ring: Mutex::new(VecDeque::with_capacity(RING)),
    });
}

/// Daily files: yesterday's log becomes `<name>.YYYY-MM-DD.log`; files older than 7 days are removed.
/// A log bigger than MAX_FILE is rotated right away.
fn rotate(path: &Path) {
    let Ok(meta) = std::fs::metadata(path) else { return };
    let modified: chrono::DateTime<chrono::Local> = meta.modified().map(Into::into).unwrap_or_else(|_| chrono::Local::now());
    let today = chrono::Local::now().date_naive();
    if modified.date_naive() < today || meta.len() > MAX_FILE {
        let stem = path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
        let mut dst = path.with_file_name(format!("{stem}.{}.log", modified.format("%Y-%m-%d")));
        if dst.exists() {
            dst = path.with_file_name(format!("{stem}.{}.log", modified.format("%Y-%m-%d-%H%M%S")));
        }
        let _ = std::fs::rename(path, dst);
    }
    if let Some(dir) = path.parent() {
        let week = std::time::Duration::from_secs(7 * 24 * 3600);
        for e in std::fs::read_dir(dir).into_iter().flatten().flatten() {
            let p = e.path();
            let old = e.metadata().and_then(|m| m.modified()).map(|t| t.elapsed().unwrap_or_default() > week).unwrap_or(false);
            if old && p != path && p.extension().is_some_and(|x| x == "log") {
                let _ = std::fs::remove_file(p);
            }
        }
    }
}

pub fn path() -> Option<PathBuf> {
    LOGGER.get().map(|l| l.path.clone())
}

pub fn log(level: &str, msg: &str) {
    let line = format!("{} {} {}", chrono::Local::now().format("%H:%M:%S"), level, msg);
    eprintln!("{line}");
    if let Some(l) = LOGGER.get() {
        if let Some(f) = l.file.lock().unwrap().as_mut() {
            let _ = writeln!(f, "{line}");
        }
        let mut r = l.ring.lock().unwrap();
        if r.len() == RING {
            r.pop_front();
        }
        r.push_back(line.clone());
    }
    if let Some(h) = HOOK.get() {
        h(&line);
    }
}

type Hook = Box<dyn Fn(&str) + Send + Sync>;
static HOOK: OnceLock<Hook> = OnceLock::new();

/// Called with every new line (the settings window gets it as a `log` event).
pub fn on_line(f: impl Fn(&str) + Send + Sync + 'static) {
    let _ = HOOK.set(Box::new(f));
}

pub fn clear() {
    if let Some(l) = LOGGER.get() {
        l.ring.lock().unwrap().clear();
    }
}

pub fn tail() -> Vec<String> {
    LOGGER.get().map(|l| l.ring.lock().unwrap().iter().cloned().collect()).unwrap_or_default()
}

#[macro_export]
macro_rules! info { ($($a:tt)*) => { $crate::logbuf::log("INFO", &format!($($a)*)) } }
#[macro_export]
macro_rules! warn { ($($a:tt)*) => { $crate::logbuf::log("WARN", &format!($($a)*)) } }
#[macro_export]
macro_rules! error { ($($a:tt)*) => { $crate::logbuf::log("ERROR", &format!($($a)*)) } }
