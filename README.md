# dos-gatos

Альтернативная прошивка для пиксельных часов Ulanzi TC001 и агент для macOS, который связывается с часами по USB.

Часы Ulanzi TC001 сами решают, **что** и **когда** запрашивать (список apps хранится в NVS).
HTTP-запросы выполняет агент на Mac, связь идёт по USB-serial (CH340), WiFi не используется.

```
[TC001: планировщик apps] --req{url,path|find,re}--> USB 460800 --> [agent.py на Mac] --HTTPS--> API
                        <--resp{status, body: "24.6"}--                                 <--
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

Извлечение делает агент, поэтому на часы приходит только готовое значение.

## Экран и кнопки

- Ротация: часы → apps, по 8 с на экран. Длинный текст прокручивается.
- Левая и правая кнопки листают экраны, средняя принудительно обновляет текущий.
- Красная точка в правом нижнем углу: связи с агентом нет больше 10 с.
- Серые часы: время взято только из RTC, агент его ещё не подтвердил.
- Серый текст: данные устарели (старше 3×`every`) или восстановлены из памяти после перезагрузки. Экраны без данных пропускаются.

## Протокол (NDJSON, 460800 бод)

- device → host: `hello`, `req{id,url,path?,find?,keep?,re?}`, `pong`, `btn`, `log`
- host → device: `hello?`, `ping` (каждые 3 с), `time{epoch,tz}`, `resp{id,status,body}`, `notify`, `apps`, `bright`

## Известные ограничения прототипа

- Если Mac спит, данные не обновляются: часы показывают последние значения серым. Фолбэка на WiFi пока нет.
- Время хранится в RTC DS1307 (0x68) в UTC. После перезагрузки оно используется, только если на последней сверке с агентом RTC ушёл меньше чем на 60 с (флаг `rtc_ok` в NVS); иначе часы показывают `--:--` до синхронизации. Сверка видна в логе: `grep rtc ~/Library/Logs/tc001-agent.log`. Смещение часового пояса лежит в NVS. Агент подстраивает время при подключении и затем раз в час, поэтому переход на летнее и зимнее время подхватывается в течение часа.
- Шрифт 5×7 из Adafruit GFX, только ASCII.

## Лицензия

MIT, см. `LICENSE`. Сторонние компоненты и условия API перечислены в `THIRD_PARTY_NOTICES.md`.
- Открытие порта может перезагрузить ESP32 через DTR/RTS. Агент ставит обе линии в неактивное состояние; если ресет всё равно случается, часы переподключаются примерно за 1 с.
