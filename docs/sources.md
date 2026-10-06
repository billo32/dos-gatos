# Sources (apps)

An app the clock shows is a *source*: an entry of the app list the Dos GatOS app sends to the clock
(and keeps in `apps.json` on a Mac). In an extension manifest it is the `source` object. The clock
plans the requests itself; over USB or the Wi‑Fi link the app makes them, otherwise the clock does,
with the same rules.

| field | meaning |
|---|---|
| `url` | what to request |
| `every` | how often, in seconds |
| `path` | a path in the JSON response: `current.temperature_2m`, `list.0.price`; `items.length` is the number of elements |
| `find` + `keep` | the `keep` characters after a substring, for responses that aren't JSON |
| `re` | a regular expression over the result: its first group, or the whole match |
| `scale`, `dec` | multiply the number, round to `dec` decimals |
| `fmt` | the text, e.g. `"{v}°"` |
| `color` | `#RRGGBB` |
| `icon` | an icon id from the [LaMetric gallery](https://developer.lametric.com/icons), e.g. `"2422"` |
| `icons` | (0.7.4) the icon by a second value of the same response: `{"path": "current.weather_code", "rules": [[3, "8756"], [48, "2154"], …]}` — the first rule whose number is ≥ the value wins; text rules (`["failed", "5141"]`, 0.8.0) match case-insensitively |
| `font` | `5x7`, `4x6` or `3x5`; without it, the clock's default |
| `headers` | (0.8.0) up to 4 request headers: `{"Authorization": "Bearer …"}` |
| `kind` | (0.8.0) a screen worked out on the clock, without a URL: `"date"` (strftime `format`, e.g. `"%a %d"`) or `"countdown"` (days until `date`, `"2027-01-01"`; `"yearly": true` for anniversaries) |
| `dur`, `off` | seconds on screen; switched off in the rotation |
| `values`, `text`, `rules`, color scales | (0.9.1) see [manifest-v2.md](manifest-v2.md) |
| `frames`, `each`, `item`, `fdur` | (0.9.2) several screens in turn, see [manifest-v2.md](manifest-v2.md) |

Version-2 rules and frames are worked out by the app, which sends the clock the finished text,
color and icon. A clock on its own shows the version-1 result (`path` + `fmt`), so a version-2
source should keep a sensible `path` and `fmt`.

## Example

```json
{
  "name": "weather",
  "url": "https://api.open-meteo.com/v1/forecast?latitude=40.42&longitude=-3.70&current=temperature_2m,weather_code",
  "every": 600,
  "path": "current.temperature_2m",
  "dec": 0,
  "fmt": "{v}°",
  "color": "#FFF1DC",
  "icon": "8756",
  "icons": { "path": "current.weather_code", "rules": [[2, "8756"], [3, "1531"], [48, "2154"], [86, "160"], [99, "2288"]] }
}
```

## Fonts

| font | characters on screen | source |
|---|---|---|
| `3x5` (default) | up to 8 | the project's own blocky font, capitals only, `tools/fonts/blocky3x5.txt` |
| `4x6` | 8 | X11 misc-fixed 4×6 (public domain) |
| `5x7` | 5 | Adafruit GFX's built-in font |

`°` works in all three. The 3×5 glyphs are edited in `tools/fonts/blocky3x5.txt` (5 rows of `#` and
`.`); `tools/gen_fonts.py` then rebuilds `src/FontBlocky3x5.h`:

```bash
python3 tools/gen_fonts.py .pio/libdeps/tc001/Adafruit\ GFX\ Library
```
