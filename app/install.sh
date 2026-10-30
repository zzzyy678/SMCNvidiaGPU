#!/bin/bash
# Install the menu bar app and start it at login.
#
#   app/install.sh            user-level install (no sudo): ~/Applications + ~/Library/LaunchAgents
#   sudo app/install.sh --system   system-wide install: /Applications + /Library/LaunchAgents
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/build/SMCNvidiaGPUStatus.app"
LABEL=com.nullmoth.smcngpustatus
MODE=user
[ "${1:-}" = "--system" ] && MODE=system

if [ "$MODE" = system ]; then
    [ "$(id -u)" = 0 ] || { echo "STOP: --system needs sudo"; exit 2; }
    DEST=/Applications/SMCNvidiaGPUStatus.app
    PLIST=/Library/LaunchAgents/$LABEL.plist
    CONSOLE_USER=$(stat -f%Su /dev/console)
    CONSOLE_UID=$(id -u "$CONSOLE_USER")
else
    DEST="$HOME/Applications/SMCNvidiaGPUStatus.app"
    PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
    CONSOLE_UID=$(id -u)
    mkdir -p "$HOME/Applications" "$HOME/Library/LaunchAgents"
fi

[ -d "$SRC" ] || { echo "STOP: build first with app/build.sh"; exit 1; }

launchctl bootout "gui/$CONSOLE_UID/$LABEL" 2>/dev/null || true
rm -rf "$DEST"
ditto "$SRC" "$DEST"

cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key><string>$LABEL</string>
	<key>ProgramArguments</key>
	<array><string>/usr/bin/open</string><string>-a</string><string>$DEST</string></array>
	<key>RunAtLoad</key><true/>
	<key>LimitLoadToSessionType</key><string>Aqua</string>
	<key>ProcessType</key><string>Interactive</string>
</dict>
</plist>
PL
chmod 644 "$PLIST"

if [ "$MODE" = system ]; then
    chown -R root:wheel "$DEST"; chown root:wheel "$PLIST"
    launchctl bootstrap "gui/$CONSOLE_UID" "$PLIST" 2>/dev/null || true
    launchctl asuser "$CONSOLE_UID" /usr/bin/open -a "$DEST" 2>/dev/null || open -a "$DEST"
else
    launchctl bootstrap "gui/$CONSOLE_UID" "$PLIST" 2>/dev/null || true
    open -a "$DEST"
fi

echo "installed: $DEST"
echo "start at login: $PLIST"
