# Dos GatOS desktop app — build spec for a coding agent

Build the **Dos GatOS** macOS menu-bar app. It configures and feeds data to a Ulanzi TC001 clock running the Dos GatOS firmware over USB serial (Wi‑Fi is the fallback). It replaces the current Python host agent and its LaunchAgent.

The design is final and lives in `design/` as `.dc.html` mockups. Implement it faithfully. Where this file and a mockup disagree, the mockup wins on visuals and this file wins on behavior.

Repo: `github.com/billo32/dos-gatos` (private), cloned at `~/Projects/dos-gatos`. Put the app in `app/` inside that repo unless the owner says otherwise.

---

## 0. Ground rules

1. **The existing Python host agent is the source of truth for the device protocol.** Read it before writing any Rust: serial framing, commands, apps.json format, time sync, heartbeat, VID/PID or port matching. Port that behavior 1:1. **Do not invent protocol messages.** If a UI control needs a command the firmware doesn't have yet, still build the control, disable it with a tooltip ("Needs firmware support"), and add `// TODO(firmware): <what is needed>`.
2. **Keep the existing `apps.json` format** so current clocks and configs keep working. If the UI needs extra fields, add them as optional keys, or keep them in the app's own `settings.json`.
3. No CI runs on this repo. Every build, sign and notarize step must work locally on the owner's Mac with one command.
4. Never invent real-world facts in UI copy (versions, dates, counts). Use the mockup copy.

---

## 1. Scope

| Surface | Mockup | Notes |
|---|---|---|
| Main window: **Playlist** | `design/AppWindow.dc.html` | Default tab |
| Main window: **Sources** | `design/AppSources.dc.html` | REST sources, modal editor |
| Main window: **Extensions** | `design/AppExtensions.dc.html` | Install / remove from a catalog |
| Main window: **Device** | `design/AppDevice.dc.html` | Settings rows + Wi‑Fi modal |
| Main window: **Log** | `design/AppLog.dc.html` | Live log |
| Menu-bar (tray) icon + menu | — (spec in §6) | Always running |
| Clock preview component | `design/Clock.dc.html` | Pixel-exact 32×8 renderer |
| App icon / tray icon | `design/Mark.dc.html`, `design/Logo.dc.html` | Two-cat pixel mark |

Out of scope: accounts, a remote marketplace backend (use a bundled catalog, see §7.3), Windows/Linux builds (keep the code portable, but ship macOS only), and notification forwarding from macOS (the Notifications *screen* exists in the playlist; its data path is `TODO`).

---

## 2. Stack

- **Tauri 2** (Rust stable). macOS 13+. Universal binary (`aarch64` + `x86_64`).
- Rust crates:
  - `tokio`
  - `serialport` or `tokio-serial` (whichever matches the Python agent's needs)
  - `reqwest` with `rustls`
  - `serde` / `serde_json`
  - `regex`
  - `jsonpath-rust` for the JSON path step, or a hand-rolled dot/index walker (see §5)
  - `tracing` + `tracing-appender`
- Tauri plugins:
  - `autostart` (Launch at login)
  - `dialog` (Export log)
  - `shell` or `opener` ("Show apps.json in Finder")
  - `single-instance`
  - `updater` is optional; leave it wired but disabled until there is a release feed
- Frontend: **Vite + React 19 + TypeScript (strict) + Tailwind CSS v4**. No UI kit. Fonts are self-hosted via `@fontsource-variable/onest`, `…/unbounded` and `…/jetbrains-mono`.
- State: a small store (Zustand or `useSyncExternalStore`) fed by Tauri events. All device and source I/O lives in Rust. The UI only calls commands and listens to events.

---

## 3. How to read the mockups

- Each `.dc.html` is a self-contained page. Markup is inside `<x-dc>`, and its **inline styles are the visual spec**: sizes, colors, radii, spacing. Translate them to Tailwind utilities with the tokens in §4. Don't paste inline styles.
- `{{name}}` values come from the `<script type="text/x-dc">` class's `renderVals()`. That script shows the intended state and interactions. Port the logic, not the `DCLogic` class.
- `<sc-for>` is `.map()`, `<sc-if>` is conditional render, and `<dc-import name="Clock">` renders the clock preview component.
- `href="AppSources.dc.html"` etc. means "switch to that tab".
- `Clock.dc.html` → `glyphs()`, `compose()` and `renderVals()` are the **source of truth for the LED preview**. Port them line for line (§8).

---

## 4. Design tokens (same as the website — share one `tokens.css` if both live in the repo)

```css
@theme {
  --color-ink: #1C1A17; --color-milk: #FAF7F2; --color-surface: #FFFFFF;
  --color-sand: #F6F2EC; --color-sand-2: #F4F1EB; --color-titlebar: #F4F1EB;
  --color-line: #E7E1D7; --color-line-soft: #EFEAE2; --color-field: #DCD5C9;
  --color-muted: #5F584F; --color-subtle: #6A635A;
  --color-ginger: #F0642D; --color-ginger-text: #B8431A; --color-ginger-deep: #8F3212; --color-ginger-soft: #FDE7DC;
  --color-select: #FFF4EC;      /* selected row */
  --color-nav-active: #EFE7DB;  /* active sidebar item */
  --color-toggle-on: #E2551F; --color-toggle-off: #D6D0C6;
  --color-ok: #2BB673; --color-ok-text: #1E7A45; --color-danger: #B3261E;
  --font-display: "Unbounded Variable", sans-serif;
  --font-sans: "Onest Variable", sans-serif;
  --font-mono: "JetBrains Mono Variable", monospace;
}
```

- **Type:** page title in display font, 22 px / 600. Row title 14 / 600. Meta 12–13 in `subtle`. Eyebrows in mono, 11 px, uppercase, +0.08em.
- **Controls:**
  - Inputs are 40 px high with radius 10 and a 1 px `field` border.
  - Buttons are 38 px with radius 9. Primary = ink bg + milk text. Secondary = white + `field` border. Danger = text-only `danger`.
  - Switches are 44×26 with a 20 px white knob; the track is `toggle-on`/`toggle-off`.
  - The gear button is 36×36 and icon-only (stroke 1.7), with an `aria-label` of "<Name> settings".
- **Window chrome:** use a native titlebar with `titleBarStyle: "Overlay"` + `hiddenTitle`, and draw the 44 px sand strip with the centered "Dos GatOS" title as in the mockup. Leave room for the traffic lights (~70 px). The sidebar is 220 px on `milk` with a device card on top and 5 nav items.
- **Focus:** `focus-visible:outline-2 outline-offset-2 outline-ginger` everywhere.
- **Dark mode:** not in scope. Force the light appearance (`"theme": "Light"` in the window config).

---

## 5. Rust core

```
src-tauri/src/
  main.rs            // setup, tray, windows, plugin init, single-instance
  state.rs           // AppState: Arc<RwLock<…>> for settings, sources, playlist, device
  device/            // port discovery, connect/reconnect, framing, commands — ported from the Python agent
  sources/           // poller + extraction pipeline
  playlist.rs        // order, enabled, duration, per-screen settings → device
  catalog.rs         // bundled extension catalog + install state
  logbuf.rs          // ring buffer (1 000 lines) + file sink
  commands.rs        // #[tauri::command] surface (§5.4)
```

### 5.1 Device manager
- Discover the TC001 exactly the way the Python agent does (port-name pattern and/or VID/PID). Auto-connect on launch.
- Reconnect loop with backoff (1 s → 10 s) when the port disappears. "Reconnect" in the UI forces a reconnect attempt now.
- Keep `DeviceStatus { connected, port, fw_version, last_reply_at, rtc_drift_s, wifi: Off|Standby|Active }` and emit `device://status` on every change and at least every 2 s while connected (the sidebar shows "replied N s ago").
- Time sync: same cadence and method as the Python agent (the mockup shows the result, e.g. "RTC drift −1 s · ok").

### 5.2 Sources poller + extraction pipeline
One Tokio task per enabled source, scheduled by `refresh` seconds with ±10 % jitter. HTTP GET with a 10 s timeout. On HTTP 429/5xx, retry with backoff (60 s, then doubling up to the refresh interval). Keep the last good value.

The pipeline runs in this order. **If the Python agent's semantics differ, match the Python agent.**
1. **JSON path** (optional): dot path with `[n]` indexes (`current.temperature_2m`, `data.amount`, `hourly.precipitation_probability[0]`). Numbers that come as strings (`"84486.12"`) are parsed.
2. **Find + keep** (optional): locate the first occurrence of `find` in the raw text (or in the JSON-path result stringified) and keep the next `keep` chars after the match.
3. **Regex** (optional): the first capture group if there is one, else the whole match.
4. Parse as `f64`, multiply by `multiply`, round to `decimals`.
5. **Format:** replace every `{v}` in `format` with the value.
6. Send the resulting text, color, icon and font to the clock (over USB). With Wi‑Fi fallback on, also sync the source definitions so the clock can poll by itself when USB is gone. That is the existing behavior: "USB always wins".

`test_source(def)` runs one request and returns `{ status, ms, bytes, body_preview (first 4 KB), highlighted_range?, steps: [{name, output}], result }`. This powers the **Test** button in the modal.

Emit `source://value { id, raw, text, at, ok, error? }` after every poll.

### 5.3 Persistence
- `~/Library/Application Support/<bundle-id>/apps.json`: sources, in the **existing** format.
- `…/settings.json`: playlist order, enabled flags, durations, per-screen settings, installed extensions, UI prefs.
- Write atomically (temp file + rename). Watch `apps.json` for external edits and reload.
- "Show apps.json in Finder" reveals that file.
- Never store the Wi‑Fi password on the Mac. Send it to the clock and forget it (the mockup copy promises "Stored only on the clock").

### 5.4 Commands (names are a suggestion; keep them stable)
`get_state`, `reconnect`, `set_brightness(u8)`, `set_auto_brightness(bool)`, `set_font(id)`, `set_timezone(tz)`, `set_24h(bool)`, `set_launch_at_login(bool)`, `wifi_set(ssid, password)`, `wifi_off()`, `restart_clock()`, `check_firmware()`, `playlist_reorder(ids)`, `playlist_set_enabled(id, bool)`, `playlist_save_item(id, settings)`, `playlist_remove(id)`, `sources_list()`, `source_save(def)`, `source_delete(id)`, `test_source(def)`, `catalog_list()`, `extension_install(id)`, `extension_remove(id)`, `log_tail(n)`, `log_clear()`, `log_export(path)`.

Every command that talks to the device returns `Result<_, AppError>`, with the error mapped to a short human message the UI shows inline.

---

## 6. Menu-bar icon, windows, lifecycle

- **Activation policy: Accessory.** No Dock icon; the app lives in the menu bar. When the main window is open, temporarily switch to Regular so it shows in Cmd‑Tab, and switch back on close.
- **Tray icon:** the two-cat mark (`Mark.dc.html`, variant `mono`) rendered as a **template image** (16 pt, @1x/@2x PNG, black on transparent), so macOS tints it. When the device is disconnected, show a variant with a small dot or a dimmed icon.
- **Tray menu:**
  1. Status line (disabled item), e.g. "TC001 · USB · replied 2 s ago" or "TC001 · not connected".
  2. "Next screen". Only if the firmware supports it; otherwise omit.
  3. Separator
  4. "Open Dos GatOS…" (⌘,)
  5. "Launch at login" (check item, mirrors Device)
  6. Separator
  7. "Quit Dos GatOS" (⌘Q)
- **Main window:** 1040×660 by default, 900×600 minimum, resizable. Closing it hides it (the app keeps running). Single instance: launching again focuses the window.
- **App icon:** 1024 px squircle, `milk` background with the color mark (black + ginger cats), per `Logo.dc.html` → "App icon". Generate every size with `tauri icon`.

---

## 7. Screens

Copy text verbatim from the mockups. Behavior:

### 7.1 Playlist (`AppWindow.dc.html`)
- The header shows "N of M screens on · drag to reorder" and a "+ Add screen" button. That button opens the Extensions tab filtered to "not installed" and also offers the user's Sources.
- **Preview panel:** the `PixelClock` (scale 9, white body) shows the selected row, animated. "On the clock now" shows the name of the screen the device is displaying, when the firmware reports it; otherwise the selected row.
- **Rows:**
  - Drag handle (reorder with keyboard too: ⌥↑/⌥↓), LED color tile, name, duration.
  - **Gear** opens the settings modal.
  - The **switch** enables or disables the screen. Changes go to the clock immediately, with optimistic UI; roll back on error.
- **Settings modal (per screen):**
  - Title "<Name> settings". "Show for N s" first, then the fields from the screen's schema: text, number with unit, or toggle, as shown.
  - Footer: "Remove from playlist" (danger, left), Cancel, Save.
  - Esc and Cancel close it without saving; Enter saves; focus is trapped inside.
  - Extensions declare their own fields in their catalog entry (§7.3). Sources get "Show for" plus a link to edit the source.

### 7.2 Sources (`AppSources.dc.html`)
- **List rows:** color square, name, `host · every <interval>`, the live formatted value on a dark chip in the source's color (from `source://value`), and a gear.
- If the last poll failed, show the chip in a muted style with a tooltip carrying the error.
- "+ Add source" (header and list footer) opens the modal empty, with the title "New source".
- **Modal:**
  - Preview clock at the top, using the `custom` screen with text, color and icon. It updates live as fields change.
  - Fields: Name, Refresh (s), URL, JSON path, Format, Color (7 swatches: `#FFB23F #FF7A3D #FF4B4B #3DDC84 #4DA3FF #B57BFF #FFF1DC`).
  - "Advanced" disclosure: Find, Keep (chars), Regex, Multiply by, Decimals, Icon (LaMetric ID). Open it automatically if any of these fields is set.
  - Footer: Delete (confirm first), **Test** (runs `test_source` and shows the response preview with the matched line highlighted, the step outputs and the result under the form), and **Save & send to clock**.
  - Validation: Name is required and unique; URL must be `http(s)`; Refresh ≥ 5 s; Regex must compile (show the error inline).
- Hint under the list: the mockup copy. The "Show apps.json in Finder" link sits in the list footer or the header menu.

### 7.3 Extensions (`AppExtensions.dc.html`)
- Search input, All / Installed segmented control, and rows with an icon tile, name, Official/Community tag, one-line description, and an Install or Installed button.
- Install adds the extension to the playlist (enabled, default duration) and sends it to the clock. Pressing Installed asks "Remove <name>?" and removes it from the playlist.
- **Catalog:** bundle `catalog.json` (seed from `design/extensions.json`) with `{ id, name, summary, official, screen, defaultDuration, fields: [...] }`. Leave `// TODO(marketplace): fetch from <url>` for a remote catalog later. Extensions that need firmware support that doesn't exist yet show a disabled Install button with a tooltip.

### 7.4 Device (`AppDevice.dc.html`)

Two columns of grouped rows:

| Group | Rows |
|---|---|
| **Display** | Brightness slider (debounce 150 ms), Auto brightness (light sensor), Default font (Device default / Compact / Large) |
| **Time** | Time zone (defaults to the Mac's), 24‑hour clock |
| **Connection** | USB (port + last reply + Reconnect); Wi‑Fi fallback (status text + "Set up…" / "Change…", which opens the modal); Launch at login |
| **Firmware** | Version + "Check for updates"; Restart the clock |

**Wi‑Fi modal:**
- The explainer copy, SSID and Password fields, and the hint about unverified HTTPS.
- Footer: "Turn Wi‑Fi off" (only when on), Cancel, "Save to clock".
- The password field is never pre-filled.

If the device is disconnected, rows that need it are disabled and a banner at the top says "Clock not connected — changes will apply when it reconnects". Queue the changes.

### 7.5 Log (`AppLog.dc.html`)
- A live list, newest first. Each row has the time, a level dot (info `#B7AFA3`, warn `#EDB54A`, error `#E5484D`) and the message. Error rows get a faint red bg.
- Header: "Problems only" switch, Clear, and Export (save dialog, `.log` text).
- Keep 1 000 lines in memory. The file log goes to `~/Library/Logs/DosGatOS/` with a 7-day rotation. Auto-scroll only when the user is at the top.

---

## 8. `PixelClock` preview component

Port `design/Clock.dc.html` exactly. If the website is built in the same repo, share the package (`packages/pixel-clock`).

- Props: `screen`, `scale`, `body` (`white`/`graphite`/`none`), `animate`, `text`, `color`, `icon`, `progress`.
- Geometry formulas from `renderVals()`: `g = max(1, round(s*0.16))`, `hp = round(s)`, `vp = round(1.5s)`, `rim = max(2, round(0.3s))`, the depth offset `dx = round(0.9s)`, and so on. At scale 9 the result must be **351×120**; at scale 7, **279×94**.
- Render the 256 LEDs as one SVG (not 256 divs). Tick every 700 ms from one shared timer, and only while the clock is visible and the window is focused.
- In the app, the preview must match what the firmware actually draws. If the firmware's font or icons differ from the 3×5 font and sprites in the mockup, **use the firmware's bitmaps** (export them from the firmware source into a TS module) and tell the owner.

---

## 9. Build, sign, ship

- `pnpm tauri build --target universal-apple-darwin` produces a `.app` and a `.dmg`.
- Signing: set `bundle.macOS.signingIdentity` to the owner's "Developer ID Application: …" identity (read it from an env var, don't hardcode). Enable the hardened runtime. Add entitlements only as needed (network client; serial access needs no special entitlement outside the sandbox — **don't enable App Sandbox**).
- Notarization: `APPLE_ID`, `APPLE_PASSWORD` (app-specific password) and `APPLE_TEAM_ID` come from env / keychain. Add a `scripts/release.sh` that builds, signs, notarizes, staples, and prints the DMG path.
- Bundle id: `TODO(owner)`, e.g. `dev.tls1.dosgatos`. Version comes from `package.json`.
- Migration: on first launch, if the old LaunchAgent plist exists, offer to unload and remove it, and import its `apps.json`.

---

## 10. Tests and acceptance

- **Rust unit tests:** the pipeline against the four sample sources in `design/AppSources.dc.html` (their `resp` arrays are realistic response bodies), plus edge cases (missing path, regex without a group, non-numeric).
- **Rust integration:** a fake serial device (a pty pair) replaying frames recorded from the Python agent; assert that connect → time sync → push value → brightness all round-trip.
- **Frontend:** Vitest for `compose()` geometry and screen snapshots; Testing Library for the modals (focus trap, Esc, validation).
- **Acceptance checklist:**
  - [ ] Every screen matches its mockup at 1040×660 (±2 px on key blocks).
  - [ ] The playlist switch, the gear modal, drag reorder and Remove all work and reach the clock.
  - [ ] A new source can be created, tested, saved, shows its live value in the list and appears on the clock.
  - [ ] Pulling the USB cable updates the status within 3 s; replugging reconnects automatically.
  - [ ] Wi‑Fi fallback can be set up and turned off; the password is never written to disk on the Mac.
  - [ ] Launch at login works after a reboot; closing the window keeps the app in the menu bar; a second launch focuses the window.
  - [ ] `scripts/release.sh` produces a signed, notarized, stapled universal DMG.
  - [ ] Every protocol gap is listed as `TODO(firmware)` and summarized in `app/FIRMWARE_GAPS.md` for the owner.

## 11. Order of work
1. Read the Python agent, then write `app/PROTOCOL.md` summarizing framing, commands and apps.json. Stop and show it to the owner if anything is ambiguous.
2. Scaffold Tauri + React + Tailwind + tokens. Build the tray, window lifecycle and single instance.
3. Rust device manager + status events → sidebar device card.
4. Sources pipeline + poller + persistence → Sources tab + modal + Test.
5. Playlist + gear modal → Extensions (bundled catalog).
6. Device tab + Wi‑Fi modal → Log tab.
7. Signing/notarization script, LaunchAgent migration, acceptance pass.
