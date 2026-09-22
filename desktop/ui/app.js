// Окно настроек TC001 Agent. Работает через window.__TAURI__ (withGlobalTauri).
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const $ = (id) => document.getElementById(id);
const FIELDS = ["name", "url", "every", "path", "re", "find", "keep", "fmt", "color", "scale", "dec"];
const NUMERIC = new Set(["every", "keep", "scale", "dec"]);

let apps = [];
let sel = -1;
let dirty = false;
let lastStatus = null;
let appsRev = null;
let savingSelf = false;

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
  $("dirty").textContent = v ? "Есть несохранённые изменения" : "";
}

function esc(s) {
  return String(s ?? "").replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
}

// ---------- status ----------
async function reloadApps() {
  apps = await invoke("get_apps");
  select(Math.min(Math.max(sel, 0), apps.length - 1));
}

function renderStatus(s) {
  lastStatus = s;
  if (appsRev !== null && s.apps_rev !== appsRev && !dirty && !savingSelf) reloadApps().catch(() => {});
  appsRev = s.apps_rev;
  const dot = $("dot");
  dot.className = "dot " + (s.connected && s.fw ? "ok" : s.connected ? "wait" : s.api_error ? "bad" : "");
  $("statusText").textContent = s.connected && s.fw ? "TC001 на связи" : s.connected ? "Порт открыт, жду ответа часов…" : "Нет связи с TC001";
  const sub = [];
  if (s.port) sub.push(s.port);
  if (s.fw) sub.push("fw " + s.fw);
  if (s.last_rx_ago != null && s.connected) sub.push(`ответ ${s.last_rx_ago} с назад`);
  if (s.rtc) sub.push(s.rtc);
  if (s.api_error) sub.push("⚠ " + s.api_error);
  $("statusSub").textContent = sub.join(" · ");
  renderList();
}

// ---------- apps list / editor ----------
function renderList() {
  const ul = $("appList");
  const vals = (lastStatus && lastStatus.values) || {};
  ul.innerHTML = apps
    .map((a, i) => {
      const v = vals[a.name];
      const val = v ? (v.value ?? `ошибка ${v.status}`) : "";
      return `<li data-i="${i}" class="${i === sel ? "sel" : ""}">
        <span class="name"><span class="swatch" style="background:${esc(a.color || "#ffffff")}"></span>${esc(a.name || "(без имени)")}</span>
        <span class="val" title="${v ? "обновлено " + esc(v.at) : ""}">${esc(val)}</span></li>`;
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
  if (i < 0) return;
  const a = apps[i];
  for (const f of FIELDS) {
    const el = form.elements[f];
    el.value = a[f] ?? (f === "color" ? "#ffffff" : "");
  }
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
    if (!a.name) return "У каждого источника должно быть имя";
    if (names.has(a.name)) return `Имя «${a.name}» повторяется`;
    names.add(a.name);
    if (!/^https?:\/\//.test(a.url || "")) return `«${a.name}»: URL должен начинаться с http(s)://`;
  }
  if (list.length > 8) return "На часах помещается не больше 8 источников";
  return null;
}

$("appList").addEventListener("click", (e) => {
  const li = e.target.closest("li");
  if (li) select(Number(li.dataset.i));
});

$("form").addEventListener("input", () => {
  if (sel < 0) return;
  apps[sel] = readForm();
  renderList();
  setDirty(true);
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

$("test").addEventListener("click", async () => {
  if (sel < 0) return;
  const a = readForm();
  const btn = $("test");
  btn.disabled = true;
  btn.textContent = "Запрос…";
  try {
    const r = await invoke("test_source", { app: a });
    $("result").hidden = false;
    const ok = r.status >= 200 && r.status < 300 && r.value != null;
    const led = $("ledText");
    led.textContent = ok ? r.formatted : "—";
    led.style.color = a.color || "#ffffff";
    const meta = $("resultMeta");
    meta.className = "meta" + (ok ? "" : " err");
    meta.textContent = ok
      ? `HTTP ${r.status} · извлечено: ${r.value}`
      : r.error
        ? `Ошибка: ${r.error}`
        : r.status >= 200 && r.status < 300
          ? `HTTP ${r.status}, но значение не найдено — проверь find/path/regex`
          : `HTTP ${r.status || "нет ответа"}`;
    $("preview").textContent = r.preview || "";
    $("result").scrollIntoView({ block: "nearest", behavior: "smooth" });
  } catch (e) {
    toast("Ошибка: " + e);
  } finally {
    btn.disabled = false;
    btn.textContent = "Проверить источник";
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
    toast(sent ? "Сохранено и отправлено на часы" : "Сохранено. Отправится, когда часы подключатся");
  } catch (e) {
    savingSelf = false;
    toast("Не сохранилось: " + e);
  }
});

$("openCfg").addEventListener("click", () => invoke("open_config_dir"));

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
  toast("Переподключаюсь…");
});

// ---------- notify / log ----------
$("nSend").addEventListener("click", () => {
  invoke("notify", { text: $("nText").value, color: $("nColor").value.toUpperCase() })
    .then(() => toast("Отправлено"))
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
