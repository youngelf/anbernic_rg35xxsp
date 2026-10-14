#!/bin/bash
# Knulli (Scarab / Batocera) Port Launcher for Anbernic RG35XX SP Metronome

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="${SCRIPT_DIR}/metronome"

if [ ! -d "${APP_DIR}" ] && [ -d "/userdata/roms/ports/metronome" ]; then
    APP_DIR="/userdata/roms/ports/metronome"
elif [ ! -d "${APP_DIR}" ] && [ -d "/roms/ports/metronome" ]; then
    APP_DIR="/roms/ports/metronome"
fi

cd "${APP_DIR}" || exit 1
chmod +x ./metronome 2>/dev/null

exec ./metronome
