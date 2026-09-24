#!/bin/bash
# Подписанная и нотаризованная сборка Dos GatOS (Developer ID, вне App Store).
#
#   cp signing.env.example signing.env   # заполнить один раз
#   npm run release
#
# Результат: src-tauri/target/release/bundle/{macos/Dos GatOS.app, dmg/*.dmg}
set -euo pipefail
cd "$(dirname "$0")/.."

[[ -f signing.env ]] && set -a && source signing.env && set +a

# Сертификат «Developer ID Application» из связки ключей, если не задан явно
if [[ -z "${APPLE_SIGNING_IDENTITY:-}" ]]; then
  APPLE_SIGNING_IDENTITY=$(security find-identity -v -p codesigning | grep -m1 '"Developer ID Application' | sed -E 's/.*"(.*)"/\1/' || true)
fi
if [[ -z "$APPLE_SIGNING_IDENTITY" ]]; then
  echo "Нет сертификата «Developer ID Application» в связке ключей." >&2
  echo "Xcode → Settings → Accounts → Manage Certificates → + → Developer ID Application" >&2
  exit 1
fi
export APPLE_SIGNING_IDENTITY
echo "Подпись: $APPLE_SIGNING_IDENTITY"

notarize_args=()
if [[ "${SKIP_NOTARIZE:-0}" == 1 ]]; then
  # release.sh нотаризует dmg и app.zip сам (scripts/notarize-release.sh)
  unset APPLE_API_KEY APPLE_API_ISSUER APPLE_API_KEY_PATH APPLE_ID APPLE_PASSWORD APPLE_TEAM_ID
  echo "Нотаризация — отдельным шагом после сборки."
elif [[ -n "${APPLE_NOTARY_PROFILE:-}" ]]; then
  notarize_args=(--keychain-profile "$APPLE_NOTARY_PROFILE")
elif [[ -n "${APPLE_API_KEY:-}" ]]; then
  : "${APPLE_API_ISSUER:?нужен APPLE_API_ISSUER}" "${APPLE_API_KEY_PATH:?нужен APPLE_API_KEY_PATH}"
  export APPLE_API_KEY APPLE_API_ISSUER APPLE_API_KEY_PATH
  notarize_args=(--key "$APPLE_API_KEY_PATH" --key-id "$APPLE_API_KEY" --issuer "$APPLE_API_ISSUER")
elif [[ -n "${APPLE_ID:-}" ]]; then
  : "${APPLE_PASSWORD:?нужен APPLE_PASSWORD (app-specific)}" "${APPLE_TEAM_ID:?нужен APPLE_TEAM_ID}"
  export APPLE_ID APPLE_PASSWORD APPLE_TEAM_ID
  notarize_args=(--apple-id "$APPLE_ID" --password "$APPLE_PASSWORD" --team-id "$APPLE_TEAM_ID")
else
  echo "⚠ Данные для нотаризации не заданы — будет только подпись (Gatekeeper на других Mac будет ругаться)."
fi

# Tauri сам подписывает .app (hardened runtime) и нотаризует его, если заданы переменные выше
npx tauri build --bundles app,dmg "$@"

BUNDLE=src-tauri/target/release/bundle
for t in universal-apple-darwin aarch64-apple-darwin x86_64-apple-darwin; do
  if [[ " $* " == *" $t"* ]]; then BUNDLE="src-tauri/target/$t/release/bundle"; fi
done
APP="$BUNDLE/macos/Dos GatOS.app"
DMG=$(ls -t "$BUNDLE"/dmg/*.dmg | head -1)

codesign --verify --deep --strict --verbose=2 "$APP"

# DMG нотаризуем отдельно, чтобы он открывался без предупреждений
if (( ${#notarize_args[@]} )); then
  codesign --force --timestamp --sign "$APPLE_SIGNING_IDENTITY" "$DMG"
  xcrun notarytool submit "$DMG" "${notarize_args[@]}" --wait
  xcrun stapler staple "$DMG"
  spctl -a -vv -t install "$DMG"
fi
spctl -a -vv -t exec "$APP" || true

echo
echo "Готово:"
echo "  $APP"
echo "  $DMG"
