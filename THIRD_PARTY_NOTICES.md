# Third-party components

Dos GatOS's own code is licensed under the PolyForm Noncommercial License 1.0.0 (see `LICENSE`).
The firmware image is built with the components below, which keep their own licenses — the
noncommercial terms don't apply to them.

| Component | License | Notes |
|---|---|---|
| [arduino-esp32](https://github.com/espressif/arduino-esp32) 2.0.x / ESP-IDF | LGPL-2.1 + Apache-2.0 | Arduino core and SDK |
| [FastLED](https://github.com/FastLED/FastLED) | MIT | |
| [FastLED NeoMatrix](https://github.com/marcmerlin/FastLED_NeoMatrix) | LGPL-3.0-or-later | a port of Adafruit_NeoMatrix |
| [Framebuffer GFX](https://github.com/marcmerlin/Framebuffer_GFX) | LGPL-3.0-or-later | |
| [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) | BSD-2-Clause | including the 5×7 font |
| [Adafruit BusIO](https://github.com/adafruit/Adafruit_BusIO) | MIT | |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | MIT | |
| X11 misc-fixed 4×6 (`src/Font4x6.h`, `tools/fonts/4x6.bdf`) | Public domain | "Public domain font. Share and enjoy." |

**LGPL components.** The complete firmware source is in this repository and the release images are
rebuilt with `pio run`, so you can relink the firmware with modified versions of the LGPL libraries
(change their versions in `platformio.ini`).

**Trademarks.** Ulanzi and TC001 are trademarks of their respective owners. This is an independent
project, not affiliated with Ulanzi. No code from [AWTRIX](https://github.com/Blueforcer/awtrix-ng) is used.
