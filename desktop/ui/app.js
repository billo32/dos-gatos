// TC001 Agent settings window. Uses window.__TAURI__ (withGlobalTauri).
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;
const { Matrix, paintIcon } = window.PixelMatrix;

const $ = (id) => document.getElementById(id);
const FIELDS = ["name", "url", "every", "path", "re", "find", "keep", "fmt", "color", "scale", "dec", "icon", "font"];
const NUMERIC = new Set(["every", "keep", "scale", "dec"]);
const FONTS = [
  ["3x5", "Blocky 3×5"],
  ["4x6", "Compact 4×6"],
  ["5x7", "Classic 5×7"],
];

let apps = [];
let sel = -1;
let dirty = false;
let lastStatus = null;
let appsRev = null;
let savingSelf = false;
const iconCache = new Map(); // id -> 64 CSS colors | Error message
const led = new Matrix($("led"));

// ---------- helpers ----------
function toast(msg, ms = 2600) {
  const t = $("toast");
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toast._t);
  toast._t = setTimeout(() => (t.hidden = true), ms);
}

function setDirty(v) {
  dirty = v;
  $("save").disabled = !v;
  $("dirty").textContent = v ? "Unsaved changes" : "";
}

function esc(s) {
  return String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

const deviceFont = () => (lastStatus && lastStatus.font) || "3x5";

async function getIcon(id) {
  id = (id || "").trim();
  if (!id) return null;
  if (iconCache.has(id)) {
    const v = iconCache.get(id);
    if (typeof v === "string") throw new Error(v);
    return v;
  }
  try {
    const px = await invoke("icon_preview", { id });
    iconCache.set(id, px);
    return px;
  } catch (e) {
    iconCache.set(id, String(e));
    throw new Error(String(e));
  }
}

// ---------- status ----------
async function reloadApps() {
  apps = await invoke("get_apps");
  select(Math.min(Math.max(sel, 0), apps.length - 1));
}

function renderStatus(s) {
  const fontChanged = !lastStatus || lastStatus.font !== s.font;
  lastStatus = s;
  if (appsRev !== null && s.apps_rev !== appsRev && !dirty && !savingSelf) reloadApps().catch(() => {});
  appsRev = s.apps_rev;
  const dot = $("dot");
  dot.className = "dot " + (s.connected && s.fw ? "ok" : s.connected ? "wait" : s.api_error ? "bad" : "");
  $("statusText").textContent = s.connected && s.fw ? "TC001 connected via USB" : s.connected ? "Port open, waiting for the clock…" : "TC001 not connected";
  const sub = [];
  if (s.port) sub.push(s.port);
  if (s.fw) sub.push("fw " + s.fw);
  if (s.last_rx_ago != null && s.connected) sub.push(`last reply ${s.last_rx_ago}s ago`);
  if (s.rtc) sub.push(s.rtc);
  if (s.api_error) sub.push("⚠ " + s.api_error);
  $("statusSub").textContent = sub.join(" · ");
  renderList();
  renderWifi(s);
  if (fontChanged) renderFonts();
}

// ---------- sources ----------
function renderList() {
  const ul = $("appList");
  const vals = (lastStatus && lastStatus.values) || {};
  ul.innerHTML = apps
    .map((a, i) => {
      const v = vals[a.name];
      const val = v ? (v.value ?? `error ${v.status}`) : "";
      return `<li data-i="${i}" class="${i === sel ? "sel" : ""}">
        <span class="name"><span class="swatch" style="background:${esc(a.color || "#ffffff")}"></span>${esc(a.name || "(unnamed)")}</span>
        <span class="val" title="${v ? "updated " + esc(v.at) : ""}">${esc(val)}</span></li>`;
    })
    .join("");
}

function select(i) {
  sel = i;
  renderList();
  const form = $("form");
  $("emptyHint").hidden = i >= 0;
  form.hidden = i < 0;
  $("result").hidden = true;
  led.stop();
  if (i < 0) return;
  const a = apps[i];
  for (const f of FIELDS) {
    const el = form.elements[f];
    el.value = a[f] ?? (f === "color" ? "#ffffff" : "");
  }
  refreshIconPreview();
}

function readForm() {
  const form = $("form");
  const a = {};
  for (const f of FIELDS) {
    let v = form.elements[f].value.trim();
    if (v === "") continue;
    if (NUMERIC.has(f)) {
      const n = Number(v);
      if (Number.isNaN(n)) continue;
      v = n;
    }
    a[f] = v;
  }
  if (a.color) a.color = a.color.toUpperCase();
  return a;
}

function validate(list) {
  const names = new Set();
  for (const a of list) {
    if (!a.name) return "Every source needs a name";
    if (names.has(a.name)) return `Name “${a.name}” is used twice`;
    names.add(a.name);
    if (!/^https?:\/\//.test(a.url || "")) return `“${a.name}”: URL must start with http(s)://`;
    if (a.icon && !/^[A-Za-z0-9_-]{1,16}$/.test(a.icon)) return `“${a.name}”: icon ID looks wrong`;
  }
  if (list.length > 8) return "The clock holds up to 8 sources";
  return null;
}

let iconT;
async function refreshIconPreview() {
  const id = $("form").elements.icon.value.trim();
  const cv = $("iconPreview");
  cv.title = id ? "Loading…" : "No icon";
  cv.classList.toggle("empty", !id);
  paintIcon(cv, null);
  if (!id) return;
  try {
    paintIcon(cv, await getIcon(id));
    cv.title = `LaMetric icon ${id}`;
  } catch (e) {
    cv.title = e.message;
    cv.classList.add("bad");
    setTimeout(() => cv.classList.remove("bad"), 1500);
  }
}

$("appList").addEventListener("click", (e) => {
  const li = e.target.closest("li");
  if (li) select(Number(li.dataset.i));
});

$("form").addEventListener("input", (e) => {
  if (sel < 0) return;
  apps[sel] = readForm();
  renderList();
  setDirty(true);
  if (e.target.name === "icon") {
    clearTimeout(iconT);
    iconT = setTimeout(refreshIconPreview, 500);
  }
});

$("addApp").addEventListener("click", () => {
  apps.push({ name: `app${apps.length + 1}`, url: "https://", every: 300, fmt: "{v}", color: "#FFFFFF" });
  select(apps.length - 1);
  setDirty(true);
  $("form").elements.url.focus();
});

$("del").addEventListener("click", () => {
  if (sel < 0) return;
  apps.splice(sel, 1);
  select(Math.min(sel, apps.length - 1));
  setDirty(true);
});

$("browseIcons").addEventListener("click", () => invoke("open_icon_gallery"));

$("test").addEventListener("click", async () => {
  if (sel < 0) return;
  const a = readForm();
  const btn = $("test");
  btn.disabled = true;
  btn.textContent = "Fetching…";
  try {
    const r = await invoke("test_source", { app: a });
    $("result").hidden = false;
    const ok = r.status >= 200 && r.status < 300 && r.value != null;
    let icon = null;
    if (a.icon) icon = await getIcon(a.icon).catch(() => null);
    led.show({ text: ok ? r.formatted : "--", color: ok ? a.color || "#ffffff" : "#555555", font: a.font || deviceFont(), icon });
    const meta = $("resultMeta");
    meta.className = "meta" + (ok ? "" : " err");
    meta.textContent = ok
      ? `HTTP ${r.status} · extracted: ${r.value}`
      : r.error
        ? `Error: ${r.error}`
        : r.status >= 200 && r.status < 300
          ? `HTTP ${r.status}, but nothing matched — check find / JSON path / regex`
          : `HTTP ${r.status || "no response"}`;
    $("preview").textContent = r.preview || "";
    $("result").scrollIntoView({ block: "nearest", behavior: "smooth" });
  } catch (e) {
    toast("Error: " + e);
  } finally {
    btn.disabled = false;
    btn.textContent = "Test source";
  }
});

$("save").addEventListener("click", async () => {
  const err = validate(apps);
  if (err) return toast(err);
  try {
    savingSelf = true;
    const sent = await invoke("save_apps", { apps });
    savingSelf = false;
    setDirty(false);
    toast(sent ? "Saved and sent to the clock" : "Saved. Will be sent when the clock connects");
  } catch (e) {
    savingSelf = false;
    toast("Couldn't save: " + e);
  }
});

$("openCfg").addEventListener("click", () => invoke("open_config_dir"));

// ---------- device: Wi-Fi ----------
function renderWifi(s) {
  const w = s.wifi;
  const el = $("wifiState");
  if (!s.connected || !s.fw) {
    el.textContent = "Connect the clock over USB to change Wi‑Fi settings.";
    el.className = "wifi-state muted";
  } else if (!w || w.state === "off" || !w.ssid) {
    el.textContent = "Wi‑Fi is off — the clock only works while the agent is connected.";
    el.className = "wifi-state muted";
  } else if (w.state === "connected") {
    el.textContent = `Connected to “${w.ssid}” · ${w.ip}${w.rssi != null ? ` · ${w.rssi} dBm` : ""}`;
    el.className = "wifi-state ok";
  } else {
    el.textContent = `Connecting to “${w.ssid}”…`;
    el.className = "wifi-state wait";
  }
  const ssidEl = $("wSsid");
  if (w && w.ssid && document.activeElement !== ssidEl && !ssidEl.dataset.touched) ssidEl.value = w.ssid;
  const usable = s.connected && !!s.fw;
  for (const id of ["wSave", "wOff"]) $(id).disabled = !usable;
}

$("wSsid").addEventListener("input", (e) => (e.target.dataset.touched = "1"));

$("wSave").addEventListener("click", async () => {
  const ssid = $("wSsid").value.trim();
  if (!ssid) return toast("Enter the network name");
  try {
    await invoke("set_wifi", { ssid, pass: $("wPass").value });
    $("wPass").value = "";
    delete $("wSsid").dataset.touched;
    toast("Sent. The clock is connecting…");
  } catch (e) {
    toast(String(e));
  }
});

$("wOff").addEventListener("click", async () => {
  try {
    await invoke("set_wifi", { ssid: "", pass: "" });
    toast("Wi‑Fi turned off on the clock");
  } catch (e) {
    toast(String(e));
  }
});

// ---------- device: fonts ----------
const fontMatrices = [];
function renderFonts() {
  const box = $("fontChoices");
  if (!box.childElementCount) {
    for (const [key, label] of FONTS) {
      const b = document.createElement("button");
      b.className = "font-choice";
      b.dataset.font = key;
      b.innerHTML = `<canvas width="32" height="8" class="led small"></canvas><span>${label}</span>`;
      b.addEventListener("click", async () => {
        try {
          await invoke("set_font", { font: key });
          toast(`Default font: ${label}`);
        } catch (e) {
          toast(String(e));
        }
      });
      box.appendChild(b);
      fontMatrices.push([key, new Matrix(b.querySelector("canvas"))]);
    }
  }
  const now = new Date();
  const hhmm = `${String(now.getHours()).padStart(2, "0")}:${String(now.getMinutes()).padStart(2, "0")}`;
  const weekday = (now.getDay() + 6) % 7;
  for (const [key, m] of fontMatrices) m.show({ text: hhmm, font: key, color: "#fff0dc", weekday });
  const current = lastStatus && lastStatus.fw ? lastStatus.font : null;
  box.querySelectorAll(".font-choice").forEach((b) => {
    b.classList.toggle("active", b.dataset.font === current);
    b.disabled = !(lastStatus && lastStatus.connected && lastStatus.fw);
  });
}

// ---------- header controls ----------
let brightT;
$("bright").addEventListener("input", (e) => {
  clearTimeout(brightT);
  brightT = setTimeout(() => invoke("set_brightness", { v: Number(e.target.value) }).catch((x) => toast(String(x))), 120);
});

$("autostart").addEventListener("change", (e) => {
  invoke("set_autostart", { enabled: e.target.checked }).catch((x) => toast(String(x)));
});

$("reconnect").addEventListener("click", () => {
  invoke("reconnect");
  toast("Reconnecting…");
});

// ---------- notify / log ----------
$("nSend").addEventListener("click", () => {
  const icon = $("nIcon").value.trim() || null;
  invoke("notify", { text: $("nText").value, color: $("nColor").value.toUpperCase(), icon })
    .then(() => toast("Sent"))
    .catch((e) => toast(String(e)));
});

$("openLog").addEventListener("click", () => invoke("open_log"));

async function refreshLog() {
  const lines = await invoke("get_log");
  const el = $("log");
  const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 20;
  el.textContent = lines.join("\n");
  if (atBottom) el.scrollTop = el.scrollHeight;
}

// ---------- tabs ----------
let logTimer;
document.querySelectorAll(".tab").forEach((t) =>
  t.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((x) => x.classList.toggle("active", x === t));
    document.querySelectorAll(".tab-body").forEach((b) => b.classList.toggle("active", b.id === "tab-" + t.dataset.tab));
    $("save").parentElement.hidden = t.dataset.tab !== "apps";
    clearInterval(logTimer);
    if (t.dataset.tab === "log") {
      refreshLog();
      logTimer = setInterval(refreshLog, 2000);
    }
    if (t.dataset.tab === "device") renderFonts();
  })
);

// ---------- init ----------
(async () => {
  try {
    apps = await invoke("get_apps");
  } catch (e) {
    toast("apps.json: " + e, 6000);
    apps = [];
  }
  select(apps.length ? 0 : -1);
  renderStatus(await invoke("get_status"));
  $("autostart").checked = await invoke("get_autostart");
  await listen("status", (e) => renderStatus(e.payload));
  await listen("autostart", (e) => ($("autostart").checked = e.payload));
})();
