//! Icons: LaMetric catalog id → 8×8 RGB565 for the clock, cached on disk.
//! The clock stores them in LittleFS, so they keep working in Wi-Fi mode without the agent.

use image::{imageops::FilterType, AnimationDecoder, DynamicImage, RgbaImage};
use std::io::Cursor;
use std::path::{Path, PathBuf};
use std::time::Duration;

pub const ICON: u32 = 8;
const LAMETRIC_URL: &str = "https://developer.lametric.com/content/apps/icon_thumbs/";

pub fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 16 && id.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}

/// 8×8 RGB565 as 256 hex chars — the format of the `icon` message.
pub fn to_hex(px: &[u16]) -> String {
    px.iter().map(|p| format!("{p:04X}")).collect()
}

/// `#RRGGBB` per pixel (empty string = off) for the preview canvas.
pub fn to_css(px: &[u16]) -> Vec<String> {
    px.iter()
        .map(|&p| {
            if p == 0 {
                return String::new();
            }
            let r = ((p >> 11) & 0x1F) as u32 * 255 / 31;
            let g = ((p >> 5) & 0x3F) as u32 * 255 / 63;
            let b = (p & 0x1F) as u32 * 255 / 31;
            format!("#{r:02X}{g:02X}{b:02X}")
        })
        .collect()
}

fn rgb565(r: u8, g: u8, b: u8) -> u16 {
    ((r as u16 & 0xF8) << 8) | ((g as u16 & 0xFC) << 3) | (b as u16 >> 3)
}

/// First frame (GIF) or the image itself, scaled to 8×8; transparent pixels are off.
pub fn decode(bytes: &[u8]) -> Result<Vec<u16>, String> {
    let frame: RgbaImage = if bytes.starts_with(b"GIF8") {
        let dec = image::codecs::gif::GifDecoder::new(Cursor::new(bytes)).map_err(|e| e.to_string())?;
        dec.into_frames()
            .next()
            .ok_or("empty GIF")?
            .map_err(|e| e.to_string())?
            .into_buffer()
    } else {
        image::load_from_memory(bytes).map_err(|e| e.to_string())?.to_rgba8()
    };
    let img = if frame.width() != ICON || frame.height() != ICON {
        DynamicImage::ImageRgba8(frame).resize_exact(ICON, ICON, FilterType::Nearest).to_rgba8()
    } else {
        frame
    };
    Ok(img
        .pixels()
        .map(|p| {
            let [r, g, b, a] = p.0;
            if a < 128 { 0 } else { rgb565(r, g, b).max(1) } // 0 = off on the clock
        })
        .collect())
}

pub struct IconStore {
    dir: PathBuf,
    http: reqwest::blocking::Client,
}

impl IconStore {
    pub fn new(dir: PathBuf) -> Self {
        let _ = std::fs::create_dir_all(&dir);
        let http = reqwest::blocking::Client::builder()
            .user_agent("dos-gatos-agent")
            .timeout(Duration::from_secs(15))
            .build()
            .expect("http client");
        Self { dir, http }
    }

    fn cache_path(&self, id: &str) -> PathBuf {
        self.dir.join(format!("{id}.hex"))
    }

    fn read_cache(path: &Path) -> Option<Vec<u16>> {
        let s = std::fs::read_to_string(path).ok()?;
        let s = s.trim();
        if s.len() != (ICON * ICON * 4) as usize {
            return None;
        }
        (0..s.len()).step_by(4).map(|i| u16::from_str_radix(&s[i..i + 4], 16).ok()).collect()
    }

    /// Cached icon or download from LaMetric.
    pub fn get(&self, id: &str) -> Result<Vec<u16>, String> {
        if !valid_id(id) {
            return Err(format!("invalid icon id '{id}'"));
        }
        let path = self.cache_path(id);
        if let Some(px) = Self::read_cache(&path) {
            return Ok(px);
        }
        let resp = self.http.get(format!("{LAMETRIC_URL}{id}")).send().map_err(|e| e.to_string())?;
        if !resp.status().is_success() {
            return Err(format!("LaMetric: HTTP {} for icon {id}", resp.status().as_u16()));
        }
        let bytes = resp.bytes().map_err(|e| e.to_string())?;
        let px = decode(&bytes).map_err(|e| format!("icon {id}: {e}"))?;
        let _ = std::fs::write(&path, to_hex(&px));
        Ok(px)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{codecs::gif::GifEncoder, Delay, Frame, ImageFormat, Rgba};

    fn sample(w: u32, h: u32) -> RgbaImage {
        RgbaImage::from_fn(w, h, |x, y| {
            if x == 0 && y == 0 {
                Rgba([0, 0, 0, 0]) // transparent
            } else {
                Rgba([255, (x * 30) as u8, (y * 30) as u8, 255])
            }
        })
    }

    #[test]
    fn png_8x8_roundtrip() {
        let mut buf = Vec::new();
        DynamicImage::ImageRgba8(sample(8, 8)).write_to(&mut Cursor::new(&mut buf), ImageFormat::Png).unwrap();
        let px = decode(&buf).unwrap();
        assert_eq!(px.len(), 64);
        assert_eq!(px[0], 0, "transparent pixel is off");
        assert_eq!(px[1], rgb565(255, 30, 0));
        let hex = to_hex(&px);
        assert_eq!(hex.len(), 256);
        assert_eq!(&to_css(&px)[1][..3], "#FF");
    }

    #[test]
    fn gif_first_frame_and_resize() {
        let mut buf = Vec::new();
        {
            let mut enc = GifEncoder::new(&mut buf);
            let f1 = Frame::from_parts(sample(16, 16), 0, 0, Delay::from_numer_denom_ms(100, 1));
            let f2 = Frame::from_parts(RgbaImage::from_pixel(16, 16, Rgba([0, 0, 255, 255])), 0, 0, Delay::from_numer_denom_ms(100, 1));
            enc.encode_frames(vec![f1, f2]).unwrap();
        }
        let px = decode(&buf).unwrap();
        assert_eq!(px.len(), 64);
        assert!(px.iter().any(|&p| (p >> 11) > 20), "first frame (red-ish), not the blue second one");
    }

    #[test]
    fn black_pixel_stays_on() {
        assert_eq!(rgb565(0, 0, 0).max(1), 1);
    }

    #[test]
    fn ids() {
        assert!(valid_id("2422"));
        assert!(!valid_id("../x"));
        assert!(!valid_id(""));
    }
}
