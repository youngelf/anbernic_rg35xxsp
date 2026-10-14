#!/bin/bash
# Knulli (Scarab / Batocera) Port Launcher for Anbernic RG35XX SP Pomodoro Timer

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="${SCRIPT_DIR}/pomodoro"

if [ ! -d "${APP_DIR}" ] && [ -d "/userdata/roms/ports/pomodoro" ]; then
    APP_DIR="/userdata/roms/ports/pomodoro"
elif [ ! -d "${APP_DIR}" ] && [ -d "/roms/ports/pomodoro" ]; then
    APP_DIR="/roms/ports/pomodoro"
fi

# Ensure 640x480 32bpp framebuffer configuration if fbset is available
if command -v fbset >/dev/null 2>&1; then
    fbset -g 640 480 640 960 32 2>/dev/null || true
fi

cd "${APP_DIR}" || exit 1
chmod +x ./pomodoro 2>/dev/null

exec ./pomodoro
