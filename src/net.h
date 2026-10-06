// Автономные HTTP(S)-запросы по Wi-Fi, когда агента на USB нет.
// Запрос выполняется в отдельной задаче FreeRTOS (ядро 0), чтобы не замораживать рендер.
#pragma once
#include <Arduino.h>

struct NetRequest {
  int32_t id;
  String url, path, find, re;
  uint16_t keep;
  String ipath;    // второе значение из того же ответа — по нему выбирается иконка (напр. код погоды)
  uint8_t nh = 0;  // заголовки запроса (Authorization, X-Api-Key…)
  String hk[4], hv[4];
};

struct NetResult {
  int32_t id;
  int status;      // HTTP-код, 0 — нет ответа или ошибка извлечения
  String body;     // извлечённое значение; пусто, если не найдено
  String error;
  String ikey;     // значение по ipath, если было
};

void netBegin();                          // создать задачу и очереди
bool netBusy();                           // выполняется ли запрос
bool netSubmit(const NetRequest &r);      // false — занято
bool netPoll(NetResult &out);             // готовый результат (вызывать из loop)

// То же извлечение, что делает агент на Mac: find/keep → path → re.
// Экспортировано для тестов и повторного использования.
bool extractValue(const String &text, const String &path, const String &re, String &out, String &err);
