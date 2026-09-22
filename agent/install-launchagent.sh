#!/bin/bash
# Автозапуск агента при входе в систему (LaunchAgent) + перезапуск при падении.
#   ./agent/install-launchagent.sh            установить или обновить
#   ./agent/install-launchagent.sh uninstall  удалить
set -euo pipefail

LABEL="dev.tls1.tc001-agent"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/tc001-agent.log"
AGENT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$AGENT_DIR")"
PY="$ROOT/.venv/bin/python"
DOMAIN="gui/$(id -u)"

if [[ "${1:-}" == "uninstall" ]]; then
  launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
  rm -f "$PLIST"
  echo "removed $LABEL"
  exit 0
fi

if [[ ! -x "$PY" ]]; then
  echo "нет $PY — сначала: python3 -m venv .venv && .venv/bin/pip install -r agent/requirements.txt" >&2
  exit 1
fi
"$PY" -c "import serial, certifi" 2>/dev/null || "$PY" -m pip install -q -r "$AGENT_DIR/requirements.txt"

if lsof -nP -iTCP:7765 -sTCP:LISTEN >/dev/null 2>&1 && ! launchctl print "$DOMAIN/$LABEL" >/dev/null 2>&1; then
  echo "порт 7765 занят — останови агент, запущенный вручную (Ctrl+C), и повтори" >&2
  exit 1
fi

mkdir -p "$(dirname "$PLIST")" "$(dirname "$LOG")"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key>
  <array>
    <string>$PY</string>
    <string>-u</string>
    <string>$AGENT_DIR/agent.py</string>
  </array>
  <key>WorkingDirectory</key><string>$AGENT_DIR</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>10</integer>
  <key>ProcessType</key><string>Background</string>
  <key>StandardOutPath</key><string>$LOG</string>
  <key>StandardErrorPath</key><string>$LOG</string>
</dict>
</plist>
EOF

launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
launchctl bootstrap "$DOMAIN" "$PLIST"
sleep 2
if launchctl print "$DOMAIN/$LABEL" | grep -q "state = running"; then
  echo "running: $LABEL"
else
  echo "не запустился, см. лог: $LOG" >&2
fi
echo "лог:        tail -f $LOG"
echo "перезапуск: launchctl kickstart -k $DOMAIN/$LABEL"
