// dos-gatos — прошивка для Ulanzi TC001
// Часы сами решают, что и когда запрашивать (apps в NVS).
//   USB (приоритет): HTTP выполняет агент на Mac, протокол NDJSON 460800 бод.
//   Wi-Fi (fallback): если агента нет, часы ходят в интернет сами, время — по NTP.
//
// device -> host: hello, req, pong, btn, log, wifi, cfg
// host -> device: hello?, ping, time, resp, notify, apps, bright, icon, wifi, settings, cfg, cfg?

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <FastLED_NeoMatrix.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_partition.h>
#include <sys/time.h>

#include "Font4x6.h"
#include "FontBlocky3x5.h"
#include "net.h"
#include "version.h"

#define PIN_MATRIX   32
#define PIN_BTN_L    26
#define PIN_BTN_M    27
#define PIN_BTN_R    14
#define PIN_BUZZER   15
#define PIN_SDA      21
#define PIN_SCL      22
#define RTC_ADDR     0x68   // DS1307, хранит UTC
#define RTC_MAX_DRIFT_S 60  // RTC считается рабочим, если на последней сверке ушёл меньше
#define MW 32
#define MH 8
#define NUM_LEDS (MW * MH)

#define SERIAL_BAUD      460800  // CH340 на macOS не держит 921600
#define RX_LINE_MAX      12288   // a whole config (apps + settings) must fit in one line
#define LINK_TIMEOUT_MS  10000
#define REQ_TIMEOUT_MS   30000
#define APP_DWELL_MS     8000
#define BOOT_SCREEN_MS   5000    // «HOLA» с котами после включения
#define MAX_APPS         16      // switched-on sources shown; each slot holds a 2 KB icon
#define PERSIST_EVERY_MS 900000   // пишем значение в NVS не чаще раза в 15 мин на app
#define ICON_W           8
#define ICON_PX          (ICON_W * ICON_W)
// Полный конфиг приложения (источники, экран часов, яркость, пояс) — как его прислало
// приложение, с номером ревизии. Часы его не разбирают, только хранят и отдают, чтобы
// любой компьютер, к которому их подключили, показывал и правил тот же набор.
#define CFG_PATH         "/cfg.json"

CRGB leds[NUM_LEDS];
FastLED_NeoMatrix *matrix;
Preferences prefs;

// ---------- fonts ----------
enum FontId : int8_t { FONT_DEFAULT = -1, FONT_5X7 = 0, FONT_4X6 = 1, FONT_3X5 = 2 };
const char *FONT_NAMES[] = {"5x7", "4x6", "3x5"};
int8_t fontDefault = FONT_3X5;

int8_t parseFont(const char *s, int8_t def) {
  if (!s) return def;
  for (int8_t i = 0; i < 3; i++) if (!strcmp(s, FONT_NAMES[i])) return i;
  return def;
}

const GFXfont *gfxFont(int8_t f) {
  return f == FONT_4X6 ? &Font4x6 : f == FONT_3X5 ? &FontBlocky3x5 : nullptr;
}
int8_t baselineY(int8_t f) { return f == FONT_5X7 ? 0 : 6; }   // 5x7: курсор — верх; GFXfont: базовая линия

// «°» (UTF-8 C2 B0) → глиф градуса: 0xF8 в 5x7 (CP437), '`' в наших шрифтах
// ---------- text → ASCII ----------
// The fonts are ASCII only. Values come as UTF-8 (a Russian track name, "España"), so letters are
// transliterated before drawing: Latin with diacritics → base letters, Cyrillic → Latin, typographic
// punctuation → ASCII; anything else becomes '?'. Tables generated from Unicode (NFD) and GOST-like
// transliteration.
const char *const TR_LATIN[] = {"A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I", "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss", "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i", "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y", "A", "a", "A", "a", "A", "a", "C", "c", "C", "c", "C", "c", "C", "c", "D", "d", "D", "d", "E", "e", "E", "e", "E", "e", "E", "e", "E", "e", "G", "g", "G", "g", "G", "g", "G", "g", "H", "h", "H", "h", "I", "i", "I", "i", "I", "i", "I", "i", "I", "i", "IJ", "ij", "J", "j", "K", "k", "k", "L", "l", "L", "l", "L", "l", "L", "l", "L", "l", "N", "n", "N", "n", "N", "n", "'n", "N", "n", "O", "o", "O", "o", "O", "o", "OE", "oe", "R", "r", "R", "r", "R", "r", "S", "s", "S", "s", "S", "s", "S", "s", "T", "t", "T", "t", "T", "t", "U", "u", "U", "u", "U", "u", "U", "u", "U", "u", "U", "u", "W", "w", "Y", "y", "Y", "Z", "z", "Z", "z", "Z", "z", "s"};   // U+00C0..U+017F
const char *const TR_CYRILLIC[] = {"?", "E", "?", "?", "Ye", "?", "I", "Yi", "J", "?", "?", "?", "?", "?", "U", "?", "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P", "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "", "Y", "", "E", "Yu", "Ya", "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p", "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya", "?", "e", "?", "?", "ye", "?", "i", "yi", "j", "?", "?", "?", "?", "?", "u", "?"};  // U+0400..U+045F

String toAscii(const String &s) {
  String out;
  out.reserve(s.length());
  const uint8_t *p = (const uint8_t *)s.c_str();
  while (*p) {
    uint32_t cp;
    int len;
    if (*p < 0x80) { cp = *p; len = 1; }
    else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
    else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
    else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = 0x1F000; len = 4; }   // emoji etc.
    else { cp = '?'; len = 1; }
    p += len;
    if (cp < 0x80) out += (char)cp;
    else if (cp == 0xB0) out += "\xC2\xB0";                 // degree: glyphText maps it per font
    else if (cp >= 0xC0 && cp < 0x180) out += TR_LATIN[cp - 0xC0];
    else if (cp >= 0x400 && cp < 0x460) out += TR_CYRILLIC[cp - 0x400];
    else if (cp == 0x490 || cp == 0x491) out += cp == 0x490 ? 'G' : 'g';   // Ukrainian Ґ ґ
    else if (cp == 0x2018 || cp == 0x2019) out += '\'';
    else if (cp == 0x201C || cp == 0x201D || cp == 0xAB || cp == 0xBB) out += '"';
    else if (cp == 0x2013 || cp == 0x2014) out += '-';
    else if (cp == 0x2026) out += "...";
    else if (cp == 0xA0) out += ' ';
    else out += '?';
  }
  return out;
}

String glyphText(int8_t f, const String &s) {
  String t = toAscii(s);
  t.replace("\xC2\xB0", f == FONT_5X7 ? "\xF8" : "`");
  return t;
}

int advance(int8_t f, uint8_t c) {
  const GFXfont *g = gfxFont(f);
  if (!g) return 6;
  if (c < pgm_read_word(&g->first) || c > pgm_read_word(&g->last)) c = '?';
  GFXglyph *gl = ((GFXglyph *)pgm_read_ptr(&g->glyph)) + (c - pgm_read_word(&g->first));
  return pgm_read_byte(&gl->xAdvance);
}

int textWidth(int8_t f, const String &s) {
  if (!s.length()) return 0;
  int w = 0;
  for (size_t i = 0; i < s.length(); i++) w += advance(f, s[i]);
  return w - 1;
}

// ---------- icons (8×8 RGB565, до 16 кадров, LittleFS /i/<id>.bin) ----------
// Файл: v1 — 128 байт (один кадр, 0.4.0); v2 — 'I','2',n,0, задержки n×uint16 (мс), кадры n×128 байт.
#define ICON_MAX_FRAMES 16
struct Icon {
  bool ok = false;
  uint8_t n = 0;
  uint32_t total = 0;                        // сумма задержек, мс
  uint16_t delay[ICON_MAX_FRAMES];
  uint16_t px[ICON_MAX_FRAMES][ICON_PX];
  const uint16_t *frame() const {            // текущий кадр анимации
    if (n <= 1 || !total) return px[0];
    uint32_t t = millis() % total;
    for (uint8_t i = 0; i < n; i++) {
      if (t < delay[i]) return px[i];
      t -= delay[i];
    }
    return px[n - 1];
  }
};

bool validIconId(const String &id) {
  if (!id.length() || id.length() > 16) return false;
  for (char c : id) if (!isalnum((unsigned char)c) && c != '_' && c != '-') return false;
  return true;
}

bool loadIcon(const String &id, Icon &ic) {
  ic.ok = false;
  if (!validIconId(id)) return false;
  File f = LittleFS.open("/i/" + id + ".bin", "r");
  if (!f) return false;
  size_t size = f.size();
  if (size == ICON_PX * 2) {                  // v1
    ic.n = 1;
    ic.delay[0] = 1000;
    ic.ok = f.read((uint8_t *)ic.px[0], ICON_PX * 2) == ICON_PX * 2;
  } else {
    uint8_t h[4];
    if (f.read(h, 4) == 4 && h[0] == 'I' && h[1] == '2' && h[2] >= 1 && h[2] <= ICON_MAX_FRAMES) {
      ic.n = h[2];
      size_t frames = (size_t)ic.n * ICON_PX * 2;
      ic.ok = size == 4 + ic.n * 2 + frames &&
              f.read((uint8_t *)ic.delay, ic.n * 2) == ic.n * 2u &&
              f.read((uint8_t *)ic.px, frames) == frames;
    }
  }
  f.close();
  ic.total = 0;
  if (ic.ok) for (uint8_t i = 0; i < ic.n; i++) ic.total += ic.delay[i];
  return ic.ok;
}

// ---------- LittleFS: guarded writes ----------
// LittleFS in the Arduino core panics (IntegerDivideByZero in lfs_alloc) when a write fails
// on a full or damaged filesystem. Hosts re-send icons after every hello, so one bad write
// became a reboot loop. Now:
// - nothing is written without room to spare, and after a failed write nothing more is
//   written until the next restart;
// - a marker in RTC memory (kept across a panic reboot, not across power-off) tells the next
//   boot that the last reboot happened mid-write; the filesystem is then reformatted — icons
//   and the stored config are lost and come back from the host, the clock stays usable.
#define FS_GUARD_MAGIC 0xF5A11ED0
void sendLog(const String &m);
#define FS_RESERVE     (16 * 1024)   // keep a few blocks free: LittleFS needs them to rewrite files
RTC_NOINIT_ATTR uint32_t fsGuard;
bool fsOk = false;
String fsNote;                        // said once the host is listening (sendLog at boot goes nowhere)

void fsBegin() {
  const bool crashedInWrite = esp_reset_reason() == ESP_RST_PANIC && fsGuard == FS_GUARD_MAGIC;
  fsGuard = 0;
  fsOk = LittleFS.begin(true);
  if (fsOk && crashedInWrite) {
    // LittleFS.format() fails on the damaged filesystem this recovers from (and the next mount
    // then finds the same damage), so wipe the whole partition and let begin() make a new one
    LittleFS.end();
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    const bool wiped = p && esp_partition_erase_range(p, 0, p->size) == ESP_OK;
    fsOk = wiped && LittleFS.begin(true);
    fsNote = fsOk ? "littlefs: the last restart happened while writing — erased and made anew, icons and config will be re-sent"
                  : "littlefs: the last restart happened while writing and the filesystem couldn't be made anew — icons and config can't be stored";
  } else if (!fsOk) {
    fsNote = "littlefs mount failed — icons and config can't be stored";
  }
  if (fsOk) LittleFS.mkdir("/i");
}

size_t fsFree() { return fsOk ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0; }

/** false = not written (no room, failed, or writes stopped); the file then keeps its old content. */
bool fsWrite(const String &path, const uint8_t *data, size_t len) {
  if (!fsOk) return false;
  if (fsFree() < len + FS_RESERVE) {
    sendLog("littlefs full: " + path + " not written (" + String(fsFree() / 1024) + " KB free)");
    return false;
  }
  fsGuard = FS_GUARD_MAGIC;
  File f = LittleFS.open(path, "w", true);
  bool ok = f && f.write(data, len) == len;
  if (f) f.close();
  fsGuard = 0;
  if (!ok) {
    fsOk = false;
    sendLog("littlefs write failed: " + path + " — no more writes until restart");
  }
  return ok;
}

bool fsRemove(const String &path) {
  if (!fsOk) return false;
  fsGuard = FS_GUARD_MAGIC;
  bool ok = LittleFS.remove(path);
  fsGuard = 0;
  return ok;
}

bool saveIcon(const String &id, const char *hex, int n, JsonArrayConst delays) {
  if (!validIconId(id) || !hex || n < 1 || n > ICON_MAX_FRAMES || strlen(hex) != (size_t)n * ICON_PX * 4) return false;
  const size_t size = 4 + n * 2 + (size_t)n * ICON_PX * 2;
  uint8_t *buf = (uint8_t *)malloc(size);
  if (!buf) return false;
  buf[0] = 'I'; buf[1] = '2'; buf[2] = n; buf[3] = 0;
  uint16_t *dl = (uint16_t *)(buf + 4);
  for (int i = 0; i < n; i++) dl[i] = constrain((int)(delays[i] | 100), 20, 60000);
  uint16_t *px = (uint16_t *)(buf + 4 + n * 2);
  for (int i = 0; i < n * ICON_PX; i++) {
    char b[5] = {hex[i * 4], hex[i * 4 + 1], hex[i * 4 + 2], hex[i * 4 + 3], 0};
    px[i] = (uint16_t)strtoul(b, nullptr, 16);
  }
  const String path = "/i/" + id + ".bin";
  bool ok = false, same = false;
  File r = LittleFS.open(path, "r");                  // та же иконка уже лежит — флеш не трогаем
  if (r && r.size() == size) {
    uint8_t *old = (uint8_t *)malloc(size);
    same = old && r.read(old, size) == size && !memcmp(old, buf, size);
    free(old);
  }
  if (r) r.close();
  ok = same || fsWrite(path, buf, size);
  free(buf);
  return ok;
}

// ---------- data apps (proactive sources) ----------
#define MAX_ICON_RULES 12

struct DataApp {
  String name, url, path, find, re, fmt, iconId;
  // icon by a second value of the same response (e.g. the weather code): first rule with
  // key <= max wins; curIcon is the icon on screen now (starts as iconId)
  String ipath, curIcon;
  uint8_t nrules = 0;
  float ruleMax[MAX_ICON_RULES];
  String ruleIcon[MAX_ICON_RULES];
  String ruleStr[MAX_ICON_RULES];   // non-empty: the rule matches this text (e.g. "failed") instead of <= max
  // request headers (Authorization, PRIVATE-TOKEN, X-Api-Key…)
  uint8_t nh = 0;
  String hk[4], hv[4];
  // local screens, computed on the clock (no URL): 1 = date, 2 = countdown to a day
  uint8_t kind = 0;
  String dateFmt;                   // date: strftime pattern, e.g. "%a %d"
  int16_t cdYear = 0; int8_t cdMonth = 0, cdDay = 0;
  bool cdYearly = false;            // countdown: every year (birthdays)
  uint16_t keep = 128;
  uint32_t everyMs = 300000;
  uint32_t durMs = APP_DWELL_MS;   // сколько показывать экран в плейлисте
  float scale = 1.0f;
  int8_t dec = -1;          // -1: показывать значение как есть
  int8_t font = FONT_DEFAULT;
  uint16_t color = 0xFFFF;  // RGB565
  // manifest v2 frames (fw 0.9.2): several finished screens shown in turn, fdurMs each
  struct Frame { String text; uint16_t color; String icon; };
  std::vector<Frame> frames;            // on the heap: static DRAM is nearly full
  uint16_t fdurMs = 3000;
  int8_t curFrame = -1;
  // manifest v2 (rules worked out by the host): the value is the finished text, in its own color
  bool finalText = false;
  bool hasColorOv = false;
  uint16_t colorOv = 0;
  // runtime
  String value;
  uint32_t updatedAt = 0;
  uint32_t nextDue = 0;
  int32_t inFlightId = -1;
  bool inFlightLocal = false;
  uint32_t sentAt = 0;
  uint32_t persistedAt = 0;
  bool persisted = false;
};

DataApp apps[MAX_APPS];
Icon appIcons[MAX_APPS];      // отдельно от DataApp: 2 КБ на иконку, не копируем через DataApp()
uint8_t appCount = 0;

// ---------- state ----------
uint32_t lastRx = 0;
// Where the host talks from: the USB cable or the local network (the Mac app over Wi-Fi).
// Replies go back the way the last line came; USB wins — a cable host closes a network one.
enum Chan : uint8_t { CH_NONE, CH_USB, CH_NET };
Chan linkChan = CH_NONE;
WiFiClient netClient;
bool linkUp = false;          // агент на USB отвечает
bool timeSynced = false;      // время есть (из RTC, от агента или NTP)
bool trustedTime = false;     // время подтверждено агентом или NTP в этой загрузке
bool agentSynced = false;     // агент прислал время в этой загрузке (hello дошёл)
volatile bool ntpEvent = false;
uint32_t lastHello = 0;
String tzPosix = "UTC0";
int32_t nextReqId = 1;
uint8_t brightness = 30;

String wifiSsid, wifiPass;
bool wifiWasConnected = false;
uint32_t wifiReportAt = 0;

int8_t current = -1;          // -1 = clock, 0..appCount-1 = data app

// Экран часов в плейлисте (settings{clock:{on,dur,pos,h24,wday}} от агента, хранится в NVS "clk")
struct ClockCfg { bool on = true; uint32_t durMs = 10000; uint8_t pos = 0; bool h24 = true; bool wday = true; };
ClockCfg clk;
uint32_t shownAt = 0;
int16_t scrollX = MW;
uint32_t lastScroll = 0;

String notifyText;
uint16_t notifyColor = 0xFFFF;
uint32_t notifyUntil = 0;
int8_t notifyFont = FONT_DEFAULT;
Icon notifyIcon;
String notifyIconId;
// "On a call" from the host: its own screen, like the Pomodoro timer, while the host keeps saying so
Icon callIcon;
String callIconId;
uint32_t busyUntil = 0;
bool onCall() { return busyUntil && (int32_t)(busyUntil - millis()) > 0; }

char lineBuf[RX_LINE_MAX];
size_t lineLen = 0;
bool lineOverflow = false;

// ---------- helpers ----------
uint16_t parseColor(const char *s, uint16_t def) {
  if (!s || s[0] != '#' || strlen(s) != 7) return def;
  uint32_t v = strtoul(s + 1, nullptr, 16);
  return matrix->Color((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

uint16_t dim565(uint16_t c) { return (c >> 1) & 0x7BEF; }   // половинная яркость

void send(JsonDocument &doc) {
  if (linkChan == CH_NET && netClient.connected()) {
    String s;
    serializeJson(doc, s);
    s += '\n';
    netClient.write((const uint8_t *)s.c_str(), s.length());
    return;
  }
  serializeJson(doc, Serial);
  Serial.write('\n');
}

void sendLog(const String &m) {
  JsonDocument d;
  d["t"] = "log";
  d["m"] = m;
  send(d);
}

// why the last attempt to join failed (from the Wi-Fi driver; 0 = none yet)
volatile uint8_t wifiReason = 0;

const char *wifiReasonText(uint8_t r) {
  switch (r) {
    case WIFI_REASON_NO_AP_FOUND: return "network not found — the clock only sees 2.4 GHz networks";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED: return "wrong password";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_EXPIRE: return "the router didn't let the clock in";
    case WIFI_REASON_BEACON_TIMEOUT: return "signal lost";
    default: return nullptr;
  }
}

void fillWifiStatus(JsonObject w) {
  w["ssid"] = wifiSsid;
  const char *state = !wifiSsid.length() ? "off" : WiFi.isConnected() ? "connected" : "connecting";
  w["state"] = state;
  if (wifiSsid.length() && !WiFi.isConnected() && wifiReason) {
    const char *why = wifiReasonText(wifiReason);
    w["why"] = why ? String(why) : "error " + String(wifiReason);
  }
  if (WiFi.isConnected()) {
    w["ip"] = WiFi.localIP().toString();
    w["rssi"] = WiFi.RSSI();
  }
}

const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}

uint32_t cfgRev = 0;          // ревизия сохранённого конфига приложения, 0 — нет
uint16_t scrollMs = 75;       // бегущая строка: мс на пиксель (с 0.7.3 настраивается, было 45)

void loadCfgRev() {
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f) return;
  JsonDocument filter;
  filter["rev"] = true;
  JsonDocument d;
  if (deserializeJson(d, f, DeserializationOption::Filter(filter)) == DeserializationError::Ok) cfgRev = d["rev"] | 0;
  f.close();
}

// Ответ на cfg?: {"t":"cfg","rev":N,"cfg":{...}} или {"t":"cfg","rev":0}, если конфига нет.
void sendCfg() {
  JsonDocument d;
  File f = LittleFS.open(CFG_PATH, "r");
  if (!f || deserializeJson(d, f) != DeserializationError::Ok) {
    d.clear();
    d["rev"] = 0;
  }
  if (f) f.close();
  d["t"] = "cfg";
  send(d);
}

void saveCfg(JsonDocument &msg) {
  uint32_t rev = msg["rev"] | 0;
  if (!rev || !msg["cfg"].is<JsonObjectConst>()) {
    sendLog("cfg rejected");
    return;
  }
  JsonDocument d;
  d["rev"] = rev;
  d["cfg"] = msg["cfg"];
  String out;
  serializeJson(d, out);
  // the same config again (hosts re-send on reconnect) — confirm without touching the flash
  bool same = false;
  if (File r = LittleFS.open(CFG_PATH, "r")) {
    same = r.size() == out.length() && r.readString() == out;
    r.close();
  }
  bool ok = same || fsWrite(CFG_PATH, (const uint8_t *)out.c_str(), out.length());
  if (ok) cfgRev = rev;
  sendLog(ok ? "cfg saved: rev " + String(rev) : String("cfg write failed"));
}

// pomodoro state (the timer itself is further down, next to drawing)
struct PomoCfg { bool on = true; uint16_t work = 25, brk = 5, lng = 15; uint8_t every = 4; bool sound = true; };
PomoCfg pomo;
enum PomoPhase : uint8_t { POMO_IDLE, POMO_WORK, POMO_BREAK, POMO_LONG };
const char *const POMO_NAMES[] = {"idle", "work", "break", "long"};
PomoPhase pomoPhase = POMO_IDLE;
uint32_t pomoEndsAt = 0, pomoLeftMs = 0, pomoPhaseMs = 1;
bool pomoPaused = false;
uint8_t pomoRounds = 0;
// looking at other screens while the timer runs: since when (0 = the timer is on screen)
uint32_t pomoAwayAt = 0;
#define POMO_AWAY_MS 15000   // back to the timer after this long without a button
Icon pomoTomato, pomoCup;   // pomoCup also holds the hourglass while a plain timer runs (static DRAM is tight)
bool pomoOnce = false;      // a plain timer (from the app's menu): one phase, a beep, done
// do not disturb (from the app's menu): screen dark and buzzer quiet until this time (epoch, 0 = off)
time_t dndUntil = 0;
bool dndActive();
void navigate(int dir);
void plainTimer(int minutes);
uint8_t beepsLeft = 0;
uint32_t beepAt = 0;
bool beepOn = false;
void applyPomoCfg(JsonObjectConst c);
bool pomoActive();
bool stickyActive();
void pomoStartStop();
void pomoStop();
void pomoPauseResume();
void sendPomo();

void sendHello() {
  JsonDocument d;
  d["t"] = "hello";
  d["fw"] = FW_VERSION;
  if (dndActive()) d["dnd"] = (long)dndUntil;
  d["rst"] = resetReason();                // причина последней перезагрузки — для диагностики
  d["heap"] = ESP.getFreeHeap();
  d["apps"] = appCount;
  d["cfg"] = cfgRev;
  d["max"] = MAX_APPS;                     // с 0.6.2: сколько включённых источников показывают часы
  d["line"] = RX_LINE_MAX;                 // и какой длины строку принимают
  d["pomo"] = POMO_NAMES[pomoPhase];       // с 0.7.0: есть помидор (и в какой он фазе)
  d["scroll"] = scrollMs;                  // с 0.7.3: скорость бегущей строки, мс на пиксель                       // есть с 0.6.0: приложение по нему понимает, что конфиг хранится на часах
  if (fsOk) {                              // с 0.6.1: заполненность LittleFS
    JsonObject fs = d["fs"].to<JsonObject>();
    fs["used"] = LittleFS.usedBytes();
    fs["total"] = LittleFS.totalBytes();
  }
  char id[13];
  snprintf(id, sizeof id, "%012llx", ESP.getEfuseMac());
  d["id"] = id;
  d["font"] = FONT_NAMES[fontDefault];
  d["scr"] = current < 0 ? "clock" : apps[current].name.c_str();
  fillWifiStatus(d["wifi"].to<JsonObject>());
  send(d);
}

void sendWifiStatus() {
  JsonDocument d;
  fillWifiStatus(d.to<JsonObject>());
  d["t"] = "wifi";
  send(d);
}

// ---------- time ----------
// UTC из struct tm без зависимости от TZ (mktime учитывает TZ, а нам нужен UTC)
time_t utcFromTm(const struct tm &t) {
  int y = t.tm_year + 1900, m = t.tm_mon + 1;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.tm_mday - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = era * 146097L + (long)doe - 719468L;
  return (time_t)days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

void applyTz(const String &posix) {
  tzPosix = posix.length() ? posix : "UTC0";
  setenv("TZ", tzPosix.c_str(), 1);
  tzset();
}

// POSIX-строка из смещения в секундах (знак в POSIX обратный): +7200 → "UTC-2"
String posixFromOffset(int32_t off) {
  int32_t a = abs(off);
  String s = String("UTC") + (off > 0 ? "-" : "+") + String(a / 3600);
  if (a % 3600) s += ":" + String((a % 3600) / 60);
  return s;
}

// ---------- RTC DS1307 ----------
static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool rtcRead(time_t &out) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(RTC_ADDR, 7) != 7) return false;
  uint8_t r[7];
  for (int i = 0; i < 7; i++) r[i] = Wire.read();
  if (r[0] & 0x80) return false;               // CH: генератор остановлен — время не задано
  if (r[2] & 0x40) return false;               // 12-часовой режим мы не пишем
  struct tm t = {};
  t.tm_sec = bcd2bin(r[0] & 0x7F);
  t.tm_min = bcd2bin(r[1]);
  t.tm_hour = bcd2bin(r[2] & 0x3F);
  t.tm_mday = bcd2bin(r[4]);
  t.tm_mon = bcd2bin(r[5]) - 1;
  t.tm_year = bcd2bin(r[6]) + 100;             // 20xx
  if (t.tm_year < 124 || t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1) return false;
  out = utcFromTm(t);
  return out > 0;
}

bool rtcWrite(time_t utc) {
  struct tm t;
  gmtime_r(&utc, &t);
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0);
  Wire.write(bin2bcd(t.tm_sec));               // CH=0: запустить генератор
  Wire.write(bin2bcd(t.tm_min));
  Wire.write(bin2bcd(t.tm_hour));              // 24-часовой режим
  Wire.write(bin2bcd(t.tm_wday + 1));
  Wire.write(bin2bcd(t.tm_mday));
  Wire.write(bin2bcd(t.tm_mon + 1));
  Wire.write(bin2bcd(t.tm_year % 100));
  return Wire.endTransmission() == 0;
}

// Доверяем RTC, только если на последней сверке он был точен (флаг в NVS).
void restoreTimeFromRtc() {
  if (!prefs.getBool("rtc_ok", false)) return;
  time_t t;
  if (!rtcRead(t)) return;
  struct timeval tv = { t, 0 };
  settimeofday(&tv, nullptr);
  timeSynced = true;
}

// Точное время пришло (агент или NTP): сверить RTC, запомнить результат, записать RTC.
void onTrustedTime(time_t epoch, const char *source) {
  time_t rtcNow;
  bool rtcOk = false;
  if (rtcRead(rtcNow)) {
    long drift = (long)(rtcNow - epoch);
    rtcOk = labs(drift) < RTC_MAX_DRIFT_S;
    sendLog(String("rtc drift ") + drift + " s, " + (rtcOk ? "ok" : "untrusted") + " (" + source + ")");
  } else {
    sendLog(String("rtc invalid, untrusted (") + source + ")");
  }
  if (rtcOk != prefs.getBool("rtc_ok", false)) prefs.putBool("rtc_ok", rtcOk);
  if (!rtcWrite(epoch)) sendLog("rtc write failed");
  timeSynced = true;
  trustedTime = true;
}

void onNtpSync(struct timeval *) { ntpEvent = true; }   // вызывается из задачи lwIP — только флаг

// ---------- Wi-Fi ----------
void wifiApply() {
  if (!wifiSsid.length()) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }
  static bool hooked = false;
  if (!hooked) {
    hooked = true;
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) { wifiReason = info.wifi_sta_disconnected.reason; },
                 ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t) { wifiReason = 0; }, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  }
  wifiReason = 0;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("dos-gatos");
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
}

void wifiLoop() {
  bool up = WiFi.isConnected();
  if (up != wifiWasConnected) {
    wifiWasConnected = up;
    if (up) {
      // NTP держим включённым всегда: даёт точное время и без агента
      sntp_set_time_sync_notification_cb(onNtpSync);
      configTzTime(tzPosix.c_str(), "pool.ntp.org", "time.google.com");
    }
    if (linkUp) sendWifiStatus();
  }
  static uint8_t lastReason = 0;
  if (wifiReason != lastReason) {
    lastReason = wifiReason;
    if (lastReason && linkUp) {
      const char *why = wifiReasonText(lastReason);
      sendLog("wifi: can't join " + wifiSsid + " — " + (why ? String(why) : "error " + String(lastReason)));
      sendWifiStatus();
    }
  }
  if (ntpEvent) {
    ntpEvent = false;
    if (!linkUp || !trustedTime) onTrustedTime(time(nullptr), "ntp");
  }
}

// ---------- apps config (NVS) ----------
// Последнее значение каждого app: ключ "v<i>" = "<name>\t<value>". Имя сверяем, чтобы
// после смены apps.json не показать чужое значение.
void persistValue(uint8_t i, bool force) {
  DataApp &a = apps[i];
  if (!force && a.persisted && millis() - a.persistedAt < PERSIST_EVERY_MS) return;
  // "\v" instead of "\t": the value is a finished text (manifest v2), not formatted again
  prefs.putString(("v" + String(i)).c_str(), a.name + (a.finalText ? "\v" : "\t") + a.value);
  a.persistedAt = millis();
  a.persisted = true;
}

void restoreValue(uint8_t i) {
  DataApp &a = apps[i];
  String s = prefs.getString(("v" + String(i)).c_str(), "");
  int tab = s.indexOf('\t');
  const int vt = s.indexOf('\v');
  const bool fin = vt > 0 && (tab < 0 || vt < tab);
  if (fin) tab = vt;
  if (tab > 0 && s.substring(0, tab) == a.name) {
    a.value = s.substring(tab + 1);
    a.finalText = fin;
  }
}

/** Returns how many switched-on sources didn't fit into MAX_APPS (0 = all shown). */
int loadAppsFromJson(JsonArrayConst arr) {
  appCount = 0;
  int skipped = 0;
  for (JsonObjectConst a : arr) {
    if (appCount >= MAX_APPS) {
      if (!(a["off"] | false) && (strlen(a["url"] | "") || strlen(a["kind"] | ""))) skipped++;
      continue;
    }
    DataApp &x = apps[appCount];
    x = DataApp();
    x.name = a["name"] | "app";
    x.url = a["url"] | "";
    x.path = a["path"] | "";
    x.find = a["find"] | "";
    x.re = a["re"] | "";
    x.keep = a["keep"] | 128;
    x.fmt = a["fmt"] | "{v}";
    if (a["off"] | false) continue;       // выключен в плейлисте: не опрашиваем и не показываем
    x.everyMs = (uint32_t)(a["every"] | 300) * 1000UL;
    x.durMs = (uint32_t)constrain((int)(a["dur"] | (int)(APP_DWELL_MS / 1000)), 2, 600) * 1000UL;
    x.scale = a["scale"] | 1.0f;
    x.dec = a["dec"] | -1;
    x.font = parseFont(a["font"].as<const char *>(), FONT_DEFAULT);
    x.color = parseColor(a["color"] | "#FFFFFF", 0xFFFF);
    x.iconId = String(a["icon"] | "");
    x.curIcon = x.iconId;
    x.ipath = a["icons"]["path"] | "";
    for (JsonArrayConst r : a["icons"]["rules"].as<JsonArrayConst>()) {
      if (x.nrules >= MAX_ICON_RULES) break;
      x.ruleStr[x.nrules] = r[0].is<const char *>() ? r[0].as<const char *>() : "";
      x.ruleMax[x.nrules] = r[0] | 0.0f;
      x.ruleIcon[x.nrules++] = r[1] | "";
    }
    for (JsonPairConst h : a["headers"].as<JsonObjectConst>()) {
      if (x.nh >= 4) break;
      x.hk[x.nh] = h.key().c_str();
      x.hv[x.nh++] = h.value().as<const char *>();
    }
    const char *kind = a["kind"] | "";
    if (!strcmp(kind, "date")) {
      x.kind = 1;
      x.dateFmt = a["format"] | "%a %d";
    } else if (!strcmp(kind, "countdown")) {
      x.kind = 2;
      sscanf(a["date"] | "", "%hd-%hhd-%hhd", &x.cdYear, &x.cdMonth, &x.cdDay);
      x.cdYearly = a["yearly"] | false;
    }
    appIcons[appCount].ok = false;
    if (x.iconId.length()) loadIcon(x.iconId, appIcons[appCount]);
    if (x.url.length() || x.kind) {
      if (!x.kind) restoreValue(appCount);             // updatedAt = 0 → покажется серым, пока не придёт свежее
      appCount++;
    }
  }
  if (current >= appCount) current = -1;
  return skipped;
}

// Icons no source uses any more (off sources count as used) are deleted: hosts upload icons
// but never remove them, and each one takes a whole flash block.
void gcIcons(JsonArrayConst arr) {
  if (!fsOk) return;
  File dir = LittleFS.open("/i");
  if (!dir) return;
  String stale[16];
  uint8_t n = 0;
  for (File f = dir.openNextFile(); f && n < 16; f = dir.openNextFile()) {
    String name = f.name();                 // "2422.bin"
    f.close();
    String id = name.substring(0, name.lastIndexOf('.'));
    bool used = (notifyIcon.ok && id == notifyIconId) || id == callIconId;
    for (JsonObjectConst a : arr) {
      if (id == (a["icon"] | "")) used = true;
      for (JsonArrayConst r : a["icons"]["rules"].as<JsonArrayConst>())
        if (id == (r[1] | "")) used = true;
    }
    if (!used) stale[n++] = name;
  }
  dir.close();
  for (uint8_t i = 0; i < n; i++)
    if (fsRemove("/i/" + stale[i])) sendLog("icon removed: " + stale[i]);
}

// The list of sources lives in LittleFS (/apps.json). Up to 0.6.1 it was an NVS string, which
// caps at 4000 bytes: a longer list failed to save without a word and was gone after a restart.
#define APPS_PATH "/apps.json"

void saveApps(const String &s) {
  bool same = false;
  if (File r = LittleFS.open(APPS_PATH, "r")) {
    same = r.size() == s.length() && r.readString() == s;
    r.close();
  }
  if (!same && !fsWrite(APPS_PATH, (const uint8_t *)s.c_str(), s.length())) {
    sendLog("apps not stored — they'll be lost on restart");
    return;
  }
  if (prefs.isKey("apps")) prefs.remove("apps");   // moved over from NVS
}

void loadApps() {
  String s;
  if (File f = LittleFS.open(APPS_PATH, "r")) {
    s = f.readString();
    f.close();
  } else {
    s = prefs.getString("apps", "[]");   // firmware ≤ 0.6.1
  }
  JsonDocument d;
  if (deserializeJson(d, s) == DeserializationError::Ok) {
    loadAppsFromJson(d.as<JsonArrayConst>());
    gcIcons(d.as<JsonArrayConst>());
  }
}

// ---------- value formatting ----------
String formatValue(const DataApp &a) {
  if (a.finalText) return a.value;
  String v = a.value;
  if (a.dec >= 0 || a.scale != 1.0f) {
    char *end;
    double num = strtod(v.c_str(), &end);
    if (end != v.c_str()) v = String(num * a.scale, a.dec >= 0 ? a.dec : 2);
  }
  String out = a.fmt;
  out.replace("{v}", v);
  return out;
}

// ---------- requests: USB (агент) или Wi-Fi (сами) ----------
bool requestApp(uint8_t i) {
  DataApp &a = apps[i];
  if (linkUp) {
    a.inFlightId = nextReqId++;
    a.inFlightLocal = false;
    a.sentAt = millis();
    JsonDocument d;
    d["t"] = "req";
    d["id"] = a.inFlightId;
    d["url"] = a.url;
    if (a.path.length()) d["path"] = a.path;
    if (a.find.length()) { d["find"] = a.find; d["keep"] = a.keep; }
    if (a.re.length()) d["re"] = a.re;
    if (a.ipath.length()) d["ipath"] = a.ipath;
    if (a.nh) {
      JsonObject h = d["headers"].to<JsonObject>();
      for (uint8_t k = 0; k < a.nh; k++) h[a.hk[k]] = a.hv[k];
    }
    send(d);
    return true;
  }
  if (WiFi.isConnected() && !netBusy()) {
    int32_t id = nextReqId++;
    NetRequest nr;
    nr.id = id; nr.url = a.url; nr.path = a.path; nr.find = a.find; nr.re = a.re; nr.keep = a.keep; nr.ipath = a.ipath;
    nr.nh = a.nh;
    for (uint8_t k = 0; k < a.nh; k++) { nr.hk[k] = a.hk[k]; nr.hv[k] = a.hv[k]; }
    if (!netSubmit(nr)) return false;
    a.inFlightId = id;
    a.inFlightLocal = true;
    a.sentAt = millis();
    return true;
  }
  return false;
}

// Local screens: refresh their value from the clock's own time (once a second is plenty).
void updateLocalApps() {
  static uint32_t last = 0;
  if (millis() - last < 1000 || !timeSynced) return;
  last = millis();
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  for (uint8_t i = 0; i < appCount; i++) {
    DataApp &a = apps[i];
    if (a.kind == 1) {
      char buf[48];
      strftime(buf, sizeof buf, a.dateFmt.c_str(), &t);
      a.value = buf;
    } else if (a.kind == 2 && a.cdMonth) {
      // whole days between today and the day, both at noon so DST shifts don't matter
      struct tm today = t, target = t;
      today.tm_hour = target.tm_hour = 12;
      today.tm_min = target.tm_min = today.tm_sec = target.tm_sec = 0;
      target.tm_mon = a.cdMonth - 1;
      target.tm_mday = a.cdDay;
      target.tm_year = (a.cdYearly ? t.tm_year + 1900 : a.cdYear) - 1900;
      time_t d0 = mktime(&today), d1 = mktime(&target);
      if (a.cdYearly && d1 < d0) { target.tm_year++; d1 = mktime(&target); }
      a.value = String((long)((d1 - d0 + 43200) / 86400));
    } else {
      continue;
    }
    a.updatedAt = millis();
  }
}


void scheduleFetches() {
  uint32_t now = millis();
  bool canLocal = !linkUp && WiFi.isConnected();
  for (uint8_t i = 0; i < appCount; i++) {
    DataApp &a = apps[i];
    if (a.inFlightId >= 0 && now - a.sentAt > REQ_TIMEOUT_MS + (a.inFlightLocal ? 15000 : 0)) {
      a.inFlightId = -1;                 // ответ потерян — разрешаем повтор
      a.nextDue = now + 10000;
    }
    if (a.kind || a.inFlightId >= 0 || (!linkUp && !canLocal)) continue;
    if (!linkUp && a.url.startsWith("mac://")) continue;  // only the Mac app can answer these
    if ((int32_t)(now - a.nextDue) >= 0) {
      if (requestApp(i)) a.nextDue = now + a.everyMs;
      else break;                        // Wi-Fi-задача занята — остальные в следующий проход
    }
  }
}

// Pick the icon for an icon key (e.g. weather code 61 → rain) and load it if it changed.
void applyIconKey(uint8_t i, const String &key) {
  DataApp &a = apps[i];
  if (!a.nrules || !key.length()) return;
  const float k = key.toFloat();
  String want = a.iconId;
  for (uint8_t r = 0; r < a.nrules; r++) {
    const bool hit = a.ruleStr[r].length() ? key.equalsIgnoreCase(a.ruleStr[r]) : k <= a.ruleMax[r];
    if (hit) { want = a.ruleIcon[r]; break; }
  }
  if (want == a.curIcon) return;
  a.curIcon = want;
  if (!loadIcon(want, appIcons[i]) && a.iconId.length()) loadIcon(a.iconId, appIcons[i]);   // not uploaded yet
}

void applyIconId(uint8_t i, const String &want) {
  DataApp &a = apps[i];
  if (want == a.curIcon) return;
  a.curIcon = want;
  if (!loadIcon(want, appIcons[i]) && a.iconId.length()) loadIcon(a.iconId, appIcons[i]);
}

// A result the host finished with the manifest's rules (v2): text as shown ("" hides the
// screen), and optionally the color and icon those rules picked.
void applyFinal(int32_t id, JsonDocument &d) {
  for (uint8_t i = 0; i < appCount; i++) {
    DataApp &a = apps[i];
    if (a.inFlightId != id) continue;
    a.inFlightId = -1;
    const int status = d["status"] | 0;
    if (status < 200 || status >= 300) {
      a.nextDue = millis() + 30000;
      return;
    }
    a.frames.clear();
    a.curFrame = -1;
    const String text = d["text"].as<String>();
    const char *color = d["color"] | "";
    a.hasColorOv = *color;
    if (*color) a.colorOv = parseColor(color, a.color);
    const char *icon = d["icon"] | "";
    applyIconId(i, *icon ? String(icon) : a.iconId);
    const bool changed = !a.finalText || text != a.value;
    a.finalText = true;
    a.value = text;
    a.updatedAt = millis();
    if (changed) persistValue(i, false);
    return;
  }
}

// Frames the host worked out (manifest v2, fw 0.9.2): {"frames":[{"text","color","icon"}…],"fdur"}.
// No frames left means the app is skipped, like an empty text.
#define MAX_FRAMES 16
void applyFrames(int32_t id, JsonDocument &d) {
  for (uint8_t i = 0; i < appCount; i++) {
    DataApp &a = apps[i];
    if (a.inFlightId != id) continue;
    a.inFlightId = -1;
    const int status = d["status"] | 0;
    if (status < 200 || status >= 300) {
      a.nextDue = millis() + 30000;
      return;
    }
    a.frames.clear();
    for (JsonObjectConst f : d["frames"].as<JsonArrayConst>()) {
      if (a.frames.size() >= MAX_FRAMES) break;
      String t = f["text"] | "";
      if (t.length() > 96) t = t.substring(0, 96);
      a.frames.push_back({t, parseColor(f["color"] | "", a.color), f["icon"] | ""});
    }
    a.fdurMs = (uint16_t)constrain((int)(d["fdur"] | 3), 1, 30) * 1000;
    a.curFrame = -1;
    a.hasColorOv = false;
    const String first = a.frames.empty() ? String() : a.frames[0].text;
    const bool changed = !a.finalText || first != a.value;
    a.finalText = true;
    a.value = first;                     // what the playlist checks; empty = skip the app
    a.updatedAt = millis();
    if (changed) persistValue(i, false);
    return;
  }
}

void applyResult(int32_t id, int status, const String &body, bool hasBody, const String &ikey = "") {
  for (uint8_t i = 0; i < appCount; i++) {
    DataApp &a = apps[i];
    if (a.inFlightId != id) continue;
    a.inFlightId = -1;
    a.finalText = false;
    a.hasColorOv = false;
    a.frames.clear();
    a.curFrame = -1;
    if (status >= 200 && status < 300 && hasBody && body.length()) applyIconKey(i, ikey);
    if (status >= 200 && status < 300 && hasBody && body.length()) {
      bool changed = body != a.value;
      a.value = body;
      a.updatedAt = millis();
      if (changed) persistValue(i, false);
    } else {
      a.nextDue = millis() + 30000;      // ошибка — повтор через 30 с
    }
    return;
  }
}

void pollNet() {
  NetResult r;
  if (netPoll(r)) applyResult(r.id, r.status, r.body, r.status >= 200 && r.status < 300, r.ikey);
}

// ---------- protocol: incoming ----------
void onLinkUp() {
  linkUp = true;
  if (fsNote.length()) {
    sendLog(fsNote);
    fsNote = "";
  }
  for (uint8_t i = 0; i < appCount; i++) {
    apps[i].inFlightId = -1;
    apps[i].nextDue = millis();          // после (пере)подключения — обновить всё
  }
}

void reloadIconUsers(const String &id) {
  for (uint8_t i = 0; i < appCount; i++)
    if (apps[i].curIcon == id) loadIcon(id, appIcons[i]);
  if (id == callIconId) loadIcon(id, callIcon);
}

void applyClockCfg(JsonObjectConst c) {
  clk.on = c["on"] | true;
  clk.durMs = (uint32_t)constrain((int)(c["dur"] | 10), 2, 600) * 1000UL;
  clk.pos = constrain((int)(c["pos"] | 0), 0, MAX_APPS);
  clk.h24 = c["h24"] | true;
  clk.wday = c["wday"] | true;
}

void sendScreen() {
  if (!linkUp) return;
  JsonDocument d;
  d["t"] = "scr";
  d["name"] = current < 0 ? "clock" : apps[current].name.c_str();
  send(d);
}

void netDrop(const char *why);

void handleLine(char *line, Chan from = CH_USB) {
  JsonDocument d;
  if (deserializeJson(d, line) != DeserializationError::Ok) return;
  const char *t = d["t"] | "";
  if (from == CH_USB && netClient.connected()) netDrop("the USB cable took over");
  // no authentication yet: whoever is on the network may drive the clock, but not move it to
  // another Wi-Fi network — that only over the cable
  if (from == CH_NET && !strcmp(t, "wifi")) {
    sendLog("wifi settings can only be changed over USB");
    return;
  }
  lastRx = millis();
  linkChan = from;
  if (!linkUp) onLinkUp();

  if (!strcmp(t, "ping")) {
    JsonDocument r;
    r["t"] = "pong";
    send(r);
  } else if (!strcmp(t, "busy")) {           // хост: «на звонке» — держим экран 15 с, хост повторяет
    const bool was = onCall();
    busyUntil = (d["on"] | false) ? millis() + 15000 : 0;
    if (!was && onCall()) pomoAwayAt = 0;    // a call starting always shows its screen
    const String id = d["icon"] | "";
    if (id != callIconId) {
      callIconId = id;
      callIcon.ok = false;
      if (id.length()) loadIcon(id, callIcon);
    }
  } else if (!strcmp(t, "nav")) {            // the app's "Next screen": like the side buttons
    navigate((d["dir"] | 1) < 0 ? -1 : 1);
  } else if (!strcmp(t, "dnd")) {            // do not disturb until {until} (epoch), 0 = now off
    dndUntil = (time_t)(d["until"] | 0L);
    sendLog(dndActive() ? "do not disturb on" : "do not disturb off");
  } else if (!strcmp(t, "pomo") && (d["min"] | 0) > 0) {   // a plain timer from the app's menu
    plainTimer(d["min"] | 0);
  } else if (!strcmp(t, "pomo")) {           // из приложения: start / stop / toggle (пауза)
    const char *cmd = d["cmd"] | "";
    if (!strcmp(cmd, "start") && !pomoActive()) pomoStartStop();
    else if (!strcmp(cmd, "stop") && pomoActive()) pomoStop();
    else if (!strcmp(cmd, "toggle")) pomoPauseResume();
    else sendPomo();
  } else if (!strcmp(t, "hello?")) {
    sendHello();
  } else if (!strcmp(t, "cfg?")) {
    sendCfg();
  } else if (!strcmp(t, "cfg")) {
    saveCfg(d);
  } else if (!strcmp(t, "time")) {
    time_t epoch = (time_t)(d["epoch"] | 0L);
    String tzp = d["tzp"] | "";
    if (!tzp.length()) tzp = posixFromOffset(d["tz"] | 0);
    if (tzp != tzPosix) { applyTz(tzp); prefs.putString("tzp", tzp); }
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
    onTrustedTime(epoch, "agent");
    agentSynced = true;
  } else if (!strcmp(t, "resp") && d["frames"].is<JsonArrayConst>()) {
    applyFrames(d["id"] | -1, d);
  } else if (!strcmp(t, "resp") && d["text"].is<const char *>()) {
    applyFinal(d["id"] | -1, d);
  } else if (!strcmp(t, "resp")) {
    String body = d["body"].isNull() ? String() : d["body"].as<String>();
    applyResult(d["id"] | -1, d["status"] | 0, body, !d["body"].isNull(), d["ikey"] | "");
  } else if (!strcmp(t, "notify")) {
    notifyText = d["text"] | "";
    notifyColor = parseColor(d["color"] | "#FFFFFF", 0xFFFF);
    notifyUntil = millis() + (uint32_t)(d["dur"] | 6000);
    notifyFont = parseFont(d["font"].as<const char *>(), FONT_DEFAULT);
    notifyIcon.ok = false;
    notifyIconId = d["icon"] | "";
    if (notifyIconId.length()) loadIcon(notifyIconId, notifyIcon);
    scrollX = MW;
  } else if (!strcmp(t, "apps")) {
    JsonArrayConst arr = d["apps"].as<JsonArrayConst>();
    int skipped = loadAppsFromJson(arr);
    gcIcons(arr);
    String s;
    serializeJson(arr, s);
    saveApps(s);                            // агент шлёт apps на каждый hello — одинаковое не пишется
    if (skipped) sendLog("apps: " + String(appCount + skipped) + " switched on, the clock shows the first " + String(MAX_APPS));
    onLinkUp();
    sendLog("apps updated: " + String(appCount));
  } else if (!strcmp(t, "icon")) {
    String id = d["id"] | "";
    if (saveIcon(id, d["px"].as<const char *>(), d["n"] | 1, d["d"].as<JsonArrayConst>())) reloadIconUsers(id);
    else sendLog("icon rejected: " + id);
  } else if (!strcmp(t, "bright")) {
    brightness = constrain((int)(d["v"] | 30), 1, 255);
    matrix->setBrightness(brightness);
    prefs.putUChar("bright", brightness);
  } else if (!strcmp(t, "settings")) {
    // Новый шрифт приложение узнаёт из hello. На остальные настройки hello не отвечаем:
    // приложение на каждый hello заново шлёт настройки, и получалась бесконечная петля.
    bool fontChanged = false;
    if (d["font"].is<const char *>()) {
      fontDefault = parseFont(d["font"].as<const char *>(), fontDefault);
      prefs.putUChar("font", fontDefault);
      scrollX = MW;
      fontChanged = true;
    }
    if (d["clock"].is<JsonObjectConst>()) {
      applyClockCfg(d["clock"].as<JsonObjectConst>());
      String c;
      serializeJson(d["clock"], c);
      if (c != prefs.getString("clk", "")) prefs.putString("clk", c);
    }
    if (d["scroll"].is<int>()) {
      scrollMs = constrain((int)d["scroll"], 20, 250);
      if (prefs.getUShort("scroll", 0) != scrollMs) prefs.putUShort("scroll", scrollMs);
    }
    if (d["pomo"].is<JsonObjectConst>()) {
      applyPomoCfg(d["pomo"].as<JsonObjectConst>());
      String c;
      serializeJson(d["pomo"], c);
      if (c != prefs.getString("pomo", "")) prefs.putString("pomo", c);
      if (!pomo.on && pomoActive()) pomoStop();
    }
    if (d["restart"] | false) {
      sendLog("restarting on request");
      Serial.flush();
      delay(100);
      ESP.restart();
    }
    if (fontChanged) sendHello();
  } else if (!strcmp(t, "wifi")) {
    if (d["ssid"].is<const char *>()) {
      wifiSsid = d["ssid"].as<const char *>();
      wifiPass = d["pass"] | "";
      prefs.putString("wssid", wifiSsid);
      prefs.putString("wpass", wifiPass);
      wifiWasConnected = false;
      wifiApply();
      sendLog(wifiSsid.length() ? "wifi: connecting to " + wifiSsid : "wifi: off");
    }
    sendWifiStatus();
  }
}

void linkDown() {
  linkUp = false;
  linkChan = CH_NONE;
  // запросы, ушедшие агенту, уже не вернутся — сразу пробуем по Wi-Fi
  for (uint8_t i = 0; i < appCount; i++)
    if (apps[i].inFlightId >= 0 && !apps[i].inFlightLocal) { apps[i].inFlightId = -1; apps[i].nextDue = millis(); }
}

// ---------- the host over the local network ----------
// The clock announces itself over mDNS as _dosgatos._tcp (TXT: id, fw) and takes one host on
// NET_PORT, speaking the same NDJSON as over USB. No authentication in this first version:
// anyone on the network can connect (Wi-Fi settings are refused over the network, though).
#define NET_PORT 7766
WiFiServer netServer(NET_PORT);
bool netUp = false;                        // server and mDNS running
char *netBuf = nullptr;                    // line buffer, only while a client is connected
size_t netLen = 0;
bool netOverflow = false;
uint32_t netLastRx = 0;                    // a host that went quiet (Mac asleep) is dropped
#define NET_IDLE_MS 15000

void netWrite(JsonDocument &d) {
  String s;
  serializeJson(d, s);
  s += '\n';
  netClient.write((const uint8_t *)s.c_str(), s.length());
}

void netDrop(const char *why) {
  if (netClient.connected()) {
    JsonDocument d;
    d["t"] = "bye";
    d["why"] = why;
    netWrite(d);
    netClient.stop();
  }
  const bool wasLink = linkChan == CH_NET;
  free(netBuf);
  netBuf = nullptr;
  netLen = 0;
  if (wasLink) linkDown();
}

void netLoop() {
  const bool wifi = WiFi.isConnected();
  if (wifi && !netUp) {
    char host[24];
    snprintf(host, sizeof host, "dos-gatos-%04llx", ESP.getEfuseMac() >> 32);
    if (MDNS.begin(host)) {
      char id[13];
      snprintf(id, sizeof id, "%012llx", ESP.getEfuseMac());
      MDNS.addService("dosgatos", "tcp", NET_PORT);
      MDNS.addServiceTxt("dosgatos", "tcp", "id", String(id));
      MDNS.addServiceTxt("dosgatos", "tcp", "fw", String(FW_VERSION));
    }
    netServer.begin();
    netServer.setNoDelay(true);
    netUp = true;
  } else if (!wifi && netUp) {
    netDrop("Wi-Fi lost");
    netServer.end();
    MDNS.end();
    netUp = false;
  }
  if (!netUp) return;

  if (netServer.hasClient()) {
    WiFiClient c = netServer.available();
    const bool usbHost = linkUp && linkChan == CH_USB;
    const bool busy = netClient.connected() && millis() - netLastRx < NET_IDLE_MS;
    if (usbHost || busy) {
      JsonDocument d;
      d["t"] = "bye";
      d["why"] = usbHost ? "the clock is on USB" : "another host is connected";
      String s;
      serializeJson(d, s);
      s += '\n';
      c.write((const uint8_t *)s.c_str(), s.length());
      c.stop();
    } else {
      netDrop("replaced");
      netClient = c;
      netClient.setNoDelay(true);
      netBuf = (char *)malloc(RX_LINE_MAX);
      netLen = 0;
      netOverflow = false;
      netLastRx = millis();
      JsonDocument d;
      d["t"] = "hi";
      char id[13];
      snprintf(id, sizeof id, "%012llx", ESP.getEfuseMac());
      d["id"] = id;
      d["fw"] = FW_VERSION;
      netWrite(d);
    }
  }
  if (!netBuf) return;
  if (!netClient.connected() || millis() - netLastRx > NET_IDLE_MS) {
    netDrop(netClient.connected() ? "idle" : "gone");
    return;
  }
  while (netClient.available()) {
    const char c = netClient.read();
    if (c == '\n') {
      if (!netOverflow && netLen > 0 && netBuf[0] == '{') {
        netBuf[netLen] = 0;
        netLastRx = millis();
        handleLine(netBuf, CH_NET);
        if (!netBuf) return;                 // dropped while handling
      }
      netLen = 0;
      netOverflow = false;
    } else if (c != '\r') {
      if (netLen < RX_LINE_MAX - 1) netBuf[netLen++] = c;
      else netOverflow = true;
    }
  }
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (!lineOverflow && lineLen > 0 && lineBuf[0] == '{') {
        lineBuf[lineLen] = 0;
        handleLine(lineBuf, CH_USB);
      } else if (lineOverflow) {
        sendLog("message dropped: longer than " + String(RX_LINE_MAX) + " bytes");
      }
      lineLen = 0;
      lineOverflow = false;
    } else if (c != '\r') {
      // Обрывок, принятый во время перезагрузки (порт открыли — ESP32 сбросился), без \n
      // склеивался со следующей строкой и губил её. Строка начинается с '{' — мусор перед ней выбрасываем.
      if (c == '{' && lineLen > 0 && lineBuf[0] != '{') { lineLen = 0; lineOverflow = false; }
      if (lineLen < RX_LINE_MAX - 1) lineBuf[lineLen++] = c;
      else lineOverflow = true;
    }
  }
  if (linkUp && millis() - lastRx > LINK_TIMEOUT_MS) linkDown();
}

// ---------- buttons ----------
struct Btn { uint8_t pin; const char *name; bool prev; uint32_t at; };
bool midLong = false;                  // средняя кнопка: долгое нажатие уже сработало
// technical screen (IP address, why Wi-Fi is down): all three buttons at once; any button closes it
uint32_t infoUntil = 0;
bool comboHeld = false;                // the three were pressed together and aren't all released yet
#define INFO_MS 15000
Btn btns[3] = { {PIN_BTN_L, "left", true, 0}, {PIN_BTN_M, "select", true, 0}, {PIN_BTN_R, "right", true, 0} };

// Слоты плейлиста: data apps в порядке apps.json, часы вставлены на позицию clk.pos.
// Экраны без данных пропускаем; выключенные часы показываем, только если больше нечего.
int slotToApp(int slot) {                 // -> -1 (часы) или индекс app
  int cp = min((int)clk.pos, (int)appCount);
  if (slot == cp) return -1;
  return slot < cp ? slot : slot - 1;
}
int appToSlot(int app) {
  int cp = min((int)clk.pos, (int)appCount);
  if (app < 0) return cp;
  return app < cp ? app : app + 1;
}
bool anyAppHasValue() {
  for (uint8_t i = 0; i < appCount; i++) if (apps[i].value.length()) return true;
  return false;
}
void nextApp(int dir) {
  int n = appCount + 1;
  int slot = appToSlot(current);
  bool clockOk = clk.on || !anyAppHasValue();
  for (int step = 0; step < n; step++) {
    slot = (slot + dir + n) % n;
    int a = slotToApp(slot);
    if (a < 0 ? clockOk : apps[a].value.length() > 0) break;
  }
  int prev = current;
  current = slotToApp(slot);
  shownAt = millis();
  scrollX = MW;
  if (current != prev) sendScreen();
}

uint32_t dwellMs() {
  if (current < 0) return clk.durMs;
  const DataApp &a = apps[current];
  // an app with frames stays until every frame was shown once
  return a.frames.empty() ? a.durMs : max(a.durMs, (uint32_t)a.frames.size() * a.fdurMs);
}

bool dndActive() { return dndUntil && timeSynced && time(nullptr) < dndUntil; }

// Left/right, from the buttons or the app: while the timer or a call is on, the first press leaves its
// screen for the others (it comes back by itself), later ones go through the playlist.
void navigate(int dir) {
  infoUntil = 0;
  const bool fromSticky = stickyActive() && !pomoAwayAt;
  if (stickyActive()) pomoAwayAt = millis();
  if (!fromSticky) nextApp(dir);
  else shownAt = millis();
}

void pollButtons() {
  for (auto &b : btns) {
    bool v = digitalRead(b.pin);
    if (v != b.prev && millis() - b.at > 40) {
      b.at = millis();
      b.prev = v;
      if (!v) {                          // active low: нажата
        if (dndActive()) {               // any button ends "do not disturb", and does nothing else
          dndUntil = 0;
          comboHeld = true;
          midLong = true;
          sendLog("do not disturb off (button)");
          continue;
        }
        if (infoUntil && !comboHeld) {   // any button closes the technical screen, and does nothing else
          infoUntil = 0;
          comboHeld = true;
          midLong = true;
          continue;
        }
        if (linkUp) {
          JsonDocument d;
          d["t"] = "btn";
          d["b"] = b.name;
          send(d);
        }
        if (b.pin == PIN_BTN_M) midLong = false;   // решаем на отпускании: короткое или долгое
        else navigate(b.pin == PIN_BTN_L ? -1 : 1);
      } else if (b.pin == PIN_BTN_M && !midLong) {  // отпущена: короткое нажатие
        if (stickyActive() && pomoAwayAt) pomoAwayAt = 0;        // back to the timer or the call
        else if (onCall()) {}                                    // nothing to do on the call screen
        else if (pomoActive()) pomoPauseResume();
        else if (current >= 0 && apps[current].inFlightId < 0) requestApp(current);
      }
    }
  }
  const bool anyDown = !btns[0].prev || !btns[1].prev || !btns[2].prev;
  if (!btns[0].prev && !btns[1].prev && !btns[2].prev && !comboHeld) {
    comboHeld = true;
    midLong = true;                      // no Pomodoro and no refresh from the middle button
    infoUntil = millis() + INFO_MS;
    scrollX = MW;
  }
  if (!anyDown) comboHeld = false;
  // долгое нажатие средней: помидор старт/стоп, срабатывает ещё до отпускания
  Btn &m = btns[1];
  if (!m.prev && !midLong && millis() - m.at > 700) {
    midLong = true;
    pomoStartStop();
  }
}

// ---------- rendering ----------
void drawIcon(const Icon &ic, bool dim) {
  for (int y = 0; y < ICON_W; y++)
    for (int x = 0; x < ICON_W; x++) {
      uint16_t c = ic.frame()[y * ICON_W + x];
      if (c) matrix->drawPixel(x, y, dim ? dim565(c) : c);
    }
}

// Текст в области [x0, MW): по центру, если помещается; иначе бегущая строка.
// С иконкой область начинается с x=9, иконка рисуется поверх уехавшего влево текста.
int drawText(const String &text, uint16_t color, int8_t font, const Icon *icon = nullptr, bool dimIcon = false) {
  if (font == FONT_DEFAULT) font = fontDefault;
  const String s = glyphText(font, text);
  int startX;
  const int x0 = (icon && icon->ok) ? ICON_W + 1 : 0;
  const int area = MW - x0;
  matrix->setFont(gfxFont(font));
  matrix->setTextColor(color);
  int w = textWidth(font, s);
  if (w <= area) {
    startX = x0 + (area - w + 1) / 2;
  } else {
    if (millis() - lastScroll > scrollMs) { scrollX--; lastScroll = millis(); }
    if (scrollX < x0 - w) scrollX = MW;
    startX = scrollX;
  }
  matrix->setCursor(startX, baselineY(font));
  matrix->print(s);
  if (x0) {
    matrix->fillRect(0, 0, x0, MH, 0);
    drawIcon(*icon, dimIcon);
  }
  return startX;
}

// "ON CALL" next to the icon. In the 3x5 font it just fits beside the icon with a 2-pixel gap
// between the words; other fonts scroll.
void drawCall() {
  const uint16_t red = matrix->Color(255, 40, 40);
  if (fontDefault != FONT_3X5 || !callIcon.ok) {
    drawText("ON CALL", red, FONT_DEFAULT, &callIcon);
    return;
  }
  matrix->setFont(gfxFont(FONT_3X5));
  matrix->setTextColor(red);
  matrix->setCursor(ICON_W, baselineY(FONT_3X5));
  matrix->print("ON");
  matrix->setCursor(ICON_W + 9, baselineY(FONT_3X5));
  matrix->print("CALL");
  drawIcon(callIcon, false);
}

// Technical screen: the IP address, or why there's no Wi-Fi.
void drawInfo() {
  String t;
  uint16_t c = matrix->Color(80, 200, 255);
  if (WiFi.isConnected()) {
    t = "IP " + WiFi.localIP().toString();
  } else {
    c = matrix->Color(255, 150, 60);
    const char *why = wifiReasonText(wifiReason);
    t = !wifiSsid.length() ? "NO WIFI SET" : "NO WIFI " + wifiSsid + (why ? String(": ") + why : String(""));
  }
  t += "  FW " FW_VERSION;
  drawText(t, c, FONT_DEFAULT);
}

// ---------- pomodoro ----------
// Built-in timer: long-press the middle button to start or stop, short-press to pause. Work and
// break alternate on their own, with a long break after every few rounds; the buzzer beeps at
// each change. Settings come from the app (settings.pomo), commands also as {"t":"pomo"}.

void applyPomoCfg(JsonObjectConst c) {
  pomo.on = c["on"] | true;
  pomo.work = constrain((int)(c["work"] | 25), 1, 120);
  pomo.brk = constrain((int)(c["brk"] | 5), 1, 60);
  pomo.lng = constrain((int)(c["long"] | 15), 1, 60);
  pomo.every = constrain((int)(c["every"] | 4), 1, 12);
  pomo.sound = c["sound"] | true;
}

static const char *const TOMATO[8] = {"...gg...", "..rggr..", ".rrrrrr.", "rrrrrrrr", "rrrrrrrr", "rrrrrrrr", ".rrrrrr.", "..rrrr.."};
static const char *const CUP[8] = {"..s.s...", "...s.s..", ".wwwww..", ".wwwwwww", ".wwwww.w", ".wwwwwww", "..www...", "........"};
static const char *const HOURGLASS[8] = {".wwwwww.", ".w....w.", "..wssw..", "...ss...", "...ss...", "..w..w..", ".wssssw.", ".wwwwww."};

void spriteIcon(Icon &ic, const char *const rows[8], char key1, uint16_t c1, char key2, uint16_t c2) {
  ic.n = 1;
  ic.delay[0] = 1000;
  ic.total = 1000;
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      char k = rows[y][x];
      ic.px[0][y * 8 + x] = k == key1 ? c1 : k == key2 ? c2 : 0;
    }
  ic.ok = true;
}

void pomoInit() {
  spriteIcon(pomoTomato, TOMATO, 'r', matrix->Color(255, 70, 50), 'g', matrix->Color(60, 200, 90));
  spriteIcon(pomoCup, CUP, 'w', matrix->Color(255, 241, 220), 's', matrix->Color(110, 110, 120));
  JsonDocument c;
  if (deserializeJson(c, prefs.getString("pomo", "{}")) == DeserializationError::Ok) applyPomoCfg(c.as<JsonObjectConst>());
}

// The buzzer is active: high = sound. Between beeps the pin goes back to a pull-down, as at boot.
void beep(uint8_t n) {
  if (!pomo.sound || dndActive()) return;
  beepsLeft = n;
  beepAt = millis();
}

void pollBeep() {
  if ((!beepsLeft && !beepOn) || millis() < beepAt) return;
  if (beepOn) {
    digitalWrite(PIN_BUZZER, LOW);
    pinMode(PIN_BUZZER, INPUT_PULLDOWN);
    beepOn = false;
  } else {
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, HIGH);
    beepOn = true;
    beepsLeft--;
  }
  beepAt = millis() + 120;
}

bool pomoActive() { return pomoPhase != POMO_IDLE; }
bool stickyActive() { return pomoActive() || onCall(); }

uint32_t pomoLeft() {
  if (pomoPaused) return pomoLeftMs;
  int32_t d = (int32_t)(pomoEndsAt - millis());
  return d > 0 ? d : 0;
}

void sendPomo() {
  if (!linkUp) return;
  JsonDocument d;
  d["t"] = "pomo";
  d["phase"] = POMO_NAMES[pomoPhase];
  d["left"] = pomoLeft() / 1000;
  d["paused"] = pomoPaused;
  d["rounds"] = pomoRounds;
  d["timer"] = pomoOnce;
  d["total"] = pomoPhaseMs / 1000;
  send(d);
}

void pomoStart(PomoPhase ph, uint32_t ms = 0) {
  pomoPhase = ph;
  pomoAwayAt = 0;                           // a new phase always shows the timer
  pomoPhaseMs = ms ? ms : (ph == POMO_WORK ? pomo.work : ph == POMO_BREAK ? pomo.brk : pomo.lng) * 60000UL;
  pomoEndsAt = millis() + pomoPhaseMs;
  pomoPaused = false;
  sendPomo();
}

void pomoStop() {
  pomoPhase = POMO_IDLE;
  pomoOnce = false;
  pomoPaused = false;
  shownAt = millis();
  sendPomo();
}

void pomoStartStop() {
  if (!pomo.on) return;
  if (pomoActive()) {
    pomoStop();
  } else {
    pomoRounds = 0;
    pomoOnce = false;
    spriteIcon(pomoCup, CUP, 'w', matrix->Color(255, 241, 220), 's', matrix->Color(110, 110, 120));
    beep(1);
    pomoStart(POMO_WORK);
  }
}

// A plain timer for `minutes` (1–120): one phase with an hourglass, three beeps at the end.
void plainTimer(int minutes) {
  pomoRounds = 0;
  pomoOnce = true;
  spriteIcon(pomoCup, HOURGLASS, 'w', matrix->Color(255, 200, 90), 's', matrix->Color(255, 241, 220));
  beep(1);
  pomoStart(POMO_WORK, (uint32_t)constrain(minutes, 1, 120) * 60000UL);
}

void pomoPauseResume() {
  if (!pomoActive()) return;
  if (pomoPaused) pomoEndsAt = millis() + pomoLeftMs;
  else pomoLeftMs = pomoLeft();
  pomoPaused = !pomoPaused;
  sendPomo();
}

void pomoLoop() {
  pollBeep();
  if (!pomoActive() || pomoPaused || pomoLeft() > 0) return;
  if (pomoOnce) {                          // a plain timer just ends
    beep(3);
    pomoStop();
    return;
  }
  if (pomoPhase == POMO_WORK) {
    pomoRounds++;
    beep(3);
    pomoStart(pomoRounds % pomo.every == 0 ? POMO_LONG : POMO_BREAK);
  } else {
    beep(2);
    pomoStart(POMO_WORK);
  }
}

void drawPomodoro() {
  const uint32_t left = pomoLeft();
  char buf[8];
  const uint32_t s = (left + 999) / 1000;
  if (s >= 3600) snprintf(buf, sizeof buf, "%lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));   // 1:59
  else snprintf(buf, sizeof buf, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  const bool work = pomoPhase == POMO_WORK && !pomoOnce;
  uint16_t color = pomoOnce ? matrix->Color(255, 200, 90) : work ? matrix->Color(255, 110, 90) : matrix->Color(90, 220, 130);
  if (pomoPaused && (millis() / 500) % 2) color = matrix->Color(70, 70, 70);   // пауза — мигает серым
  drawText(buf, color, FONT_DEFAULT, work ? &pomoTomato : &pomoCup);   // pomoCup is the hourglass for a timer
  // bottom row: how much of the phase is done
  const int x0 = ICON_W + 1, w = MW - x0;
  const int done = (int)((uint64_t)(pomoPhaseMs - left) * w / pomoPhaseMs);
  matrix->drawFastHLine(x0, MH - 1, w, matrix->Color(40, 40, 45));
  if (done > 0) matrix->drawFastHLine(x0, MH - 1, done, pomoOnce ? matrix->Color(200, 140, 40) : work ? matrix->Color(200, 60, 40) : matrix->Color(50, 170, 90));
}

// Полоса дней недели в нижней строке: 7 сегментов по 3 px, понедельник первый, сегодня — оранжевый
void drawWeekdays(int wday) {
  const int today = (wday + 6) % 7;
  for (int d = 0; d < 7; d++)
    matrix->drawFastHLine(2 + d * 4, MH - 1, 3, d == today ? matrix->Color(240, 110, 40) : matrix->Color(45, 45, 50));
}

void drawClock() {
  if (!timeSynced) { drawText("--:--", matrix->Color(80, 80, 80), FONT_DEFAULT); return; }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char buf[6];
  int hh = t.tm_hour;
  if (!clk.h24) hh = hh % 12 ? hh % 12 : 12;
  snprintf(buf, sizeof(buf), "%02d:%02d", hh, t.tm_min);
  // серым — время только из RTC, ни агент, ни NTP его ещё не подтвердили
  int x = drawText(buf, trustedTime ? matrix->Color(255, 240, 220) : matrix->Color(90, 90, 90), FONT_DEFAULT);
  if (t.tm_sec % 2) {
    // мигание: гасим двоеточие, не сдвигая цифры
    const int f = fontDefault;
    const int cx = x + advance(f, buf[0]) + advance(f, buf[1]);
    matrix->fillRect(cx, 0, advance(f, ':') - 1, MH - 1, 0);
  }
  if (clk.wday) drawWeekdays(t.tm_wday);
}

// Точка в правом нижнем углу: нет — USB; синяя — работаем по Wi-Fi; красная — связи нет.
void drawLinkDot() {
  if (linkUp) return;
  matrix->drawPixel(MW - 1, MH - 1, WiFi.isConnected() ? matrix->Color(0, 60, 255) : matrix->Color(255, 0, 0));
}

// Загрузочный экран, как на dosgatos.app: два кота (белый и рыжий) по очереди
// подмигивают, рядом HOLA. Спрайт — design/Clock.dc.html в репозитории desktop.
const char *const CAT[] = {"X.....X", "XX...XX", "XXXXXXX", "X.XXX.X", "XXXXXXX", ".XXXXX."};
const char *const CAT_BLINK[] = {"X.....X", "XX...XX", "XXXXXXX", "XXXXXXX", "XXXXXXX", ".XXXXX."};

void drawCat(int x0, int y0, bool shut, uint16_t color) {
  const char *const *rows = shut ? CAT_BLINK : CAT;
  for (int y = 0; y < 6; y++)
    for (int x = 0; x < 7; x++)
      if (rows[y][x] == 'X') matrix->drawPixel(x0 + x, y0 + y, color);
}

void drawBoot(uint32_t now) {
  // каждые 2 с: белый жмурится на 180 мс, рыжий — через 500 мс после него
  uint32_t phase = (now + 1200) % 2000;
  drawCat(1, 1, phase < 180, matrix->Color(255, 241, 220));
  drawCat(9, 2, phase >= 500 && phase < 680, matrix->Color(255, 122, 61));
  matrix->setFont(gfxFont(FONT_3X5));
  matrix->setTextColor(matrix->Color(255, 241, 220));
  matrix->setCursor(17, 7);
  matrix->print("HOLA");
}

void render() {
  matrix->fillScreen(0);
  uint32_t now = millis();
  if (dndActive()) {                       // do not disturb: dark until the morning (or a button)
    matrix->show();
    return;
  }

  if (now < BOOT_SCREEN_MS && now >= notifyUntil) {
    drawBoot(now);
    shownAt = now;                           // первый экран после заставки — на полное время
  } else if (infoUntil && (int32_t)(infoUntil - now) > 0) {
    drawInfo();
  } else if (now < notifyUntil) {
    drawText(notifyText, notifyColor, notifyFont, &notifyIcon);
  } else if (stickyActive() && (!pomoAwayAt || now - pomoAwayAt > POMO_AWAY_MS)) {
    pomoAwayAt = 0;
    if (onCall()) drawCall();
    else drawPomodoro();
    shownAt = now;                           // после помидора — снова полное время на экран
  } else {
    if (now - shownAt > dwellMs() || (current < 0 && !clk.on && anyAppHasValue())) nextApp(1);
    if (current < 0) {
      drawClock();
    } else {
      DataApp &a = apps[current];
      if (!a.value.length()) {
        nextApp(1);                          // данные пропали (напр. сменился apps.json)
      } else {
        bool stale = a.updatedAt == 0 || now - a.updatedAt > a.everyMs * 3;
        if (!a.frames.empty()) {
          // frames in turn; a new frame starts scrolling from the right and loads its icon
          const int8_t f = ((now - shownAt) / a.fdurMs) % a.frames.size();
          if (f != a.curFrame) {
            a.curFrame = f;
            scrollX = MW;
            applyIconId(current, a.frames[f].icon.length() ? a.frames[f].icon : a.iconId);
          }
          drawText(a.frames[f].text, stale ? matrix->Color(70, 70, 70) : a.frames[f].color, a.font, &appIcons[current], stale);
        } else
        drawText(formatValue(a), stale ? matrix->Color(70, 70, 70) : a.hasColorOv ? a.colorOv : a.color, a.font, &appIcons[current], stale);
      }
    }
  }
  if (now >= BOOT_SCREEN_MS) drawLinkDot();   // на заставке связи ещё и не должно быть
  // on a call but looking at another screen: a red dot in the top-left corner
  if (onCall() && pomoAwayAt && now >= notifyUntil) matrix->drawPixel(0, 0, matrix->Color(255, 0, 0));
  // timer running on another screen: a dot in the top-right corner, red for work, green for a break
  if (pomoActive() && (pomoAwayAt || onCall()) && now >= notifyUntil)
    matrix->drawPixel(MW - 1, 0, pomoPaused && (now / 500) % 2 ? (uint16_t)0 : pomoPhase == POMO_WORK ? matrix->Color(255, 60, 40) : matrix->Color(60, 220, 100));
  matrix->show();
}

// ---------- setup / loop ----------
void setup() {
  pinMode(PIN_BUZZER, INPUT_PULLDOWN);   // иначе пищит
  pinMode(PIN_BTN_L, INPUT_PULLUP);
  pinMode(PIN_BTN_M, INPUT_PULLUP);
  pinMode(PIN_BTN_R, INPUT_PULLUP);

  Serial.setRxBufferSize(8192);
  Serial.begin(SERIAL_BAUD);

  prefs.begin("tc001usb", false);
  brightness = prefs.getUChar("bright", 30);
  scrollMs = constrain((int)prefs.getUShort("scroll", 75), 20, 250);
  fontDefault = constrain((int8_t)prefs.getUChar("font", FONT_3X5), FONT_5X7, FONT_3X5);
  applyTz(prefs.getString("tzp", "UTC0"));
  {
    JsonDocument c;
    if (deserializeJson(c, prefs.getString("clk", "{}")) == DeserializationError::Ok) applyClockCfg(c.as<JsonObjectConst>());
  }

  fsBegin();
  loadCfgRev();

  Wire.begin(PIN_SDA, PIN_SCL);
  restoreTimeFromRtc();

  FastLED.addLeds<NEOPIXEL, PIN_MATRIX>(leds, NUM_LEDS);
  matrix = new FastLED_NeoMatrix(leds, MW, MH,
      NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG);
  matrix->begin();
  matrix->setTextWrap(false);
  matrix->cp437(true);                   // 0xF8 = «°» во встроенном 5x7
  matrix->setBrightness(brightness);
  pomoInit();

  loadApps();
  netBegin();
  wifiSsid = prefs.getString("wssid", "");
  wifiPass = prefs.getString("wpass", "");
  wifiApply();

  shownAt = millis();
  delay(200);
  sendHello();
}

void loop() {
  pollSerial();
  // hello после загрузки может потеряться в выводе ROM-бутлоадера — повторяем, пока агент не прислал время
  if (linkUp && !agentSynced && millis() - lastHello > 3000) {
    lastHello = millis();
    sendHello();
  }
  wifiLoop();
  netLoop();
  pollNet();
  pollButtons();
  pomoLoop();
  updateLocalApps();
  scheduleFetches();
  static uint32_t lastFrame = 0;
  if (millis() - lastFrame >= 20) {
    lastFrame = millis();
    render();
  }
}
