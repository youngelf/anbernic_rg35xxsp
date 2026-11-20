#!/bin/sh
# Anbernic RG35XXSP (Knulli / Batocera / MuOS / PortMaster) Port Launcher for Bard
# Place this script at: /userdata/roms/ports/Bard.sh
# And the binary at:    /userdata/roms/ports/bard/bard

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -x "$SCRIPT_DIR/bard/bard" ]; then
    APP_DIR="$SCRIPT_DIR/bard"
elif [ -x "/userdata/roms/ports/bard/bard" ]; then
    APP_DIR="/userdata/roms/ports/bard"
else
    APP_DIR="$SCRIPT_DIR"
fi

cd "$APP_DIR" || exit 1
export BARD_CFG="$APP_DIR/.bard_rg35xxsp.cfg"
chmod +x ./bard 2>/dev/null

exec ./bard
