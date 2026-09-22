// TC001 USB prototype firmware
// Часы сами решают, что и когда запрашивать (apps в NVS), а HTTP выполняет агент на Mac
// через USB-serial (CH340). Протокол: NDJSON, 460800 бод, одна JSON-строка на сообщение.
//
// device -> host: hello, req, pong, btn, log
// host -> device: ping, time, resp, notify, apps, bright

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <FastLED_NeoMatrix.h>
#include <Preferences.h>
#include <Wire.h>
#include <sys/time.h>

#define FW_VERSION   "0.3.2"
#define PIN_MATRIX   32
#define PIN_BTN_L    26
#define PIN_BTN_M    27
#define PIN_BTN_R    14
#define PIN_BUZZER   15
#define PIN_SDA      21
#define PIN_SCL      22
#define RTC_ADDR     0x68   // DS1307, хранит UTC
#define RTC_MAX_DRIFT_S 60   // RTC считается рабочим, если на последней сверке ушёл меньше
#define MW 32
#define MH 8
#define NUM_LEDS (MW * MH)

#define SERIAL_BAUD      460800  // CH340 на macOS не держит 921600
#define RX_LINE_MAX         3072
#define LINK_TIMEOUT_MS  10000
#define REQ_TIMEOUT_MS   30000
#define APP_DWELL_MS     8000
#define MAX_APPS         8
#define PERSIST_EVERY_MS 900000   // пишем значение в NVS не чаще раза в 15 мин на app

CRGB leds[NUM_LEDS];
FastLED_NeoMatrix *matrix;
Preferences prefs;

// ---------- data apps (proactive sources) ----------
struct DataApp {
  String name, url, path, find, re, fmt;
  uint16_t keep = 128;
  uint32_t everyMs = 300000;
  float scale = 1.0f;
  int8_t dec = -1;          // -1: показывать значение как есть
  uint16_t color = 0xFFFF;  // RGB565
  // runtime
  String value;
  uint32_t updatedAt = 0;
  uint32_t nextDue = 0;
  int32_t inFlightId = -1;
  uint32_t sentAt = 0;
  uint32_t persistedAt = 0;
  bool persisted = false;
};

DataApp apps[MAX_APPS];
uint8_t appCount = 0;

// ---------- state ----------
uint32_t lastRx = 0;
bool linkUp = false;
bool timeSynced = false;      // время есть (из RTC или от агента)
bool agentTime = false;       // время получено от агента в этой загрузке
uint32_t lastHello = 0;
int32_t tzOffset = 0;
int32_t nextReqId = 1;
uint8_t brightness = 30;

int8_t current = -1;          // -1 = clock, 0..appCount-1 = data app
uint32_t shownAt = 0;
int16_t scrollX = 0;
uint32_t lastScroll = 0;

String notifyText;
uint16_t notifyColor = 0xFFFF;
uint32_t notifyUntil = 0;

char lineBuf[RX_LINE_MAX];
size_t lineLen = 0;
bool lineOverflow = false;

// ---------- helpers ----------
uint16_t parseColor(const char *s, uint16_t def) {
  if (!s || s[0] != '#' || strlen(s) != 7) return def;
  uint32_t v = strtoul(s + 1, nullptr, 16);
  return matrix->Color((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

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

void sendHello() {
  JsonDocument d;
  d["t"] = "hello";
  d["fw"] = FW_VERSION;
  d["apps"] = appCount;
  send(d);
}

// ---------- RTC DS1307 ----------
// Держит время, пока агент не подключён (после перезагрузки или без Mac).
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
  out = mktime(&t);                            // TZ=UTC0, см. setup()
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

// Доверяем RTC, только если на последней сверке с агентом он был точен (флаг в NVS).
// На некоторых TC001 DS1307 отдаёт мусор (питание вне спецификации / клоны).
void restoreTimeFromRtc() {
  if (!prefs.getBool("rtc_ok", false)) return;
  time_t t;
  if (!rtcRead(t)) return;
  struct timeval tv = { t, 0 };
  settimeofday(&tv, nullptr);
  tzOffset = prefs.getInt("tz", 0);
  timeSynced = true;
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
    x.color = parseColor(a["color"] | "#FFFFFF", 0xFFFF);
    if (x.url.length()) {
      restoreValue(appCount);             // updatedAt = 0 → покажется серым, пока не придёт свежее
      appCount++;
    }
  }
  if (current >= appCount) current = -1;
}

void saveApps(const String &json) {
  prefs.putString("apps", json);
}

void loadApps() {
  String s = prefs.getString("apps", "[]");
  JsonDocument d;
  if (deserializeJson(d, s) == DeserializationError::Ok) {
    loadAppsFromJson(d.as<JsonArrayConst>());
  }
}

// ---------- value formatting ----------
String formatValue(const DataApp &a) {
  String v = a.value;
  if (a.dec >= 0 || a.scale != 1.0f) {
    char *end;
    double num = strtod(v.c_str(), &end);
    if (end != v.c_str()) {
      num *= a.scale;
      v = String(num, a.dec >= 0 ? a.dec : 2);
    }
  }
  String out = a.fmt;
  out.replace("{v}", v);
  return out;
}

// ---------- protocol: requests ----------
void requestApp(uint8_t i) {
  DataApp &a = apps[i];
  a.inFlightId = nextReqId++;
  a.sentAt = millis();
  JsonDocument d;
  d["t"] = "req";
  d["id"] = a.inFlightId;
  d["url"] = a.url;
  if (a.path.length()) d["path"] = a.path;
  if (a.find.length()) { d["find"] = a.find; d["keep"] = a.keep; }
  if (a.re.length()) d["re"] = a.re;
  send(d);
}

void scheduleFetches() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < appCount; i++) {
    DataApp &a = apps[i];
    if (a.inFlightId >= 0 && now - a.sentAt > REQ_TIMEOUT_MS) {
      a.inFlightId = -1;                 // ответ потерян — разрешаем повтор
      a.nextDue = now + 10000;
    }
    if (!linkUp || a.inFlightId >= 0) continue;
    if ((int32_t)(now - a.nextDue) >= 0) {
      requestApp(i);
      a.nextDue = now + a.everyMs;
    }
  }
}

void handleResp(JsonDocument &d) {
  int32_t id = d["id"] | -1;
  int status = d["status"] | 0;
  for (uint8_t i = 0; i < appCount; i++) {
    DataApp &a = apps[i];
    if (a.inFlightId != id) continue;
    a.inFlightId = -1;
    if (status >= 200 && status < 300 && !d["body"].isNull()) {
      String v = d["body"].as<String>();
      bool changed = v != a.value;
      a.value = v;
      a.updatedAt = millis();
      if (changed) persistValue(i, false);
    } else {
      a.nextDue = millis() + 30000;      // ошибка — повтор через 30 с
    }
    return;
  }
}

// ---------- protocol: incoming ----------
void onLinkUp() {
  linkUp = true;
  for (uint8_t i = 0; i < appCount; i++) {
    apps[i].inFlightId = -1;
    apps[i].nextDue = millis();          // после (пере)подключения — обновить всё
  }
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
    time_t rtcNow;
    bool rtcOk = false;
    if (rtcRead(rtcNow)) {
      long drift = (long)(rtcNow - epoch);
      rtcOk = labs(drift) < RTC_MAX_DRIFT_S;
      sendLog("rtc drift " + String(drift) + " s, " + (rtcOk ? "ok" : "untrusted"));
    } else {
      sendLog("rtc invalid, untrusted");
    }
    if (rtcOk != prefs.getBool("rtc_ok", false)) prefs.putBool("rtc_ok", rtcOk);
    struct timeval tv = { epoch, 0 };
    settimeofday(&tv, nullptr);
    int32_t tz = d["tz"] | 0;
    if (tz != tzOffset || !timeSynced) prefs.putInt("tz", tz);
    tzOffset = tz;
    timeSynced = true;
    agentTime = true;
    if (!rtcWrite(epoch)) sendLog("rtc write failed");
  } else if (!strcmp(t, "resp")) {
    handleResp(d);
  } else if (!strcmp(t, "notify")) {
    notifyText = d["text"] | "";
    notifyColor = parseColor(d["color"] | "#FFFFFF", 0xFFFF);
    notifyUntil = millis() + (uint32_t)(d["dur"] | 6000);
    scrollX = MW;
  } else if (!strcmp(t, "apps")) {
    JsonArrayConst arr = d["apps"].as<JsonArrayConst>();
    loadAppsFromJson(arr);
    String s;
    serializeJson(arr, s);
    saveApps(s);
    onLinkUp();
    sendLog("apps updated: " + String(appCount));
  } else if (!strcmp(t, "bright")) {
    brightness = constrain((int)(d["v"] | 30), 1, 255);
    matrix->setBrightness(brightness);
    prefs.putUChar("bright", brightness);
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
  if (linkUp && millis() - lastRx > LINK_TIMEOUT_MS) linkUp = false;
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
        JsonDocument d;
        d["t"] = "btn";
        d["b"] = b.name;
        send(d);
        if (b.pin == PIN_BTN_L) nextApp(-1);
        else if (b.pin == PIN_BTN_R) nextApp(1);
        else if (current >= 0 && linkUp && apps[current].inFlightId < 0) requestApp(current);
      }
    }
  }
}

// ---------- rendering ----------
int textWidth(const String &s) { return s.length() * 6; }

// Рисует текст: по центру, если помещается; иначе бегущая строка. Возвращает true, когда прокрутка прошла цикл.
bool drawText(const String &s, uint16_t color) {
  matrix->setTextColor(color);
  int w = textWidth(s);
  if (w <= MW) {
    matrix->setCursor((MW - w) / 2 + 1, 0);
    matrix->print(s);
    return true;
  }
  if (millis() - lastScroll > 45) { scrollX--; lastScroll = millis(); }
  bool done = false;
  if (scrollX < -w) { scrollX = MW; done = true; }
  matrix->setCursor(scrollX, 0);
  matrix->print(s);
  return done;
}

void drawClock() {
  if (!timeSynced) { drawText("--:--", matrix->Color(80, 80, 80)); return; }
  time_t now = time(nullptr) + tzOffset;
  struct tm t;
  gmtime_r(&now, &t);
  char buf[6];
  snprintf(buf, sizeof(buf), (t.tm_sec % 2) ? "%02d:%02d" : "%02d %02d", t.tm_hour, t.tm_min);
  // серым — время только из RTC, агент ещё не подтвердил
  drawText(buf, agentTime ? matrix->Color(255, 255, 255) : matrix->Color(90, 90, 90));
}

void drawLinkDot() {
  if (!linkUp) matrix->drawPixel(MW - 1, MH - 1, matrix->Color(255, 0, 0));
}

void render() {
  matrix->fillScreen(0);
  uint32_t now = millis();

  if (now < notifyUntil) {
    drawText(notifyText, notifyColor);
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
        drawText(formatValue(a), stale ? matrix->Color(70, 70, 70) : a.color);
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

  setenv("TZ", "UTC0", 1);                     // системное время = UTC, смещение применяем сами
  tzset();
  Wire.begin(PIN_SDA, PIN_SCL);
  restoreTimeFromRtc();

  FastLED.addLeds<NEOPIXEL, PIN_MATRIX>(leds, NUM_LEDS);
  matrix = new FastLED_NeoMatrix(leds, MW, MH,
      NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG);
  matrix->begin();
  matrix->setTextWrap(false);
  matrix->setBrightness(brightness);

  loadApps();
  shownAt = millis();
  delay(200);
  sendHello();
}

void loop() {
  pollSerial();
  // hello после загрузки может потеряться в выводе ROM-бутлоадера — повторяем, пока нет времени от агента
  if (linkUp && !agentTime && millis() - lastHello > 3000) {
    lastHello = millis();
    sendHello();
  }
  pollButtons();
  scheduleFetches();
  static uint32_t lastFrame = 0;
  if (millis() - lastFrame >= 20) {
    lastFrame = millis();
    render();
  }
}
