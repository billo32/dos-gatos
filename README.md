# dos-gatos

Альтернативная прошивка для пиксельных часов Ulanzi TC001 и агент для macOS.

Часы сами решают, **что** и **когда** запрашивать (список источников хранится в NVS). Каналов два:

- **USB (приоритет).** HTTP-запросы выполняет агент на Mac, связь по USB-serial (CH340), 460800 бод.
- **Wi‑Fi (fallback).** Если агента нет больше 10 с (кабель отключён, Mac спит), часы сами ходят в интернет по HTTPS, извлекают значение (`find`/`path`/`re`) и берут время по NTP. Как только агент снова на связи, запросы опять идут через него.

```
[TC001: планировщик] --req{url,path|find,re}--> USB 460800 --> [Dos GatOS на Mac] --HTTPS--> API
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

## 2. Приложение Dos GatOS (строка меню)

Rust + Tauri 2, интерфейс — React 19 + TypeScript + Tailwind v4 (`desktop/web`, макеты — `design/*.dc.html`).
Иконка в строке меню, окно с разделами Playlist / Sources / Device / Log, автозапуск при входе.
Заменяет Python-агент, LaunchAgent и прежнее приложение «TC001 Agent» (при первом запуске забирает его `apps.json`,
иконки и автозапуск). API на `127.0.0.1:7765` и протокол те же.

- **Playlist** — порядок экранов на часах (перетаскивание или ⌥↑/⌥↓), вкл/выкл, длительность; часы, источники и уведомления.
- **Sources** — REST-источники, редактор с превью и кнопкой Test.
- **Device** — яркость, шрифт, часовой пояс, 24 ч, Wi‑Fi fallback, перезапуск часов.
- **Log** — живой лог, фильтр проблем, экспорт.

Сборка (нужны Xcode Command Line Tools, Rust, Node ≥ 20.19):

```bash
cd desktop
npm install
npm run build            # сначала собирает интерфейс (npm run build:web), потом приложение
cp -R "src-tauri/target/release/bundle/macos/Dos GatOS.app" /Applications/
open "/Applications/Dos GatOS.app"
```

Интерфейс без приложения: `npm run dev:web` (в браузере работает на тестовых данных). Тесты: `npm test`, `cargo test`.

Первый запуск делай из `/Applications`: автозапуск запоминает путь к приложению.
Конфиг: `~/Library/Application Support/dev.tls1.dosgatos/apps.json` (источники, формат прежний; `dur` и `off` — флаги плейлиста)
и `settings.json` (экран часов, уведомления, яркость, пояс). Пароль Wi‑Fi на Mac не сохраняется.
Лог: `~/Library/Logs/DosGatOS/dosgatos.log` (по файлу на день, хранится 7 дней).
Отладка без железа: `TC001_PORT=<pty из fake_device.py> "/Applications/Dos GatOS.app/Contents/MacOS/dos-gatos"`.

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

Агент скачивает иконку с LaMetric (8×8; анимированные GIF — до 16 кадров с исходными задержками, более длинные прореживаются), кэширует в `~/Library/Application Support/dev.tls1.dosgatos/icons/` и отправляет на часы. Часы хранят её в LittleFS, поэтому в Wi‑Fi‑режиме иконки продолжают показываться. С иконкой текст занимает правые 23 пикселя.

### Шрифты

| шрифт | символов на экране | источник |
|---|---|---|
| `3x5` (по умолчанию) | до 8 | собственный рубленый шрифт проекта, только заглавные, `tools/fonts/blocky3x5.txt` |
| `4x6` | 8 | X11 misc-fixed 4x6 (public domain) |
| `5x7` | 5 | встроенный шрифт Adafruit GFX |

Шрифт по умолчанию выбирается во вкладке Device, для отдельного источника — полем `font`. Знак `°` в `fmt` работает во всех трёх шрифтах. Глифы 3x5 правятся в `tools/fonts/blocky3x5.txt` (5 строк из `#` и `.`), после чего `tools/gen_fonts.py` пересобирает `firmware/src/FontBlocky3x5.h` и `desktop/ui/fonts.js`.

## Экран и кнопки

- Ротация: часы → apps, по 8 с на экран. Длинный текст прокручивается.
- Левая и правая кнопки листают экраны, средняя принудительно обновляет текущий.
- Точка в правом нижнем углу: нет — связь по USB; синяя — работа по Wi‑Fi; красная — нет ни USB, ни Wi‑Fi.
- Серые часы: время взято только из RTC, ни агент, ни NTP его ещё не подтвердили.
- Под часами — полоса дней недели (понедельник первый), сегодняшний день оранжевый.
- Серый текст: данные устарели (старше 3×`every`) или восстановлены из памяти после перезагрузки. Экраны без данных пропускаются.

## Протокол (NDJSON, 460800 бод)

- device → host: `hello{fw,apps,font,wifi,rst,heap,scr}`, `req{id,url,path?,find?,keep?,re?}`, `pong`, `btn`, `log`, `wifi{ssid,state,ip?,rssi?}`, `scr{name}` (какой экран сейчас на часах)
- host → device: `hello?`, `ping` (каждые 3 с), `time{epoch,tz,tzp}`, `resp{id,status,body}`, `notify{text,color,dur,icon?,font?}`, `apps`, `bright`, `icon{id,n,d,px}` (n кадров 8×8 RGB565, d — задержки в мс, px — n×256 hex), `wifi{ssid,pass}`, `settings{font?, clock?{on,dur,pos,h24,wday}, restart?}`; в `apps` у источника `dur` (секунд на экране) и `off` (выключен в плейлисте)

`tzp` — правило часового пояса в формате POSIX (например `CET-1CEST,M3.5.0,M10.5.0/3`), агент берёт его из `/etc/localtime`. По нему часы переводят время на летнее и зимнее в Wi‑Fi‑режиме.

Локальный API агента: `POST /notify`, `/apps`, `/bright`, `/wifi {ssid,pass}`, `/font {font}`, `GET /status`.

## Известные ограничения прототипа

- В Wi‑Fi‑режиме HTTPS-сертификаты не проверяются (в прошивке нет набора корневых сертификатов). Для публичных данных вроде погоды и курсов это приемлемо; секреты в URL источников не кладите.
- Иконки загружаются только через агента: часы, ни разу не подключённые к Mac, показывают источники без иконок.
- С версии 0.4.0 используется таблица разделов `huge_app` (3 МБ под прошивку, без OTA). NVS на прежнем адресе, настройки сохраняются.
- Время хранится в RTC DS1307 (0x68) в UTC. После перезагрузки оно используется, только если на последней сверке с агентом RTC ушёл меньше чем на 60 с (флаг `rtc_ok` в NVS); иначе часы показывают `--:--` до синхронизации. Сверка видна в логе: `grep rtc ~/Library/Logs/DosGatOS/dosgatos.log`. Смещение часового пояса лежит в NVS. Агент подстраивает время при подключении и затем раз в час, поэтому переход на летнее и зимнее время подхватывается в течение часа.
- Шрифты только ASCII.
- Открытие порта может перезагрузить ESP32 через DTR/RTS. Агент ставит обе линии в неактивное состояние; если ресет всё равно случается, часы переподключаются примерно за 1 с.

## Лицензия

MIT, см. `LICENSE`. Сторонние компоненты и условия API перечислены в `THIRD_PARTY_NOTICES.md`.
