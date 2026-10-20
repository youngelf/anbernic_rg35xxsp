#!/bin/sh
# Anbernic RG35XXSP (Knulli Scarab / Batocera) Port Launcher for Boot Logo Customizer
# Place this script at: /userdata/roms/ports/Boot_Logo.sh
# And the app dir at:   /userdata/roms/ports/boot_logo/

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -x "$SCRIPT_DIR/boot_logo/boot_logo" ]; then
    APP_DIR="$SCRIPT_DIR/boot_logo"
elif [ -x "/userdata/roms/ports/boot_logo/boot_logo" ]; then
    APP_DIR="/userdata/roms/ports/boot_logo"
else
    APP_DIR="$SCRIPT_DIR"
fi

cd "$APP_DIR" || exit 1
chmod +x ./boot_logo 2>/dev/null

exec ./boot_logo
