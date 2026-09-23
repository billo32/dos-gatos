#include "net.h"
#include "version.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <regex>

#define NET_MAX_BODY  12288   // без find тело ответа читается целиком — не больше этого
#define NET_MAX_OUT   1024

static QueueHandle_t reqQ, resQ;
static volatile bool busy = false;

// Приёмник тела ответа. HTTPClient::writeToStream сам снимает chunked-кодирование,
// поэтому здесь приходит чистое тело. С find держим только окно после иглы.
class CaptureSink : public Stream {
 public:
  CaptureSink(const String &needle, size_t keep) : needle_(needle), keep_(keep) {
    out.reserve(needle.length() ? keep : 2048);
  }
  String out;
  bool found = false, overflow = false;

  size_t write(uint8_t c) override {
    if (needle_.length() == 0) {
      if (out.length() < NET_MAX_BODY) out += (char)c; else overflow = true;
      return 1;
    }
    if (found) {
      if (out.length() < keep_) out += (char)c;
      return 1;
    }
    // скользящее окно длиной с иглу
    tail_ += (char)c;
    if (tail_.length() > needle_.length()) tail_.remove(0, 1);
    if (tail_ == needle_) { found = true; out = needle_; }
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) override {
    for (size_t i = 0; i < n; i++) write(b[i]);
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

 private:
  String needle_, tail_;
  size_t keep_;
};

static bool walkPath(JsonVariantConst node, const String &path, String &out, String &err) {
  int start = 0;
  while (start <= (int)path.length()) {
    int dot = path.indexOf('.', start);
    String part = path.substring(start, dot < 0 ? path.length() : dot);
    if (node.is<JsonArrayConst>()) {
      char *end;
      long idx = strtol(part.c_str(), &end, 10);
      if (*end) { err = "path: '" + part + "' is not an array index"; return false; }
      JsonArrayConst a = node.as<JsonArrayConst>();
      if (idx < 0 || idx >= (long)a.size()) { err = "path: index out of range"; return false; }
      node = a[idx];
    } else if (node.is<JsonObjectConst>()) {
      JsonVariantConst next = node.as<JsonObjectConst>()[part];
      if (next.isNull()) { err = "path: no key '" + part + "'"; return false; }
      node = next;
    } else {
      err = "path: '" + part + "' on a scalar"; return false;
    }
    if (dot < 0) break;
    start = dot + 1;
  }
  if (node.is<const char *>()) out = node.as<const char *>();
  else { out = ""; serializeJson(node, out); }
  return true;
}

bool extractValue(const String &text, const String &path, const String &re, String &out, String &err) {
  String cur = text;
  if (path.length()) {
    // фильтр ArduinoJson по пути: в памяти остаются только нужные ветки
    JsonDocument filter;
    JsonVariant f = filter.to<JsonVariant>();
    int start = 0;
    while (true) {
      int dot = path.indexOf('.', start);
      String part = path.substring(start, dot < 0 ? path.length() : dot);
      bool isIndex = part.length() && strspn(part.c_str(), "0123456789") == part.length();
      f = isIndex ? f[0].to<JsonVariant>() : f[part].to<JsonVariant>();
      if (dot < 0) break;
      start = dot + 1;
    }
    f.set(true);
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, cur, DeserializationOption::Filter(filter));
    if (e) { err = String("json: ") + e.c_str(); return false; }
    if (!walkPath(doc.as<JsonVariantConst>(), path, cur, err)) return false;
  }
  if (re.length()) {
    try {
      std::regex rx(re.c_str(), std::regex::ECMAScript);
      std::string s(cur.c_str());
      std::smatch m;
      if (!std::regex_search(s, m, rx)) { out = ""; return true; }   // не найдено — не ошибка
      out = (m.size() > 1 ? m[1] : m[0]).str().c_str();
      return true;
    } catch (const std::exception &ex) {
      err = String("re: ") + ex.what();
      return false;
    }
  }
  out = cur;
  return true;
}

static void doFetch(const NetRequest &r, NetResult &res) {
  HTTPClient http;
  WiFiClientSecure tls;
  WiFiClient plain;
  // Сертификаты не проверяются: CA-бандла в прошивке нет. Для публичных API погоды/курсов —
  // приемлемо; для чего-то с секретами в URL — нет (см. README).
  tls.setInsecure();
  http.setTimeout(15000);
  http.setConnectTimeout(10000);
  http.setUserAgent("dos-gatos/" FW_VERSION_STR);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  bool ok = r.url.startsWith("https://") ? http.begin(tls, r.url) : http.begin(plain, r.url);
  if (!ok) { res.error = "bad url"; return; }
  int code = http.GET();
  if (code <= 0) { res.error = HTTPClient::errorToString(code); http.end(); return; }
  res.status = code;
  if (code < 200 || code >= 300) { http.end(); return; }

  CaptureSink sink(r.find, r.keep ? r.keep : 128);
  http.writeToStream(&sink);
  http.end();
  if (r.find.length() && !sink.found) return;                  // игла не найдена → body пустой
  if (sink.overflow) { res.error = "response too large, use find"; res.status = 0; return; }

  String val, err;
  if (!extractValue(sink.out, r.path, r.re, val, err)) { res.error = err; res.status = 0; return; }
  res.body = val.length() > NET_MAX_OUT ? val.substring(0, NET_MAX_OUT) : val;
}

static void netTask(void *) {
  for (;;) {
    NetRequest *r;
    if (xQueueReceive(reqQ, &r, portMAX_DELAY) != pdTRUE) continue;
    NetResult *res = new NetResult{r->id, 0, String(), String()};
    if (WiFi.isConnected()) doFetch(*r, *res);
    else res->error = "wifi down";
    delete r;
    xQueueSend(resQ, &res, portMAX_DELAY);
    busy = false;
  }
}

void netBegin() {
  reqQ = xQueueCreate(1, sizeof(NetRequest *));
  resQ = xQueueCreate(2, sizeof(NetResult *));
  // TLS-рукопожатие и std::regex требуют большого стека
  xTaskCreatePinnedToCore(netTask, "net", 20480, nullptr, 1, nullptr, 0);
}

bool netBusy() { return busy; }

bool netSubmit(const NetRequest &r) {
  if (busy) return false;
  busy = true;
  NetRequest *p = new NetRequest(r);
  if (xQueueSend(reqQ, &p, 0) != pdTRUE) { delete p; busy = false; return false; }
  return true;
}

bool netPoll(NetResult &out) {
  NetResult *p;
  if (xQueueReceive(resQ, &p, 0) != pdTRUE) return false;
  out = *p;
  delete p;
  return true;
}
