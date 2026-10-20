#!/bin/sh
# Anbernic RG35XXSP (Knulli / Batocera / MuOS / Stock OS) Port Launcher for Night Clock
# Place this script at: /userdata/roms/ports/Night_Clock.sh
# And the binary at:    /userdata/roms/ports/night_clock/night_clock

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -x "$SCRIPT_DIR/night_clock/night_clock" ]; then
    APP_DIR="$SCRIPT_DIR/night_clock"
elif [ -x "/userdata/roms/ports/night_clock/night_clock" ]; then
    APP_DIR="/userdata/roms/ports/night_clock"
else
    APP_DIR="$SCRIPT_DIR"
fi

cd "$APP_DIR" || exit 1
chmod +x ./night_clock 2>/dev/null

exec ./night_clock
