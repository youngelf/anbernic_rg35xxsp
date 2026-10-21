#!/bin/bash
# PortMaster / KNULLI / MuOS / Stock OS launcher for PocketBand on Anbernic RG 35xxSP

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

SHDIR="$(cd "$(dirname "$0")" && pwd)"
if [ -d "$SHDIR/pocketband" ]; then
  GAMEDIR="$SHDIR/pocketband"
else
  GAMEDIR="$SHDIR"
fi

cd "$GAMEDIR" || exit 1
export HOME="${HOME:-$GAMEDIR}"
export POCKETBAND_CFG="$GAMEDIR/.pocketband_rg35xxsp.cfg"
chmod +x ./pocketband
exec ./pocketband
