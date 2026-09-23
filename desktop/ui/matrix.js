// 32×8 preview that mirrors the firmware's drawText(): same fonts, icon area, centering and scrolling.
(function () {
  const W = 32, H = 8, ICON = 8;
  const OFF = "#1c1c20";

  function glyph(font, ch) {
    const g = font.glyphs[String(ch.charCodeAt(0))];
    return g || font.glyphs["63"]; // '?'
  }

  function textWidth(fontKey, s) {
    const f = window.PIXEL_FONTS[fontKey];
    let w = 0;
    for (const ch of s) w += glyph(f, ch).adv;
    return Math.max(0, w - 1);
  }

  function drawText(px, fontKey, s, x, color, clipFrom) {
    const f = window.PIXEL_FONTS[fontKey];
    for (const ch of s) {
      const g = glyph(f, ch);
      g.rows.forEach((row, r) => {
        const y = f.baseline + g.yo + r;
        for (let c = 0; c < row.length; c++) {
          const xx = x + g.xo + c;
          if (row[c] === "1" && y >= 0 && y < H && xx >= clipFrom && xx < W) px[y * W + xx] = color;
        }
      });
      x += g.adv;
    }
  }

  class Matrix {
    constructor(canvas) {
      this.canvas = canvas;
      this.ctx = canvas.getContext("2d");
      this.timer = null;
    }

    // icon: 64 CSS colors ("" = off) or null
    show({ text, color = "#ffffff", font = "5x7", icon = null }) {
      clearInterval(this.timer);
      const x0 = icon ? ICON + 1 : 0;
      const area = W - x0;
      const w = textWidth(font, text);
      const frame = (x) => {
        const px = new Array(W * H).fill("");
        drawText(px, font, text, x, color, x0);
        if (icon) icon.forEach((c, i) => { if (c) px[(i >> 3) * W + (i & 7)] = c; });
        this.paint(px);
      };
      if (w <= area) {
        frame(x0 + Math.floor((area - w + 1) / 2));
        return;
      }
      let x = W;
      frame(x);
      this.timer = setInterval(() => {
        x -= 1;
        if (x < x0 - w) x = W;
        frame(x);
      }, 45);
    }

    stop() { clearInterval(this.timer); }

    paint(px) {
      const cw = this.canvas.clientWidth || 320;
      const ch = cw / 4;
      const dpr = window.devicePixelRatio || 1;
      if (this.canvas.width !== Math.round(cw * dpr)) {
        this.canvas.width = Math.round(cw * dpr);
        this.canvas.height = Math.round(ch * dpr);
      }
      const ctx = this.ctx;
      const cell = this.canvas.width / W;
      ctx.fillStyle = "#0e0e11";
      ctx.fillRect(0, 0, this.canvas.width, this.canvas.height);
      for (let i = 0; i < W * H; i++) {
        const x = (i % W) * cell + cell / 2, y = Math.floor(i / W) * cell + cell / 2;
        ctx.beginPath();
        ctx.arc(x, y, cell * 0.38, 0, Math.PI * 2);
        ctx.fillStyle = px[i] || OFF;
        if (px[i]) { ctx.shadowColor = px[i]; ctx.shadowBlur = cell * 0.5; } else { ctx.shadowBlur = 0; }
        ctx.fill();
      }
      ctx.shadowBlur = 0;
    }
  }

  function paintIcon(canvas, icon) {
    const ctx = canvas.getContext("2d");
    ctx.clearRect(0, 0, 8, 8);
    if (!icon) return;
    icon.forEach((c, i) => { if (c) { ctx.fillStyle = c; ctx.fillRect(i & 7, i >> 3, 1, 1); } });
  }

  window.PixelMatrix = { Matrix, textWidth, paintIcon };
})();
