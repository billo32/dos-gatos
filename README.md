# dos-gatos

Альтернативная прошивка для пиксельных часов Ulanzi TC001 и агент для macOS.

Часы сами решают, **что** и **когда** запрашивать (список источников хранится в NVS). Каналов два:

- **USB (приоритет).** HTTP-запросы выполняет агент на Mac, связь по USB-serial (CH340), 460800 бод.
- **Wi‑Fi (fallback).** Если агента нет больше 10 с (кабель отключён, Mac спит), часы сами ходят в интернет по HTTPS, извлекают значение (`find`/`path`/`re`) и берут время по NTP. Как только агент снова на связи, запросы опять идут через него.

```
[TC001: планировщик] --req{url,path|find,re}--> USB 460800 --> [TC001 Agent на Mac] --HTTPS--> API
          |        <--resp{status, body: "24.6"}--
          +-- нет агента 10 с --> Wi‑Fi --HTTPS (без проверки сертификата)--> API
```

## 1. Прошивка

Сначала сделай бэкап заводской прошивки или AWTRIX (флеш 4 МБ):

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install esptool pyserial certifi
PORT=$(ls /dev/cu.usbserial-* | head -1)
esptool.py --chip esp32 --port $PORT read_flash 0 0x400000 tc001-backup.bin
```

Проще всего прошить из браузера: **[billo32.github.io/dos-gatos](https://billo32.github.io/dos-gatos/)** (Chrome или Edge на компьютере). Страница собирается из `web/` workflow `pages.yml` при каждом изменении прошивки в `main`.

Вручную: прошить образ `tc001-usb-*-merged.bin` из [Releases](https://github.com/billo32/dos-gatos/releases). На 921600 CH340 на macOS срывается, поэтому скорость 460800:

```bash
esptool.py --chip esp32 --port $PORT --baud 460800 write_flash 0x0 tc001-usb-v0.3.2-merged.bin
```

Или собрать и прошить из исходников: `cd firmware && pio run -t upload`.

Вернуть бэкап: `esptool.py --chip esp32 --port $PORT write_flash 0x0 tc001-backup.bin`.

## 2. Приложение в трее (TC001 Agent)

Rust + Tauri: иконка в строке меню, окно настроек источников с кнопкой «Проверить источник», автозапуск при входе.
Заменяет Python-агент и LaunchAgent. Лог, API на `127.0.0.1:7765` и протокол те же.

Сборка (нужны Xcode Command Line Tools, Rust, Node):

```bash
./agent/install-launchagent.sh uninstall     # старый агент держит порт — убрать
cd desktop
npm install
npm run build
cp -R "src-tauri/target/release/bundle/macos/TC001 Agent.app" /Applications/
open "/Applications/TC001 Agent.app"
```

Первый запуск делай из `/Applications`: автозапуск запоминает путь к приложению.
Конфиг источников лежит в `~/Library/Application Support/dev.tls1.tc001/apps.json`, при первом запуске он копируется из `agent/apps.json`.
Отладка без железа: `TC001_PORT=<pty из fake_device.py> "/Applications/TC001 Agent.app/Contents/MacOS/tc001-agent"`.

### Подписанная сборка (Developer ID)

Нужны сертификат **Developer ID Application** в связке ключей (Xcode → Settings → Accounts → Manage Certificates → «+») и ключ App Store Connect API для нотаризации.

```bash
cd desktop
cp signing.env.example signing.env   # вписать Issuer ID, Key ID и путь к .p8
npm run release
```

Скрипт подписывает `.app` с hardened runtime, нотаризует `.app` и `.dmg` и прикрепляет к ним подтверждение нотаризации (stapler).

### Релиз с Mac одной командой

Двойной клик по `Release.command` в Finder (или `scripts/release.sh 0.3.2` в терминале). При первом запуске он поставит `gh` и попросит войти в GitHub.

Скрипт проставляет версию, собирает прошивку (PlatformIO) и universal-приложение (подписанное, если есть Developer ID), делает коммит и тег, пушит и создаёт GitHub Release с `.dmg`, `.app.zip`, образом прошивки и `SHA256SUMS.txt`. Готовый образ прошивки можно передать через `FIRMWARE_BIN=/путь/merged.bin`.

### Релиз через GitHub Actions

Тег `v*` запускает `.github/workflows/release.yml`: сборку прошивки, universal-сборку приложения с подписью и нотаризацией и черновик релиза с `.dmg` и образом прошивки. Нужные секреты перечислены в начале workflow.

```bash
git tag v0.3.2 && git push origin v0.3.2
```

## 3. Агент на Python (для отладки)

```bash
cd agent
python3 agent.py            # порт находится сам по VID:PID CH340; -v — подробный лог
```

При подключении агент синхронизирует время и отправляет `apps.json` на часы.
Дальше часы работают по своему расписанию.

Уведомления из любых скриптов:

```bash
curl -X POST 127.0.0.1:7765/notify -d '{"text":"Deploy OK","color":"#00FF00","dur":5000}'
curl -X POST 127.0.0.1:7765/bright -d '{"v":60}'
curl 127.0.0.1:7765/status
```

### Автозапуск

```bash
./agent/install-launchagent.sh            # старт при входе в систему, перезапуск при падении
./agent/install-launchagent.sh uninstall
tail -f ~/Library/Logs/tc001-agent.log
```

После правки `apps.json` или `agent.py` перезапусти агент: `launchctl kickstart -k gui/$(id -u)/dev.tls1.tc001-agent`.

Проверка без железа: `python3 fake_device.py` выводит путь pty, затем `python3 agent.py --port <путь>`.

## apps.json

| поле | смысл |
|---|---|
| `url` | что запрашивать |
| `every` | период в секундах (часы сами планируют запросы) |
| `path` | путь в JSON: `current.temperature_2m`, `list.0.price` |
| `find` + `keep` | окно из `keep` символов после подстроки, как в AWTRIX NG |
| `re` | регулярка поверх результата: первая группа или всё совпадение |
| `scale`, `dec` | умножить число и округлить до `dec` знаков |
| `fmt` | шаблон, например `"{v}C"` |
| `color` | `#RRGGBB` |
| `icon` | ID иконки из [галереи LaMetric](https://developer.lametric.com/icons), например `"2422"` |
| `font` | `5x7`, `4x6` или `3x5`; без поля — шрифт часов по умолчанию |

По USB извлечение делает агент, по Wi‑Fi — сами часы, правила одинаковые.

### Иконки

Агент скачивает иконку с LaMetric (первый кадр, 8×8), кэширует в `~/Library/Application Support/dev.tls1.tc001/icons/` и отправляет на часы. Часы хранят её в LittleFS, поэтому в Wi‑Fi‑режиме иконки продолжают показываться. С иконкой текст занимает правые 23 пикселя.

### Шрифты

| шрифт | символов на экране | источник |
|---|---|---|
| `5x7` | 5 | встроенный шрифт Adafruit GFX |
| `4x6` | 8 | X11 misc-fixed 4x6 (public domain) |
| `3x5` | 8–10 | Tom Thumb (BSD-3) |

Шрифт по умолчанию выбирается во вкладке Device, для отдельного источника — полем `font`. Файлы шрифтов генерирует `tools/gen_fonts.py`.

## Экран и кнопки

- Ротация: часы → apps, по 8 с на экран. Длинный текст прокручивается.
- Левая и правая кнопки листают экраны, средняя принудительно обновляет текущий.
- Точка в правом нижнем углу: нет — связь по USB; синяя — работа по Wi‑Fi; красная — нет ни USB, ни Wi‑Fi.
- Серые часы: время взято только из RTC, ни агент, ни NTP его ещё не подтвердили.
- Серый текст: данные устарели (старше 3×`every`) или восстановлены из памяти после перезагрузки. Экраны без данных пропускаются.

## Протокол (NDJSON, 460800 бод)

- device → host: `hello{fw,apps,font,wifi}`, `req{id,url,path?,find?,keep?,re?}`, `pong`, `btn`, `log`, `wifi{ssid,state,ip?,rssi?}`
- host → device: `hello?`, `ping` (каждые 3 с), `time{epoch,tz,tzp}`, `resp{id,status,body}`, `notify{text,color,dur,icon?,font?}`, `apps`, `bright`, `icon{id,px}` (8×8 RGB565, 256 hex), `wifi{ssid,pass}`, `settings{font}`

`tzp` — правило часового пояса в формате POSIX (например `CET-1CEST,M3.5.0,M10.5.0/3`), агент берёт его из `/etc/localtime`. По нему часы переводят время на летнее и зимнее в Wi‑Fi‑режиме.

Локальный API агента: `POST /notify`, `/apps`, `/bright`, `/wifi {ssid,pass}`, `/font {font}`, `GET /status`.

## Известные ограничения прототипа

- В Wi‑Fi‑режиме HTTPS-сертификаты не проверяются (в прошивке нет набора корневых сертификатов). Для публичных данных вроде погоды и курсов это приемлемо; секреты в URL источников не кладите.
- Иконки загружаются только через агента: часы, ни разу не подключённые к Mac, показывают источники без иконок.
- С версии 0.4.0 используется таблица разделов `huge_app` (3 МБ под прошивку, без OTA). NVS на прежнем адресе, настройки сохраняются.
- Время хранится в RTC DS1307 (0x68) в UTC. После перезагрузки оно используется, только если на последней сверке с агентом RTC ушёл меньше чем на 60 с (флаг `rtc_ok` в NVS); иначе часы показывают `--:--` до синхронизации. Сверка видна в логе: `grep rtc ~/Library/Logs/tc001-agent.log`. Смещение часового пояса лежит в NVS. Агент подстраивает время при подключении и затем раз в час, поэтому переход на летнее и зимнее время подхватывается в течение часа.
- Шрифты только ASCII.
- Открытие порта может перезагрузить ESP32 через DTR/RTS. Агент ставит обе линии в неактивное состояние; если ресет всё равно случается, часы переподключаются примерно за 1 с.

## Лицензия

MIT, см. `LICENSE`. Сторонние компоненты и условия API перечислены в `THIRD_PARTY_NOTICES.md`.
