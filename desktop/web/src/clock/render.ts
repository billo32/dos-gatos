// 32×8 frame buffers that mirror what the firmware draws (firmware/src/main.cpp):
// same bitmap fonts (fonts.json is generated from the firmware fonts), icon area, centering,
// scrolling, clock screen with the weekday bar.
import fontData from "./fonts.json";
import type { FontId, IconPreview } from "../api";

export const W = 32;
export const H = 8;
const ICON = 8;
export type Buf = (string | null)[];

interface Glyph { w: number; h: number; xo: number; yo: number; adv: number; rows: string[] }
interface Font { baseline: number; glyphs: Record<string, Glyph> }
const FONTS = fontData as Record<FontId, Font>;

export const CLOCK_COLOR = "#FFF0DC"; // firmware: Color(255, 240, 220)
export const WEEK_ON = "#F06E28";
export const WEEK_OFF = "#3A3A42";

export const blank = (): Buf => new Array(W * H).fill(null);

// «°» → degree glyph: 0xF8 in 5x7, '`' in 3x5 and 4x6 (as in the firmware)
const glyphText = (font: FontId, s: string) => s.replace(/°/g, font === "5x7" ? "ø" : "`");

function glyph(f: Font, ch: string): Glyph {
  return f.glyphs[String(ch.charCodeAt(0))] || f.glyphs["63"]; // '?'
}

export function textWidth(font: FontId, s: string): number {
  const f = FONTS[font];
  let w = 0;
  for (const ch of glyphText(font, s)) w += glyph(f, ch).adv;
  return Math.max(0, w - 1);
}

function drawText(buf: Buf, font: FontId, s: string, x: number, color: string, clipFrom: number) {
  const f = FONTS[font];
  for (const ch of glyphText(font, s)) {
    const g = glyph(f, ch);
    g.rows.forEach((row, r) => {
      const y = f.baseline + g.yo + r;
      for (let c = 0; c < row.length; c++) {
        const xx = x + g.xo + c;
        if (row[c] === "1" && y >= 0 && y < H && xx >= clipFrom && xx < W) buf[y * W + xx] = color;
      }
    });
    x += g.adv;
  }
}

/** Frame of an (animated) icon at time `ms` — same choice as Icon::frame() in the firmware. */
export function iconFrame(icon: IconPreview, ms: number): string[] {
  const { frames, delays } = icon;
  if (frames.length <= 1) return frames[0] || [];
  const total = delays.reduce((a, b) => a + b, 0);
  if (!total) return frames[0];
  let t = ms % total;
  for (let i = 0; i < frames.length; i++) {
    if (t < delays[i]) return frames[i];
    t -= delays[i];
  }
  return frames[frames.length - 1];
}

export interface TextScene { kind: "text"; text: string; color: string; font: FontId; icon?: IconPreview | null }
export interface ClockScene { kind: "clock"; font: FontId; h24: boolean; wday: boolean; now?: Date }
export type Scene = TextScene | ClockScene;

/** Does this scene change over time (scrolling text, animated icon, clock)? */
export function isAnimated(scene: Scene): boolean {
  if (scene.kind === "clock") return true;
  const x0 = scene.icon ? ICON + 1 : 0;
  return textWidth(scene.font, scene.text) > W - x0 || (scene.icon?.frames.length ?? 0) > 1;
}

/** Firmware drawText(): centered if it fits, otherwise scrolling from the right edge (1 px per tick). */
export function textBuf(scene: TextScene, ms: number, tick: number): Buf {
  const buf = blank();
  const x0 = scene.icon ? ICON + 1 : 0;
  const area = W - x0;
  const w = textWidth(scene.font, scene.text);
  let x: number;
  if (w <= area) x = x0 + Math.floor((area - w + 1) / 2);
  else {
    const span = W - (x0 - w) + 1;
    x = W - (tick % span);
  }
  drawText(buf, scene.font, scene.text, x, scene.color, x0);
  if (scene.icon) iconFrame(scene.icon, ms).forEach((c, i) => { if (c) buf[(i >> 3) * W + (i & 7)] = c; });
  return buf;
}

/** Firmware drawClock(): HH:MM, colon blanked on odd seconds, weekday bar (Monday first). */
export function clockBuf(scene: ClockScene): Buf {
  const buf = blank();
  const t = scene.now ?? new Date();
  let hh = t.getHours();
  if (!scene.h24) hh = hh % 12 || 12;
  const s = `${String(hh).padStart(2, "0")}:${String(t.getMinutes()).padStart(2, "0")}`;
  const w = textWidth(scene.font, s);
  const x = Math.floor((W - w + 1) / 2);
  drawText(buf, scene.font, s, x, CLOCK_COLOR, 0);
  if (t.getSeconds() % 2) {
    const f = FONTS[scene.font];
    const cx = x + glyph(f, s[0]).adv + glyph(f, s[1]).adv;
    const cw = glyph(f, ":").adv - 1;
    for (let y = 0; y < H - 1; y++) for (let i = 0; i < cw; i++) buf[y * W + cx + i] = null;
  }
  if (scene.wday) {
    const today = (t.getDay() + 6) % 7; // Monday = 0
    for (let d = 0; d < 7; d++) for (let k = 0; k < 3; k++) buf[7 * W + 2 + d * 4 + k] = d === today ? WEEK_ON : WEEK_OFF;
  }
  return buf;
}

export function sceneBuf(scene: Scene, ms: number, tick: number): Buf {
  return scene.kind === "clock" ? clockBuf(scene) : textBuf(scene, ms, tick);
}

/** Value formatting as on the clock (extract::format_like_device). */
export function formatValue(raw: string, fmt?: string, scale?: number, dec?: number): string {
  let v = raw;
  const sc = scale ?? 1;
  const d = dec ?? -1;
  if (d >= 0 || sc !== 1) {
    const n = Number(raw.trim());
    if (raw.trim() !== "" && Number.isFinite(n)) v = (n * sc).toFixed(d >= 0 ? d : 2);
  }
  return (fmt || "{v}").split("{v}").join(v);
}
