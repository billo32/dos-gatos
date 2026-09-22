//! Лог в файл (~/Library/Logs/tc001-agent.log) + кольцевой буфер для окна настроек.

use std::collections::VecDeque;
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

const RING: usize = 300;
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
    // простая ротация: большой лог уезжает в .1
    if std::fs::metadata(path).map(|m| m.len() > MAX_FILE).unwrap_or(false) {
        let _ = std::fs::rename(path, path.with_extension("log.1"));
    }
    let file = OpenOptions::new().create(true).append(true).open(path).ok();
    let _ = LOGGER.set(Logger {
        path: path.to_path_buf(),
        file: Mutex::new(file),
        ring: Mutex::new(VecDeque::with_capacity(RING)),
    });
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
        r.push_back(line);
    }
}

pub fn tail() -> Vec<String> {
    LOGGER.get().map(|l| l.ring.lock().unwrap().iter().cloned().collect()).unwrap_or_default()
}

#[macro_export]
macro_rules! info { ($($a:tt)*) => { $crate::logbuf::log("INFO", &format!($($a)*)) } }
#[macro_export]
macro_rules! warn { ($($a:tt)*) => { $crate::logbuf::log("WARN", &format!($($a)*)) } }
