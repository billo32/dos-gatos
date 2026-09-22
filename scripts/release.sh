#!/bin/bash
# Локальный релиз с Mac: версия → сборка приложения (universal) и прошивки → тег → GitHub Release.
#
#   scripts/release.sh 0.3.2
#
# Подпись и нотаризация — если есть сертификат Developer ID (и desktop/signing.env),
# иначе собирается неподписанная сборка с предупреждением.
# Прошивка: PlatformIO; либо готовый образ через FIRMWARE_BIN=/путь/merged.bin.
# Релиз создаётся через gh (brew install gh && gh auth login); без gh — инструкция для ручной загрузки.
set -euo pipefail

VERSION="${1:?usage: scripts/release.sh X.Y.Z}"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "версия в формате X.Y.Z" >&2; exit 1; }
TAG="v$VERSION"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DIST="$ROOT/dist/$TAG"

step() { printf '\n\033[1;33m== %s\033[0m\n' "$*"; }

# ---------- проверки ----------
[[ "$(uname)" == "Darwin" ]] || { echo "запускать на macOS" >&2; exit 1; }
[[ -z "$(git status --porcelain)" ]] || { echo "есть незакоммиченные изменения — сначала commit" >&2; git status --short; exit 1; }
git rev-parse "$TAG" >/dev/null 2>&1 && { echo "тег $TAG уже есть" >&2; exit 1; }
HAVE_GH=0
if command -v gh >/dev/null && gh auth status >/dev/null 2>&1; then HAVE_GH=1; fi
rm -rf "$DIST" && mkdir -p "$DIST"

# ---------- версия ----------
step "Версия $VERSION"
sed -i '' -E "s/^version = \"[^\"]+\"/version = \"$VERSION\"/" desktop/src-tauri/Cargo.toml
sed -i '' -E "s/\"version\": \"[^\"]+\"/\"version\": \"$VERSION\"/" desktop/src-tauri/tauri.conf.json
sed -i '' -E "1,/\"version\":/ s/\"version\": \"[^\"]+\"/\"version\": \"$VERSION\"/" desktop/package.json
sed -i '' -E "s/#define FW_VERSION   \"[^\"]+\"/#define FW_VERSION   \"$VERSION\"/" firmware/src/main.cpp
grep -m1 FW_VERSION firmware/src/main.cpp

# ---------- прошивка ----------
step "Прошивка"
FW_OUT="$DIST/tc001-usb-$TAG-merged.bin"
if [[ -n "${FIRMWARE_BIN:-}" ]]; then
  echo "⚠ используется готовый образ $FIRMWARE_BIN — версия в нём может не совпадать с $VERSION"
  cp "$FIRMWARE_BIN" "$FW_OUT"
else
  PIO=$(command -v pio || echo "$HOME/.platformio/penv/bin/pio")
  [[ -x "$PIO" ]] || { echo "нет PlatformIO: pip install platformio, или FIRMWARE_BIN=..." >&2; exit 1; }
  "$PIO" run -d firmware
  B=firmware/.pio/build/tc001
  BOOT_APP0=$(find "$HOME/.platformio/packages" -path '*framework-arduinoespressif32*' -name boot_app0.bin | head -1)
  ESPTOOL=(python3 "$HOME/.platformio/packages/tool-esptoolpy/esptool.py")
  command -v esptool >/dev/null && ESPTOOL=(esptool)
  "${ESPTOOL[@]}" --chip esp32 merge_bin -o "$FW_OUT" \
    0x1000 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$B/firmware.bin"
fi

# ---------- приложение ----------
step "TC001 Agent (universal)"
rustup target add aarch64-apple-darwin x86_64-apple-darwin >/dev/null
cd desktop
npm ci --silent
SIGNED=0
if [[ -n "${APPLE_SIGNING_IDENTITY:-}" ]] || security find-identity -v -p codesigning | grep -q '"Developer ID Application'; then
  ./scripts/build-signed.sh --target universal-apple-darwin
  SIGNED=1
else
  echo "⚠ Нет сертификата Developer ID — сборка без подписи (скачанная с GitHub потребует «Всё равно открыть» в настройках безопасности)."
  npx tauri build --bundles app,dmg --target universal-apple-darwin
fi
BUNDLE=src-tauri/target/universal-apple-darwin/release/bundle
DMG=$(ls -t "$BUNDLE"/dmg/*.dmg | head -1)
cp "$DMG" "$DIST/TC001-Agent-$VERSION-universal.dmg"
(cd "$BUNDLE/macos" && ditto -c -k --keepParent "TC001 Agent.app" "$DIST/TC001-Agent-$VERSION-universal.app.zip")
cd "$ROOT"

(cd "$DIST" && shasum -a 256 * > SHA256SUMS.txt)
ls -lh "$DIST"

# ---------- git ----------
step "Коммит и тег $TAG"
git add desktop/src-tauri/Cargo.toml desktop/src-tauri/Cargo.lock desktop/src-tauri/tauri.conf.json desktop/package.json desktop/package-lock.json firmware/src/main.cpp
git commit -m "Release $TAG" || true
git tag -a "$TAG" -m "dos-gatos $TAG"
git push origin HEAD "$TAG"

# ---------- релиз ----------
NOTES="$DIST/NOTES.md"
cat > "$NOTES" <<EOF
## dos-gatos $TAG

**Прошивка:** \`tc001-usb-$TAG-merged.bin\` — через [веб-прошивальщик](https://billo32.github.io/dos-gatos/) или
\`esptool.py --chip esp32 --port /dev/cu.usbserial-XXXX --baud 460800 write_flash 0x0 tc001-usb-$TAG-merged.bin\`

**TC001 Agent для macOS** (Apple Silicon + Intel): \`TC001-Agent-$VERSION-universal.dmg\`
$( ((SIGNED)) && echo "Подписан Developer ID." || echo "Без подписи: после первой попытки запуска — Системные настройки → Конфиденциальность и безопасность → «Всё равно открыть», или \`xattr -dr com.apple.quarantine \"/Applications/TC001 Agent.app\"\`." )

Перед прошивкой сохрани текущую: \`esptool.py --chip esp32 --port … read_flash 0 0x400000 backup.bin\`
EOF

if ((HAVE_GH)); then
  step "GitHub Release"
  gh release create "$TAG" "$DIST"/*.dmg "$DIST"/*.zip "$DIST"/*.bin "$DIST/SHA256SUMS.txt" \
    --title "dos-gatos $TAG" --notes-file "$NOTES"
  gh release view "$TAG" --web >/dev/null 2>&1 || true
else
  step "Релиз вручную"
  echo "gh не настроен. Открой https://github.com/billo32/dos-gatos/releases/new?tag=$TAG"
  echo "и перетащи файлы из $DIST (описание — $NOTES)."
  open "$DIST"
fi
