#!/usr/bin/env python3
"""Генерация шрифтов для прошивки и превью в приложении.

    python3 tools/gen_fonts.py <путь к Adafruit-GFX-Library>

Источники:
  5x7  — glcdfont.c из Adafruit GFX (BSD-2-Clause), встроенный шрифт библиотеки
  4x6  — X11 misc-fixed 4x6 (public domain), tools/fonts/4x6.bdf
  3x5  — Tom Thumb из Adafruit GFX Fonts/TomThumb.h (BSD-3-Clause)

Результат:
  firmware/src/Font4x6.h   — GFXfont для 4x6 (5x7 и 3x5 уже есть в Adafruit GFX)
  desktop/ui/fonts.js      — все три шрифта для рендера превью 32×8 в окне настроек
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIRST, LAST = 0x20, 0x7E


def load_classic(lib: Path):
    """glcdfont.c: 5 байт на символ, столбцы, бит 0 — верхняя строка."""
    src = (lib / "glcdfont.c").read_text()
    body = src[src.index("{", src.index("font[]")) + 1: src.index("};", src.index("font[]"))]
    data = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", body)]
    glyphs = {}
    for c in range(FIRST, LAST + 1):
        cols = data[c * 5: c * 5 + 5]
        rows = ["".join("1" if (cols[x] >> y) & 1 else "0" for x in range(5)) for y in range(8)]
        glyphs[c] = {"w": 5, "h": 8, "xo": 0, "yo": -8, "adv": 6, "rows": rows}
    return {"baseline": 8, "height": 8, "glyphs": glyphs}


def load_gfx_header(path: Path, name: str):
    """Adafruit GFXfont-заголовок: bitmap + glyph-таблица."""
    src = path.read_text()
    bm_src = src[src.index("{", src.index(f"{name}Bitmaps")) + 1: src.index("};", src.index(f"{name}Bitmaps"))]
    bitmap = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", re.sub(r"/\*.*?\*/", "", bm_src))]
    gl_src = src[src.index("{", src.index(f"{name}Glyphs")) + 1: src.index("};", src.index(f"{name}Glyphs"))]
    entries = re.findall(r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}", gl_src)
    first = int(re.search(rf"{name}Glyphs,\s*(0x[0-9A-Fa-f]+|\d+)", src).group(1), 0)
    glyphs = {}
    for i, (off, w, h, adv, xo, yo) in enumerate(map(lambda e: tuple(map(int, e)), entries)):
        c = first + i
        if c < FIRST or c > LAST:
            continue
        bits = "".join(f"{b:08b}" for b in bitmap[off: off + (w * h + 7) // 8 + 1])
        rows = [bits[r * w:(r + 1) * w] for r in range(h)]
        glyphs[c] = {"w": w, "h": h, "xo": xo, "yo": yo, "adv": adv, "rows": rows}
    return {"baseline": 6, "height": 6, "glyphs": glyphs}


def load_bdf(path: Path):
    glyphs = {}
    lines = path.read_text().splitlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith("STARTCHAR"):
            enc = w = h = xo = yo = adv = None
            rows = []
            i += 1
            while not lines[i].startswith("ENDCHAR"):
                ln = lines[i]
                if ln.startswith("ENCODING"):
                    enc = int(ln.split()[1])
                elif ln.startswith("DWIDTH"):
                    adv = int(ln.split()[1])
                elif ln.startswith("BBX"):
                    w, h, xo, yo = map(int, ln.split()[1:5])
                elif ln == "BITMAP":
                    for j in range(h):
                        i += 1
                        v = int(lines[i], 16)
                        nbits = len(lines[i]) * 4
                        rows.append(f"{v:0{nbits}b}"[:w])
                i += 1
            if enc is not None and FIRST <= enc <= LAST:
                # BDF: yo — низ глифа относительно базовой линии; GFX: верх глифа
                glyphs[enc] = {"w": w, "h": h, "xo": xo, "yo": -(yo + h), "adv": adv, "rows": rows}
        i += 1
    return {"baseline": 6, "height": 6, "glyphs": glyphs}


def gfx_header(font, name: str) -> str:
    bitmap, table = [], []
    for c in range(FIRST, LAST + 1):
        g = font["glyphs"][c]
        bits = "".join(g["rows"])
        off = len(bitmap)
        for k in range(0, len(bits), 8):
            bitmap.append(int(bits[k:k + 8].ljust(8, "0"), 2))
        table.append(f"    {{{off}, {g['w']}, {g['h']}, {g['adv']}, {g['xo']}, {g['yo']}}}, /* 0x{c:02X} {chr(c)!r} */")
    items = [f"0x{b:02X}" for b in bitmap]
    bm_lines = "\n".join("    " + ", ".join(items[k:k + 16]) + "," for k in range(0, len(items), 16))
    return f"""// Сгенерировано tools/gen_fonts.py — не редактировать вручную.
// X11 misc-fixed 4x6, public domain ("Public domain font. Share and enjoy.")
#pragma once
#include <Adafruit_GFX.h>

const uint8_t {name}Bitmaps[] PROGMEM = {{
{bm_lines}
}};

const GFXglyph {name}Glyphs[] PROGMEM = {{
{chr(10).join(table)}
}};

const GFXfont {name} PROGMEM = {{(uint8_t *){name}Bitmaps, (GFXglyph *){name}Glyphs, 0x{FIRST:02X}, 0x{LAST:02X}, 6}};
"""


def main():
    lib = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if not lib or not (lib / "glcdfont.c").exists():
        sys.exit("usage: gen_fonts.py <path to Adafruit-GFX-Library>")
    fonts = {
        "5x7": load_classic(lib),
        "4x6": load_bdf(ROOT / "tools/fonts/4x6.bdf"),
        "3x5": load_gfx_header(lib / "Fonts/TomThumb.h", "TomThumb"),
    }
    for k, f in fonts.items():
        missing = [c for c in range(FIRST, LAST + 1) if c not in f["glyphs"]]
        assert not missing, (k, missing)
    (ROOT / "firmware/src/Font4x6.h").write_text(gfx_header(fonts["4x6"], "Font4x6"))
    js = {k: {"baseline": f["baseline"], "glyphs": {str(c): g for c, g in f["glyphs"].items()}} for k, f in fonts.items()}
    (ROOT / "desktop/ui/fonts.js").write_text(
        "// Сгенерировано tools/gen_fonts.py. 5x7: Adafruit GFX (BSD-2), 4x6: X11 misc-fixed (public domain),\n"
        "// 3x5: Tom Thumb (BSD-3, Brian J. Swetland, Vassilii Khachaturov, Robey Pointer).\n"
        "window.PIXEL_FONTS = " + json.dumps(js, separators=(",", ":")) + ";\n")
    for k, f in fonts.items():
        widths = sorted({g["adv"] for g in f["glyphs"].values()})
        print(f"{k}: {len(f['glyphs'])} glyphs, advance {widths}")


if __name__ == "__main__":
    main()
