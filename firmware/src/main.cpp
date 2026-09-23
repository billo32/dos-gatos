// dos-gatos — прошивка для Ulanzi TC001
// Часы сами решают, что и когда запрашивать (apps в NVS).
//   USB (приоритет): HTTP выполняет агент на Mac, протокол NDJSON 460800 бод.
//   Wi-Fi (fallback): если агента нет, часы ходят в интернет сами, время — по NTP.
//
// device -> host: hello, req, pong, btn, log, wifi
// host -> device: hello?, ping, time, resp, notify, apps, bright, icon, wifi, settings

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <FastLED_NeoMatrix.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <esp_system.h>
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
#define RX_LINE_MAX      6144
#define LINK_TIMEOUT_MS  10000
#define REQ_TIMEOUT_MS   30000
#define APP_DWELL_MS     8000
#define MAX_APPS         8
#define PERSIST_EVERY_MS 900000   // пишем значение в NVS не чаще раза в 15 мин на app
#define ICON_W           8
#define ICON_PX          (ICON_W * ICON_W)

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
String glyphText(int8_t f, const String &s) {
  String t = s;
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

// ---------- icons (8×8 RGB565, LittleFS /i/<id>.bin) ----------
struct Icon {
  bool ok = false;
  uint16_t px[ICON_PX];
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
  ic.ok = f.read((uint8_t *)ic.px, sizeof(ic.px)) == sizeof(ic.px);
  f.close();
  return ic.ok;
}

bool saveIcon(const String &id, const char *hex) {
  if (!validIconId(id) || !hex || strlen(hex) != ICON_PX * 4) return false;
  uint16_t px[ICON_PX];
  for (int i = 0; i < ICON_PX; i++) {
    char b[5] = {hex[i * 4], hex[i * 4 + 1], hex[i * 4 + 2], hex[i * 4 + 3], 0};
    px[i] = (uint16_t)strtoul(b, nullptr, 16);
  }
  const String path = "/i/" + id + ".bin";
  if (LittleFS.exists(path)) {                 // та же иконка уже лежит — флеш не трогаем
    uint16_t old[ICON_PX];
    File r = LittleFS.open(path, "r");
    bool same = r && r.read((uint8_t *)old, sizeof(old)) == sizeof(old) && !memcmp(old, px, sizeof(px));
    if (r) r.close();
    if (same) return true;
  }
  File f = LittleFS.open(path, "w", true);
  if (!f) return false;
  bool ok = f.write((uint8_t *)px, sizeof(px)) == sizeof(px);
  f.close();
  return ok;
}

// ---------- data apps (proactive sources) ----------
struct DataApp {
  String name, url, path, find, re, fmt, iconId;
  uint16_t keep = 128;
  uint32_t everyMs = 300000;
  float scale = 1.0f;
  int8_t dec = -1;          // -1: показывать значение как есть
  int8_t font = FONT_DEFAULT;
  uint16_t color = 0xFFFF;  // RGB565
  Icon icon;
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
uint8_t appCount = 0;

// ---------- state ----------
uint32_t lastRx = 0;
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
uint32_t shownAt = 0;
int16_t scrollX = MW;
uint32_t lastScroll = 0;

String notifyText;
uint16_t notifyColor = 0xFFFF;
uint32_t notifyUntil = 0;
int8_t notifyFont = FONT_DEFAULT;
Icon notifyIcon;

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
  serializeJson(doc, Serial);
  Serial.write('\n');
}

void sendLog(const String &m) {
  JsonDocument d;
  d["t"] = "log";
  d["m"] = m;
  send(d);
}

void fillWifiStatus(JsonObject w) {
  w["ssid"] = wifiSsid;
  const char *state = !wifiSsid.length() ? "off" : WiFi.isConnected() ? "connected" : "connecting";
  w["state"] = state;
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

void sendHello() {
  JsonDocument d;
  d["t"] = "hello";
  d["fw"] = FW_VERSION;
  d["rst"] = resetReason();                // причина последней перезагрузки — для диагностики
  d["heap"] = ESP.getFreeHeap();
  d["apps"] = appCount;
  d["font"] = FONT_NAMES[fontDefault];
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
  prefs.putString(("v" + String(i)).c_str(), a.name + "\t" + a.value);
  a.persistedAt = millis();
  a.persisted = true;
}

void restoreValue(uint8_t i) {
  DataApp &a = apps[i];
  String s = prefs.getString(("v" + String(i)).c_str(), "");
  int tab = s.indexOf('\t');
  if (tab > 0 && s.substring(0, tab) == a.name) a.value = s.substring(tab + 1);
}

void loadAppsFromJson(JsonArrayConst arr) {
  appCount = 0;
  for (JsonObjectConst a : arr) {
    if (appCount >= MAX_APPS) break;
    DataApp &x = apps[appCount];
    x = DataApp();
    x.name = a["name"] | "app";
    x.url = a["url"] | "";
    x.path = a["path"] | "";
    x.find = a["find"] | "";
    x.re = a["re"] | "";
    x.keep = a["keep"] | 128;
    x.fmt = a["fmt"] | "{v}";
    x.everyMs = (uint32_t)(a["every"] | 300) * 1000UL;
    x.scale = a["scale"] | 1.0f;
    x.dec = a["dec"] | -1;
    x.font = parseFont(a["font"].as<const char *>(), FONT_DEFAULT);
    x.color = parseColor(a["color"] | "#FFFFFF", 0xFFFF);
    x.iconId = String(a["icon"] | "");
    if (x.iconId.length()) loadIcon(x.iconId, x.icon);
    if (x.url.length()) {
      restoreValue(appCount);             // updatedAt = 0 → покажется серым, пока не придёт свежее
      appCount++;
    }
  }
  if (current >= appCount) current = -1;
}

void loadApps() {
  String s = prefs.getString("apps", "[]");
  JsonDocument d;
  if (deserializeJson(d, s) == DeserializationError::Ok) loadAppsFromJson(d.as<JsonArrayConst>());
}

// ---------- value formatting ----------
String formatValue(const DataApp &a) {
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
    send(d);
    return true;
  }
  if (WiFi.isConnected() && !netBusy()) {
    int32_t id = nextReqId++;
    if (!netSubmit({id, a.url, a.path, a.find, a.re, a.keep})) return false;
    a.inFlightId = id;
    a.inFlightLocal = true;
    a.sentAt = millis();
    return true;
  }
  return false;
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
    if (a.inFlightId >= 0 || (!linkUp && !canLocal)) continue;
    if ((int32_t)(now - a.nextDue) >= 0) {
      if (requestApp(i)) a.nextDue = now + a.everyMs;
      else break;                        // Wi-Fi-задача занята — остальные в следующий проход
    }
  }
}

void applyResult(int32_t id, int status, const String &body, bool hasBody) {
  for (uint8_t i = 0; i < appCount; i++) {
    DataApp &a = apps[i];
    if (a.inFlightId != id) continue;
    a.inFlightId = -1;
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
  if (netPoll(r)) applyResult(r.id, r.status, r.body, r.status >= 200 && r.status < 300);
}

// ---------- protocol: incoming ----------
void onLinkUp() {
  linkUp = true;
  for (uint8_t i = 0; i < appCount; i++) {
    apps[i].inFlightId = -1;
    apps[i].nextDue = millis();          // после (пере)подключения — обновить всё
  }
}

void reloadIconUsers(const String &id) {
  for (uint8_t i = 0; i < appCount; i++)
    if (apps[i].iconId == id) loadIcon(id, apps[i].icon);
}

void handleLine(char *line) {
  JsonDocument d;
  if (deserializeJson(d, line) != DeserializationError::Ok) return;
  const char *t = d["t"] | "";
  lastRx = millis();
  if (!linkUp) onLinkUp();

  if (!strcmp(t, "ping")) {
    JsonDocument r;
    r["t"] = "pong";
    send(r);
  } else if (!strcmp(t, "hello?")) {
    sendHello();
  } else if (!strcmp(t, "time")) {
    time_t epoch = (time_t)(d["epoch"] | 0L);
    String tzp = d["tzp"] | "";
    if (!tzp.length()) tzp = posixFromOffset(d["tz"] | 0);
    if (tzp != tzPosix) { applyTz(tzp); prefs.putString("tzp", tzp); }
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
    onTrustedTime(epoch, "agent");
    agentSynced = true;
  } else if (!strcmp(t, "resp")) {
    String body = d["body"].isNull() ? String() : d["body"].as<String>();
    applyResult(d["id"] | -1, d["status"] | 0, body, !d["body"].isNull());
  } else if (!strcmp(t, "notify")) {
    notifyText = d["text"] | "";
    notifyColor = parseColor(d["color"] | "#FFFFFF", 0xFFFF);
    notifyUntil = millis() + (uint32_t)(d["dur"] | 6000);
    notifyFont = parseFont(d["font"].as<const char *>(), FONT_DEFAULT);
    notifyIcon.ok = false;
    if (d["icon"].is<const char *>()) loadIcon(d["icon"].as<const char *>(), notifyIcon);
    scrollX = MW;
  } else if (!strcmp(t, "apps")) {
    JsonArrayConst arr = d["apps"].as<JsonArrayConst>();
    loadAppsFromJson(arr);
    String s;
    serializeJson(arr, s);
    if (s != prefs.getString("apps", "")) prefs.putString("apps", s);   // агент шлёт apps на каждый hello
    onLinkUp();
    sendLog("apps updated: " + String(appCount));
  } else if (!strcmp(t, "icon")) {
    String id = d["id"] | "";
    if (saveIcon(id, d["px"].as<const char *>())) reloadIconUsers(id);
    else sendLog("icon rejected: " + id);
  } else if (!strcmp(t, "bright")) {
    brightness = constrain((int)(d["v"] | 30), 1, 255);
    matrix->setBrightness(brightness);
    prefs.putUChar("bright", brightness);
  } else if (!strcmp(t, "settings")) {
    if (d["font"].is<const char *>()) {
      fontDefault = parseFont(d["font"].as<const char *>(), fontDefault);
      prefs.putUChar("font", fontDefault);
      scrollX = MW;
    }
    sendHello();
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

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (!lineOverflow && lineLen > 0 && lineBuf[0] == '{') {
        lineBuf[lineLen] = 0;
        handleLine(lineBuf);
      }
      lineLen = 0;
      lineOverflow = false;
    } else if (c != '\r') {
      if (lineLen < RX_LINE_MAX - 1) lineBuf[lineLen++] = c;
      else lineOverflow = true;
    }
  }
  if (linkUp && millis() - lastRx > LINK_TIMEOUT_MS) {
    linkUp = false;
    // запросы, ушедшие агенту, уже не вернутся — сразу пробуем по Wi-Fi
    for (uint8_t i = 0; i < appCount; i++)
      if (apps[i].inFlightId >= 0 && !apps[i].inFlightLocal) { apps[i].inFlightId = -1; apps[i].nextDue = millis(); }
  }
}

// ---------- buttons ----------
struct Btn { uint8_t pin; const char *name; bool prev; uint32_t at; };
Btn btns[3] = { {PIN_BTN_L, "left", true, 0}, {PIN_BTN_M, "select", true, 0}, {PIN_BTN_R, "right", true, 0} };

// Экраны без данных пропускаем; часы показываются всегда.
void nextApp(int dir) {
  int n = appCount + 1;                  // + clock
  int idx = current + 1;                 // clock = 0
  for (int step = 0; step < n; step++) {
    idx = (idx + dir + n) % n;
    if (idx == 0 || apps[idx - 1].value.length()) break;
  }
  current = idx - 1;
  shownAt = millis();
  scrollX = MW;
}

void pollButtons() {
  for (auto &b : btns) {
    bool v = digitalRead(b.pin);
    if (v != b.prev && millis() - b.at > 40) {
      b.at = millis();
      b.prev = v;
      if (!v) {                          // active low: нажата
        if (linkUp) {
          JsonDocument d;
          d["t"] = "btn";
          d["b"] = b.name;
          send(d);
        }
        if (b.pin == PIN_BTN_L) nextApp(-1);
        else if (b.pin == PIN_BTN_R) nextApp(1);
        else if (current >= 0 && apps[current].inFlightId < 0) requestApp(current);
      }
    }
  }
}

// ---------- rendering ----------
void drawIcon(const Icon &ic, bool dim) {
  for (int y = 0; y < ICON_W; y++)
    for (int x = 0; x < ICON_W; x++) {
      uint16_t c = ic.px[y * ICON_W + x];
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
    if (millis() - lastScroll > 45) { scrollX--; lastScroll = millis(); }
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
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  // серым — время только из RTC, ни агент, ни NTP его ещё не подтвердили
  int x = drawText(buf, trustedTime ? matrix->Color(255, 240, 220) : matrix->Color(90, 90, 90), FONT_DEFAULT);
  if (t.tm_sec % 2) {
    // мигание: гасим двоеточие, не сдвигая цифры
    const int f = fontDefault;
    const int cx = x + advance(f, buf[0]) + advance(f, buf[1]);
    matrix->fillRect(cx, 0, advance(f, ':') - 1, MH - 1, 0);
  }
  drawWeekdays(t.tm_wday);
}

// Точка в правом нижнем углу: нет — USB; синяя — работаем по Wi-Fi; красная — связи нет.
void drawLinkDot() {
  if (linkUp) return;
  matrix->drawPixel(MW - 1, MH - 1, WiFi.isConnected() ? matrix->Color(0, 60, 255) : matrix->Color(255, 0, 0));
}

void render() {
  matrix->fillScreen(0);
  uint32_t now = millis();

  if (now < notifyUntil) {
    drawText(notifyText, notifyColor, notifyFont, &notifyIcon);
  } else {
    if (now - shownAt > APP_DWELL_MS) nextApp(1);
    if (current < 0) {
      drawClock();
    } else {
      DataApp &a = apps[current];
      if (!a.value.length()) {
        nextApp(1);                          // данные пропали (напр. сменился apps.json)
      } else {
        bool stale = a.updatedAt == 0 || now - a.updatedAt > a.everyMs * 3;
        drawText(formatValue(a), stale ? matrix->Color(70, 70, 70) : a.color, a.font, &a.icon, stale);
      }
    }
  }
  drawLinkDot();
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
  fontDefault = constrain((int8_t)prefs.getUChar("font", FONT_3X5), FONT_5X7, FONT_3X5);
  applyTz(prefs.getString("tzp", "UTC0"));

  if (!LittleFS.begin(true)) sendLog("littlefs mount failed");
  LittleFS.mkdir("/i");

  Wire.begin(PIN_SDA, PIN_SCL);
  restoreTimeFromRtc();

  FastLED.addLeds<NEOPIXEL, PIN_MATRIX>(leds, NUM_LEDS);
  matrix = new FastLED_NeoMatrix(leds, MW, MH,
      NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG);
  matrix->begin();
  matrix->setTextWrap(false);
  matrix->cp437(true);                   // 0xF8 = «°» во встроенном 5x7
  matrix->setBrightness(brightness);

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
  pollNet();
  pollButtons();
  scheduleFetches();
  static uint32_t lastFrame = 0;
  if (millis() - lastFrame >= 20) {
    lastFrame = millis();
    render();
  }
}
