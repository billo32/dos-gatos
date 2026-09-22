#!/bin/bash
# Собирает сайт прошивальщика в web/_site:
#   web/build.sh <путь к merged .bin>
# Нужны node/npm (для esp-web-tools). Используется в .github/workflows/pages.yml.
set -euo pipefail
cd "$(dirname "$0")"

BIN="${1:?usage: web/build.sh path/to/tc001-usb-merged.bin}"
EWT_VERSION="10.4.0"
FW_VERSION=$(grep -m1 '#define FW_VERSION' ../firmware/src/main.cpp | sed -E 's/.*"(.*)".*/\1/')
REV=$(git rev-parse --short HEAD 2>/dev/null || echo local)

rm -rf _site && mkdir -p _site/firmware _site/vendor/esp-web-tools
cp index.html _site/
cp ../desktop/src-tauri/icons/128x128.png _site/icon.png
cp "$BIN" _site/firmware/tc001-usb.bin

# ESP Web Tools (Apache-2.0) — локальная копия, без внешнего CDN
TMP=$(mktemp -d)
(cd "$TMP" && npm pack "esp-web-tools@$EWT_VERSION" --silent >/dev/null && tar xzf "esp-web-tools-$EWT_VERSION.tgz")
cp "$TMP"/package/dist/web/*.js _site/vendor/esp-web-tools/
cp "$TMP"/package/LICENSE _site/vendor/esp-web-tools/LICENSE 2>/dev/null || true
rm -rf "$TMP"

cat > _site/manifest.json <<EOF
{
  "name": "dos-gatos",
  "version": "$FW_VERSION ($REV)",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [{ "path": "firmware/tc001-usb.bin", "offset": 0 }]
    }
  ]
}
EOF
touch _site/.nojekyll
echo "site: $(pwd)/_site  (fw $FW_VERSION, $REV)"
