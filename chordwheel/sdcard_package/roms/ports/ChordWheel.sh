#!/bin/sh
# Anbernic RG35XXSP (Knulli / Batocera) Port Launcher for ChordWheel
# Place this script at: /userdata/roms/ports/ChordWheel.sh
# And the binary at:    /userdata/roms/ports/chordwheel/chordwheel

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -x "$SCRIPT_DIR/chordwheel/chordwheel" ]; then
    APP_DIR="$SCRIPT_DIR/chordwheel"
elif [ -x "/userdata/roms/ports/chordwheel/chordwheel" ]; then
    APP_DIR="/userdata/roms/ports/chordwheel"
else
    APP_DIR="$SCRIPT_DIR"
fi

cd "$APP_DIR" || exit 1
chmod +x ./chordwheel 2>/dev/null

exec ./chordwheel
