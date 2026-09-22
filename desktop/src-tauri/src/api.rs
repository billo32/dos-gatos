//! Локальный HTTP API на 127.0.0.1:7765 — совместим с agent.py.
//!   curl -X POST 127.0.0.1:7765/notify -d '{"text":"Deploy OK","color":"#00FF00"}'

use serde_json::{json, Value};
use std::io::Read;
use std::sync::Arc;
use std::thread;
use tiny_http::{Header, Method, Response, Server};

use crate::link::Link;
use crate::{info, warn};

pub const API_ADDR: &str = "127.0.0.1:7765";

pub fn start(link: Arc<Link>) {
    let server = match Server::http(API_ADDR) {
        Ok(s) => s,
        Err(e) => {
            let msg = format!("порт {API_ADDR} занят ({e}) — работает старый агент? ./agent/install-launchagent.sh uninstall");
            warn!("{msg}");
            link.set_api_error(Some(msg));
            return;
        }
    };
    info!("local API on http://{API_ADDR}");
    thread::spawn(move || {
        for mut req in server.incoming_requests() {
            let mut body = String::new();
            let _ = req.as_reader().take(64 * 1024).read_to_string(&mut body);
            let (code, out) = route(&link, req.method(), req.url(), &body);
            let resp = Response::from_string(out.to_string())
                .with_status_code(code)
                .with_header(Header::from_bytes("Content-Type", "application/json").unwrap());
            let _ = req.respond(resp);
        }
    });
}

fn route(link: &Link, method: &Method, url: &str, body: &str) -> (u16, Value) {
    if *method == Method::Get && url == "/status" {
        return (200, serde_json::to_value(link.status()).unwrap_or(Value::Null));
    }
    if *method != Method::Post {
        return (404, json!({"error": "not found"}));
    }
    let b: Value = if body.trim().is_empty() {
        json!({})
    } else {
        match serde_json::from_str(body) {
            Ok(v) => v,
            Err(_) => return (400, json!({"error": "invalid json"})),
        }
    };
    let msg = match url {
        "/notify" => {
            let mut m = b.as_object().cloned().unwrap_or_default();
            m.insert("t".into(), json!("notify"));
            Value::Object(m)
        }
        "/bright" => json!({"t": "bright", "v": b.get("v").cloned().unwrap_or(json!(30))}),
        "/apps" => {
            return match link.save_apps(&b) {
                Ok(_) => (200, json!({"ok": true})),
                Err(e) => (400, json!({"error": e})),
            }
        }
        _ => return (404, json!({"error": "not found"})),
    };
    match link.send(&msg) {
        Ok(()) => (200, json!({"ok": true})),
        Err(e) => (503, json!({"error": e})),
    }
}
