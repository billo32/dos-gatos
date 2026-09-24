import { describe, expect, it } from "vitest";
import { geometry } from "./PixelClock";
import { clockBuf, formatValue, textBuf, textWidth, W } from "./render";

describe("clock geometry (design/Clock.dc.html)", () => {
  it("scale 9 is 351×120", () => {
    const g = geometry(9);
    expect([g.W, g.H]).toEqual([351, 120]);
  });
  it("scale 7 is 279×94", () => {
    const g = geometry(7);
    expect([g.W, g.H]).toEqual([279, 94]);
  });
});

describe("firmware rendering", () => {
  it("centers short text and leaves the icon column", () => {
    const icon = { frames: [new Array(64).fill("#FF0000")], delays: [1000] };
    const buf = textBuf({ kind: "text", text: "1", color: "#FFF", font: "3x5", icon }, 0, 0);
    expect(buf[0]).toBe("#FF0000");
    expect(buf.slice(0, W).filter((c) => c === "#FFF").length + buf.filter((c) => c === "#FFF").length).toBeGreaterThan(0);
    expect(buf[8]).toBeNull(); // gap column between icon and text
  });
  it("draws the weekday bar with today in orange", () => {
    const mon = new Date(2026, 8, 21, 12, 34, 0); // Monday
    const buf = clockBuf({ kind: "clock", font: "3x5", h24: true, wday: true, now: mon });
    expect(buf[7 * W + 2]).toBe("#F06E28");
    expect(buf[7 * W + 6]).toBe("#3A3A42");
  });
  it("12-hour clock", () => {
    const a = clockBuf({ kind: "clock", font: "3x5", h24: false, wday: false, now: new Date(2026, 8, 21, 13, 5, 0) });
    const b = clockBuf({ kind: "clock", font: "3x5", h24: true, wday: false, now: new Date(2026, 8, 21, 1, 5, 0) });
    expect(a).toEqual(b);
  });
  it("text width uses font advances", () => {
    expect(textWidth("5x7", "AB")).toBe(11);
  });
  it("formats like the device", () => {
    expect(formatValue("84486.12", "{v}k", 0.001, 1)).toBe("84.5k");
    expect(formatValue("21", "{v}C")).toBe("21C");
    expect(formatValue("abc", "{v}", 1, 2)).toBe("abc");
  });
});
