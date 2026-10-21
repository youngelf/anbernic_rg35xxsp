#!/bin/sh
# PortMaster / KNULLI / MuOS / Stock Launcher for RG 35xxSP EarTrainer
PROGDIR="$(dirname "$(readlink -f "$0")")/eartrainer"
if [ ! -d "$PROGDIR" ]; then
    PROGDIR="$(dirname "$(readlink -f "$0")")"
fi
cd "$PROGDIR" || exit 1
export HOME="${HOME:-/userdata/system}"
exec ./eartrainer
