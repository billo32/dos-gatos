//! Value extraction from an API response — same rules as the firmware and agent.py.
//! find/keep: window after a substring; path: "a.b.0.c" in JSON; re: first group or the whole match.

use serde_json::Value;

pub fn extract(
    text: &str,
    path: Option<&str>,
    find: Option<&str>,
    keep: usize,
    re: Option<&str>,
) -> Result<Option<String>, String> {
    let mut cur: String = text.to_string();

    if let Some(needle) = find.filter(|s| !s.is_empty()) {
        match cur.find(needle) {
            None => return Ok(None),
            Some(i) => cur = cur[i..].chars().take(keep).collect(),
        }
    }

    if let Some(p) = path.filter(|s| !s.is_empty()) {
        let root: Value = serde_json::from_str(&cur).map_err(|e| format!("json: {e}"))?;
        let mut node = &root;
        for part in p.split('.') {
            node = match node {
                Value::Array(a) => {
                    let idx: usize = part.parse().map_err(|_| format!("path: '{part}' is not an array index"))?;
                    a.get(idx).ok_or_else(|| format!("path: index {idx} out of range"))?
                }
                Value::Object(o) => o.get(part).ok_or_else(|| format!("path: no key '{part}'"))?,
                _ => return Err(format!("path: '{part}' — not an object or array")),
            };
        }
        cur = match node {
            Value::String(s) => s.clone(),
            other => other.to_string(),
        };
    }

    if let Some(r) = re.filter(|s| !s.is_empty()) {
        let rx = regex::Regex::new(r).map_err(|e| format!("re: {e}"))?;
        return Ok(rx.captures(&cur).map(|c| {
            c.get(1).or_else(|| c.get(0)).map(|m| m.as_str().to_string()).unwrap_or_default()
        }));
    }
    Ok(Some(cur))
}

/// Formatting as on the clock (scale/dec/fmt) — for the settings preview.
pub fn format_like_device(value: &str, app: &Value) -> String {
    let scale = app.get("scale").and_then(Value::as_f64).unwrap_or(1.0);
    let dec = app.get("dec").and_then(Value::as_i64).unwrap_or(-1);
    let mut v = value.to_string();
    if dec >= 0 || scale != 1.0 {
        if let Ok(num) = value.trim().parse::<f64>() {
            let d = if dec >= 0 { dec as usize } else { 2 };
            v = format!("{:.*}", d, num * scale);
        }
    }
    let fmt = app.get("fmt").and_then(Value::as_str).unwrap_or("{v}");
    fmt.replace("{v}", &v)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    const METEO: &str = r#"{"current_units":{"temperature_2m":"°C"},"current":{"time":"x","temperature_2m":24.6}}"#;
    const RAIN: &str = r#"{"hourly_units":{"precipitation_probability":"%"},"hourly":{"precipitation_probability":[35,10]}}"#;

    #[test]
    fn path_number() {
        assert_eq!(extract(METEO, Some("current.temperature_2m"), None, 128, None).unwrap().unwrap(), "24.6");
    }

    #[test]
    fn path_array_index() {
        assert_eq!(extract(RAIN, Some("hourly.precipitation_probability.1"), None, 128, None).unwrap().unwrap(), "10");
    }

    #[test]
    fn find_keep_re() {
        let v = extract(RAIN, None, Some("\"precipitation_probability\":["), 40, Some(r"\[(\d+)")).unwrap();
        assert_eq!(v.unwrap(), "35");
    }

    #[test]
    fn find_missing_is_none() {
        assert_eq!(extract(METEO, None, Some("nope"), 40, None).unwrap(), None);
    }

    #[test]
    fn bad_path_is_error() {
        assert!(extract(METEO, Some("current.wind"), None, 128, None).is_err());
    }

    #[test]
    fn format_scale_dec() {
        let app = json!({"fmt": "{v}k", "scale": 0.001, "dec": 1});
        assert_eq!(format_like_device("113245.17", &app), "113.2k");
        let app = json!({"fmt": "{v}C", "dec": 0});
        assert_eq!(format_like_device("24.6", &app), "25C");
    }
}
