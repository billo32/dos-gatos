// TC001 preview: body geometry from design/Clock.dc.html (renderVals), LEDs drawn as one SVG,
// pixels from render.ts (the firmware's fonts and layout).
import { useMemo, useSyncExternalStore } from "react";
import { H, W, isAnimated, sceneBuf, type Scene } from "./render";

// ---- one shared timer for every clock on screen; runs only while the window is visible and focused ----
const TICK_MS = 45; // firmware scroll step
let tick = 0;
let timer: number | undefined;
const subs = new Set<() => void>();
const active = () => document.visibilityState === "visible" && document.hasFocus();
function sync() {
  const want = subs.size > 0 && active();
  if (want && timer === undefined) {
    timer = window.setInterval(() => {
      tick++;
      subs.forEach((f) => f());
    }, TICK_MS);
  } else if (!want && timer !== undefined) {
    clearInterval(timer);
    timer = undefined;
  }
}
if (typeof window !== "undefined") {
  window.addEventListener("focus", sync);
  window.addEventListener("blur", sync);
  document.addEventListener("visibilitychange", sync);
}
function subscribe(f: () => void) {
  subs.add(f);
  sync();
  return () => {
    subs.delete(f);
    sync();
  };
}
const still = () => () => {};
const getTick = () => tick;
const zero = () => 0;

export interface PixelClockProps {
  scene: Scene;
  scale?: number;
  body?: "white" | "graphite" | "none";
  animate?: boolean;
  label?: string;
}

export function PixelClock({ scene, scale = 9, body = "white", animate = true, label }: PixelClockProps) {
  const live = animate && isAnimated(scene);
  const t = useSyncExternalStore(live ? subscribe : still, live ? getTick : zero);
  const buf = sceneBuf(scene, t * TICK_MS, t);

  const g = useMemo(() => geometry(scale, body), [scale, body]);
  const { s, gap } = g;
  const lit = buf.map((c, i) => (c ? { c, x: (i % W) * (s + gap), y: Math.floor(i / W) * (s + gap) } : null)).filter(Boolean) as { c: string; x: number; y: number }[];
  const mW = W * s + (W - 1) * gap;
  const mH = H * s + (H - 1) * gap;
  const glow = s >= 6 ? Math.round(s * 0.3) : 0;
  const white = body !== "graphite";

  return (
    <div className="relative shrink-0" style={{ width: g.W, height: g.H }} role="img" aria-label={label ?? "Clock preview"}>
      {!g.bare && (
        <>
          <div className="absolute top-0 flex" style={{ left: g.btnL, gap: g.btnGap }}>
            {[0, 1, 2].map((i) => (
              <div
                key={i}
                style={{ width: g.btnW, height: g.btnH + 2, background: white ? "#ECE9E3" : "#1B1B1E", borderRadius: `${g.btnR}px ${g.btnR}px 0 0`, boxShadow: "inset 0 1px 0 rgba(255,255,255,0.6)" }}
              />
            ))}
          </div>
          <div
            className="absolute"
            style={{ left: g.dx, top: g.btnH + g.dy, width: g.bodyW, height: g.bodyH, background: white ? "#E1DDD5" : "#151517", borderRadius: g.faceR + g.rim, boxShadow: `${g.dx}px ${s}px ${s * 2.5}px rgba(28,26,23,0.18)` }}
          />
        </>
      )}
      <div
        className="absolute left-0 box-border"
        style={{
          top: g.btnH,
          width: g.bodyW,
          height: g.bodyH,
          padding: g.rim,
          background: g.bare ? "transparent" : white ? "#FBFAF7" : "#2A2A2E",
          borderRadius: g.faceR + g.rim,
          boxShadow: g.bare ? "none" : white ? "inset 0 0 0 1px rgba(28,26,23,0.07), inset 0 -2px 0 rgba(28,26,23,0.05)" : "inset 0 0 0 1px rgba(255,255,255,0.06), inset 0 1px 0 rgba(255,255,255,0.12)",
        }}
      >
        <div
          className="box-border h-full w-full"
          style={{
            padding: `${g.vp}px ${g.hp}px`,
            background: "linear-gradient(180deg, #18181B 0%, #070708 36%, #030304 100%)",
            borderRadius: g.faceR,
            boxShadow: "inset 0 1px 0 rgba(255,255,255,0.10), inset 0 0 0 1px rgba(0,0,0,0.7)",
          }}
        >
          <svg width={mW} height={mH} viewBox={`0 0 ${mW} ${mH}`} className="block overflow-visible" aria-hidden="true">
            <defs>
              <pattern id={`off-${s}-${gap}`} width={s + gap} height={s + gap} patternUnits="userSpaceOnUse">
                <rect width={s} height={s} rx={g.pr} fill="#0D0D0F" />
              </pattern>
              {glow > 0 && (
                <filter id={`glow-${s}`} x="-50%" y="-50%" width="200%" height="200%">
                  <feGaussianBlur stdDeviation={glow / 2} />
                </filter>
              )}
            </defs>
            <rect width={mW} height={mH} fill={`url(#off-${s}-${gap})`} />
            {glow > 0 && (
              <g filter={`url(#glow-${s})`} opacity={0.4}>
                {lit.map((p, i) => <rect key={i} x={p.x} y={p.y} width={s} height={s} fill={p.c} />)}
              </g>
            )}
            {lit.map((p, i) => <rect key={i} x={p.x} y={p.y} width={s} height={s} rx={g.pr} fill={p.c} />)}
          </svg>
        </div>
      </div>
    </div>
  );
}

/** design/Clock.dc.html → renderVals(). Scale 9 → 351×120, scale 7 → 279×94. */
export function geometry(scale: number, body: "white" | "graphite" | "none" = "white") {
  const s = Math.max(2, scale);
  const bare = body === "none";
  const gap = Math.max(1, Math.round(s * 0.16));
  const mW = 32 * s + 31 * gap, mH = 8 * s + 7 * gap;
  const hp = bare ? Math.round(s * 0.5) : Math.round(s * 1.0);
  const vp = bare ? Math.round(s * 0.5) : Math.round(s * 1.5);
  const rim = bare ? 0 : Math.max(2, Math.round(s * 0.3));
  const dx = bare ? 0 : Math.round(s * 0.9), dy = bare ? 0 : Math.round(s * 0.35);
  const btnH = bare ? 0 : Math.round(s * 0.4);
  const bodyW = mW + 2 * hp + 2 * rim, bodyH = mH + 2 * vp + 2 * rim;
  const faceR = bare ? Math.round(s * 0.6) : Math.round(s * 0.9);
  const btnW = Math.round(s * 2.2), btnGap = Math.round(s * 0.8);
  return {
    s, gap, bare, hp, vp, rim, dx, dy, btnH, bodyW, bodyH, faceR, btnW, btnGap,
    btnR: Math.round(s * 0.3),
    btnL: bodyW - (3 * btnW + 2 * btnGap) - s * 3,
    pr: Math.max(0, Math.round(s * 0.08)),
    W: bodyW + dx,
    H: btnH + bodyH + dy,
  };
}
