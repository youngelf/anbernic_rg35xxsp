#!/bin/sh
# PortMaster / KNULLI / MuOS / Stock Launcher for RG 35xxSP Fretboard
PROGDIR="$(dirname "$(readlink -f "$0")")/fretboard"
if [ ! -d "$PROGDIR" ]; then
    PROGDIR="$(dirname "$(readlink -f "$0")")"
fi
cd "$PROGDIR" || exit 1
export HOME="${HOME:-/userdata/system}"
exec ./fretboard
