//! Icons: LaMetric catalog id → 8×8 RGB565 frames (animated GIFs keep up to 16 frames), cached on disk.
//! The clock stores them in LittleFS, so they keep working in Wi-Fi mode without the agent.

use image::{imageops::FilterType, AnimationDecoder, DynamicImage, RgbaImage};
use serde::{Deserialize, Serialize};
use std::io::Cursor;
use std::path::PathBuf;
use std::time::Duration;

pub const ICON: u32 = 8;
pub const MAX_FRAMES: usize = 16;
const LAMETRIC_URL: &str = "https://developer.lametric.com/content/apps/icon_thumbs/";

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct IconData {
    /// frames of 64 RGB565 pixels (0 = off)
    pub frames: Vec<Vec<u16>>,
    /// per-frame delay, ms
    pub delays: Vec<u16>,
}

/// For the settings window: CSS colors per pixel ("" = off) and delays.
#[derive(Serialize)]
pub struct IconPreview {
    pub frames: Vec<Vec<String>>,
    pub delays: Vec<u16>,
}

pub fn valid_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 16 && id.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}

/// All frames as one hex string (256 chars per frame) — the `px` field of the `icon` message.
pub fn to_hex(icon: &IconData) -> String {
    icon.frames.iter().flatten().map(|p| format!("{p:04X}")).collect()
}

fn css(p: u16) -> String {
    if p == 0 {
        return String::new();
    }
    let r = ((p >> 11) & 0x1F) as u32 * 255 / 31;
    let g = ((p >> 5) & 0x3F) as u32 * 255 / 63;
    let b = (p & 0x1F) as u32 * 255 / 31;
    format!("#{r:02X}{g:02X}{b:02X}")
}

pub fn preview(icon: &IconData) -> IconPreview {
    IconPreview {
        frames: icon.frames.iter().map(|f| f.iter().map(|&p| css(p)).collect()).collect(),
        delays: icon.delays.clone(),
    }
}

fn rgb565(r: u8, g: u8, b: u8) -> u16 {
    ((r as u16 & 0xF8) << 8) | ((g as u16 & 0xFC) << 3) | (b as u16 >> 3)
}

fn to_pixels(frame: RgbaImage) -> Vec<u16> {
    let img = if frame.width() != ICON || frame.height() != ICON {
        DynamicImage::ImageRgba8(frame).resize_exact(ICON, ICON, FilterType::Nearest).to_rgba8()
    } else {
        frame
    };
    img.pixels()
        .map(|p| {
            let [r, g, b, a] = p.0;
            if a < 128 { 0 } else { rgb565(r, g, b).max(1) } // 0 = off on the clock
        })
        .collect()
}

/// GIF: all (composited) frames, at most MAX_FRAMES — longer animations are thinned out evenly,
/// keeping the total duration. PNG/JPEG: one frame.
pub fn decode(bytes: &[u8]) -> Result<IconData, String> {
    if !bytes.starts_with(b"GIF8") {
        let img = image::load_from_memory(bytes).map_err(|e| e.to_string())?.to_rgba8();
        return Ok(IconData { frames: vec![to_pixels(img)], delays: vec![1000] });
    }
    let dec = image::codecs::gif::GifDecoder::new(Cursor::new(bytes)).map_err(|e| e.to_string())?;
    let mut frames = Vec::new();
    let mut delays: Vec<u32> = Vec::new();
    for f in dec.into_frames() {
        let f = f.map_err(|e| e.to_string())?;
        let (n, d) = f.delay().numer_denom_ms();
        let ms = if d == 0 { 100 } else { n / d };
        delays.push(if ms < 20 { 100 } else { ms }); // browsers treat 0–10 ms as 100 ms
        frames.push(to_pixels(f.into_buffer()));
    }
    if frames.is_empty() {
        return Err("empty GIF".into());
    }
    if frames.len() > MAX_FRAMES {
        let total: u32 = delays.iter().sum();
        let step = frames.len() as f32 / MAX_FRAMES as f32;
        let picked: Vec<usize> = (0..MAX_FRAMES).map(|i| (i as f32 * step) as usize).collect();
        frames = picked.iter().map(|&i| frames[i].clone()).collect();
        delays = vec![total / MAX_FRAMES as u32; MAX_FRAMES];
    }
    // identical frames in a row → one frame with the summed delay (saves flash and airtime)
    let mut out = IconData { frames: Vec::new(), delays: Vec::new() };
    for (f, d) in frames.into_iter().zip(delays) {
        if out.frames.last() == Some(&f) {
            let last = out.delays.last_mut().unwrap();
            *last = last.saturating_add(d.min(u16::MAX as u32) as u16);
        } else {
            out.frames.push(f);
            out.delays.push(d.min(u16::MAX as u32) as u16);
        }
    }
    Ok(out)
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

    /// Cached icon or download from LaMetric. The cache keeps the original file,
    /// so decoding changes (e.g. animation support) apply without re-downloading.
    pub fn get(&self, id: &str) -> Result<IconData, String> {
        if !valid_id(id) {
            return Err(format!("invalid icon id '{id}'"));
        }
        let path = self.dir.join(format!("{id}.img"));
        let bytes = match std::fs::read(&path) {
            Ok(b) => b,
            Err(_) => {
                let resp = self.http.get(format!("{LAMETRIC_URL}{id}")).send().map_err(|e| e.to_string())?;
                if !resp.status().is_success() {
                    return Err(format!("LaMetric: HTTP {} for icon {id}", resp.status().as_u16()));
                }
                let b = resp.bytes().map_err(|e| e.to_string())?.to_vec();
                let _ = std::fs::write(&path, &b);
                b
            }
        };
        decode(&bytes).map_err(|e| format!("icon {id}: {e}"))
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

    fn gif(frames: Vec<(RgbaImage, u32)>) -> Vec<u8> {
        let mut buf = Vec::new();
        {
            let mut enc = GifEncoder::new(&mut buf);
            enc.encode_frames(frames.into_iter().map(|(img, ms)| Frame::from_parts(img, 0, 0, Delay::from_numer_denom_ms(ms, 1))))
                .unwrap();
        }
        buf
    }

    #[test]
    fn png_single_frame() {
        let mut buf = Vec::new();
        DynamicImage::ImageRgba8(sample(8, 8)).write_to(&mut Cursor::new(&mut buf), ImageFormat::Png).unwrap();
        let ic = decode(&buf).unwrap();
        assert_eq!(ic.frames.len(), 1);
        assert_eq!(ic.frames[0][0], 0, "transparent pixel is off");
        assert_eq!(ic.frames[0][1], rgb565(255, 30, 0));
        assert_eq!(to_hex(&ic).len(), 256);
        assert_eq!(&preview(&ic).frames[0][1][..3], "#FF");
    }

    #[test]
    fn gif_all_frames_with_delays() {
        let blue = RgbaImage::from_pixel(16, 16, Rgba([0, 0, 255, 255]));
        let ic = decode(&gif(vec![(sample(16, 16), 100), (blue, 250)])).unwrap();
        assert_eq!(ic.frames.len(), 2);
        assert_eq!(ic.delays, vec![100, 250]);
        assert!(ic.frames[0].iter().any(|&p| (p >> 11) > 20), "frame 1 is red-ish");
        assert!(ic.frames[1].iter().all(|&p| p == rgb565(0, 0, 255)), "frame 2 is blue");
        assert_eq!(to_hex(&ic).len(), 512);
    }

    #[test]
    fn gif_long_animation_is_thinned() {
        let frames = (0..40u8).map(|i| (RgbaImage::from_pixel(8, 8, Rgba([i * 6, 0, 0, 255])), 50)).collect();
        let ic = decode(&gif(frames)).unwrap();
        assert!(ic.frames.len() <= MAX_FRAMES);
        let total: u32 = ic.delays.iter().map(|&d| d as u32).sum();
        assert!((1900..=2000).contains(&total), "keeps ~2 s total, got {total}");
    }

    #[test]
    fn gif_repeated_frames_merge() {
        let a = RgbaImage::from_pixel(8, 8, Rgba([255, 0, 0, 255]));
        let b = RgbaImage::from_pixel(8, 8, Rgba([0, 255, 0, 255]));
        let ic = decode(&gif(vec![(a.clone(), 100), (a, 100), (b, 100)])).unwrap();
        assert_eq!(ic.frames.len(), 2);
        assert_eq!(ic.delays, vec![200, 100]);
    }

    #[test]
    fn ids() {
        assert!(valid_id("2422"));
        assert!(!valid_id("../x"));
        assert!(!valid_id(""));
    }
}
