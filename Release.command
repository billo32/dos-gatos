#!/bin/bash
# Двойной клик в Finder: собрать TC001 Agent и прошивку, создать тег и GitHub Release.
cd "$(dirname "$0")" || exit 1
pause() { echo; read -r -p "Enter — закрыть окно" _; }
trap pause EXIT

echo "dos-gatos — релиз из $(pwd)"
echo

# gh нужен для создания релиза (репозиторий приватный — скачивание тоже через GitHub-аккаунт)
if ! command -v gh >/dev/null; then
  if command -v brew >/dev/null; then
    read -r -p "Нет gh (GitHub CLI). Установить через brew? [Y/n] " a
    [[ "$a" =~ ^[Nn] ]] || brew install gh
  fi
fi
if command -v gh >/dev/null && ! gh auth status >/dev/null 2>&1; then
  echo "Нужен вход в GitHub (один раз):"
  gh auth login --web --git-protocol https || exit 1
fi

# незакоммиченные изменения
if [[ -n "$(git status --porcelain)" ]]; then
  echo "Незакоммиченные изменения:"
  git status --short
  read -r -p "Закоммитить их перед релизом? [y/N] " a
  if [[ "$a" =~ ^[Yy] ]]; then
    git add -A && git commit -m "Prepare release" || exit 1
  else
    echo "Отменено."; exit 1
  fi
fi

LAST=$(git describe --tags --abbrev=0 2>/dev/null || echo "нет")
read -r -p "Версия релиза (последний тег: $LAST), например 0.3.2: " V
[[ -n "$V" ]] || exit 1

# Для 0.3.2 — уже проверенный на часах образ, если PlatformIO не настроен
FW="$HOME/Projects/tc001-usb/firmware/bin/tc001-usb-0.3.2-merged.bin"
if [[ "$V" == "0.3.2" && -f "$FW" && -z "${FIRMWARE_BIN:-}" ]] && ! command -v pio >/dev/null && [[ ! -x "$HOME/.platformio/penv/bin/pio" ]]; then
  export FIRMWARE_BIN="$FW"
fi

./scripts/release.sh "$V"
