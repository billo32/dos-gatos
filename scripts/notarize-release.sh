#!/bin/bash
# Нотаризация уже собранного релиза (dmg + app.zip из dist/<tag>) и замена файлов в GitHub Release.
#   scripts/notarize-release.sh v0.3.2              # нотаризовать и загрузить в релиз
#   scripts/notarize-release.sh v0.3.2 --no-upload  # только нотаризовать (вызывается из release.sh)
# Данные для нотаризации — в desktop/signing.env (см. signing.env.example).
set -euo pipefail
TAG="${1:?usage: scripts/notarize-release.sh vX.Y.Z [--no-upload]}"
UPLOAD=1; [[ "${2:-}" == "--no-upload" ]] && UPLOAD=0
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
DIST="$ROOT/dist/$TAG"
[[ -d "$DIST" ]] || { echo "нет $DIST — сначала собери релиз" >&2; exit 1; }
if [[ -f desktop/signing.env ]]; then set -a; source desktop/signing.env; set +a; fi

if [[ -n "${APPLE_NOTARY_PROFILE:-}" ]]; then NA=(--keychain-profile "$APPLE_NOTARY_PROFILE")
elif [[ -n "${APPLE_API_KEY:-}" ]]; then NA=(--key "$APPLE_API_KEY_PATH" --key-id "$APPLE_API_KEY" --issuer "$APPLE_API_ISSUER")
elif [[ -n "${APPLE_ID:-}" ]]; then NA=(--apple-id "$APPLE_ID" --password "$APPLE_PASSWORD" --team-id "$APPLE_TEAM_ID")
else echo "нет данных для нотаризации — заполни desktop/signing.env (см. signing.env.example)" >&2; exit 1; fi

ID="${APPLE_SIGNING_IDENTITY:-$(security find-identity -v -p codesigning | grep -m1 '"Developer ID Application' | sed -E 's/.*"(.*)"/\1/')}"
[[ -n "$ID" ]] || { echo "нет сертификата Developer ID Application" >&2; exit 1; }
DMG=$(ls "$DIST"/*.dmg | head -1)
ZIP=$(ls "$DIST"/*.app.zip | head -1)

notarize() {
  echo "== notarytool: $(basename "$1") (обычно 1–10 минут)"
  local out
  out=$(xcrun notarytool submit "$1" "${NA[@]}" --wait 2>&1) || true
  echo "$out"
  if ! grep -q "status: Accepted" <<<"$out"; then
    local id; id=$(grep -m1 -oE 'id: [0-9a-f-]{36}' <<<"$out" | cut -d' ' -f2)
    echo "✗ Apple не приняла $(basename "$1")${id:+. Подробности: xcrun notarytool log $id ${NA[*]}}" >&2
    exit 1
  fi
}

# .app: нотаризация zip → тикет прикрепляется к самому .app → zip пересобирается
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
ditto -x -k "$ZIP" "$WORK"
APP=$(ls -d "$WORK"/*.app | head -1)
codesign --verify --deep --strict "$APP"
notarize "$ZIP"
xcrun stapler staple "$APP"
rm "$ZIP"; ditto -c -k --keepParent "$APP" "$ZIP"

# .dmg: подпись → нотаризация → тикет
codesign --force --timestamp --sign "$ID" "$DMG"
notarize "$DMG"
xcrun stapler staple "$DMG"

echo "== Gatekeeper"
spctl -a -vv -t exec "$APP"
spctl -a -vv -t install "$DMG"

(cd "$DIST" && shasum -a 256 *.dmg *.zip *.bin > SHA256SUMS.txt)

if ((UPLOAD)); then
  gh release upload "$TAG" "$DMG" "$ZIP" "$DIST/SHA256SUMS.txt" --clobber
  NOTES=$(gh release view "$TAG" --json body -q .body | sed 's/^Подписан Developer ID\.$/Подписан Developer ID и нотаризован Apple./')
  gh release edit "$TAG" --notes "$NOTES" >/dev/null
  echo "✓ $TAG: файлы в релизе заменены на нотаризованные"
fi
