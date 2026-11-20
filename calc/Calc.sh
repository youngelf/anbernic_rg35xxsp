#!/bin/sh
# Anbernic RG35XXSP (Knulli / Batocera / MuOS / PortMaster) Port Launcher for Calc
# Place this script at: /userdata/roms/ports/Calc.sh
# And the binary at:    /userdata/roms/ports/calc/calc

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -x "$SCRIPT_DIR/calc/calc" ]; then
    APP_DIR="$SCRIPT_DIR/calc"
elif [ -x "/userdata/roms/ports/calc/calc" ]; then
    APP_DIR="/userdata/roms/ports/calc"
else
    APP_DIR="$SCRIPT_DIR"
fi

cd "$APP_DIR" || exit 1
chmod +x ./calc 2>/dev/null

exec ./calc
