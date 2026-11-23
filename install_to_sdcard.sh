#!/bin/sh
# Install all 13 Anbernic RG 35xxSP apps, Ports icons, SoundFont, and EmulationStation
# gamelist.xml metadata onto a Knulli / Batocera SD card (preserving existing PortMaster entries).
#
# Usage:
#   ./install_to_sdcard.sh /path/to/SHARE/roms/ports
#   ./install_to_sdcard.sh /path/to/SHARE            # auto-detects /roms/ports
#   ./install_to_sdcard.sh /userdata/roms/ports      # when run directly on the RG 35xxSP

set -eu

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <path-to-roms/ports-or-SHARE-mount>" >&2
    exit 1
fi

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET="$1"

if [ -d "$TARGET/roms/ports" ]; then
    PORTS_DIR="$TARGET/roms/ports"
elif [ "$(basename "$TARGET")" = "ports" ]; then
    PORTS_DIR="$TARGET"
else
    PORTS_DIR="$TARGET/roms/ports"
fi

APPS="bard boot_logo calc chordwheel eartrainer fretboard licktrainer metronome musicplayer night_clock pocketband pomodoro sightread"

mkdir -p "$PORTS_DIR/images" "$PORTS_DIR/soundfonts"

for app in $APPS; do
    pkg="$REPO_DIR/$app/sdcard_package/roms/ports"
    if [ ! -d "$pkg" ]; then
        echo "Error: missing $pkg — run 'make -C $app package' first." >&2
        exit 1
    fi
    cp -r "$pkg"/* "$PORTS_DIR/"
done

if [ -f "$REPO_DIR/soundfonts/GeneralUser-GS.sf2" ]; then
    cp "$REPO_DIR/soundfonts/GeneralUser-GS.sf2" "$PORTS_DIR/soundfonts/GeneralUser-GS.sf2"
fi

# Safely merge ports_gamelist.xml into $PORTS_DIR/gamelist.xml without losing other ports
DEST_XML="$PORTS_DIR/gamelist.xml"
SRC_XML="$REPO_DIR/ports_gamelist.xml"

if [ ! -f "$DEST_XML" ]; then
    cp "$SRC_XML" "$DEST_XML"
    echo "Created $DEST_XML with all 13 app entries."
elif command -v python3 >/dev/null 2>&1; then
    python3 - "$SRC_XML" "$DEST_XML" <<'PYEOF'
import sys
import xml.etree.ElementTree as ET

src_path, dst_path = sys.argv[1], sys.argv[2]

src_tree = ET.parse(src_path)
src_root = src_tree.getroot()

try:
    dst_tree = ET.parse(dst_path)
    dst_root = dst_tree.getroot()
    if dst_root.tag != "gameList":
        raise ValueError("Unexpected root tag")
except Exception:
    dst_root = ET.Element("gameList")
    dst_tree = ET.ElementTree(dst_root)

existing_by_path = {}
for game in list(dst_root.findall("game")):
    p = game.findtext("path", "").strip()
    if p:
        existing_by_path[p] = game

updated = 0
added = 0
for src_game in src_root.findall("game"):
    p = src_game.findtext("path", "").strip()
    if not p:
        continue
    if p in existing_by_path:
        old = existing_by_path[p]
        for tag in ("name", "desc", "image", "thumbnail", "genre", "lang"):
            src_el = src_game.find(tag)
            if src_el is None:
                continue
            dst_el = old.find(tag)
            if dst_el is None:
                dst_el = ET.SubElement(old, tag)
            dst_el.text = src_el.text
        updated += 1
    else:
        dst_root.append(src_game)
        added += 1

if hasattr(ET, "indent"):
    ET.indent(dst_tree, space="\t")
dst_tree.write(dst_path, encoding="utf-8", xml_declaration=True)
print(f"Merged gamelist.xml: {added} added, {updated} updated (existing entries preserved).")
PYEOF
else
    cp "$DEST_XML" "${DEST_XML}.bak"
    cp "$SRC_XML" "$DEST_XML"
    echo "Replaced $DEST_XML (previous file backed up to ${DEST_XML}.bak; install python3 for XML merging)."
fi

echo ""
echo "Installed all 13 RG 35xxSP apps, icons (images/), and gamelist.xml to: $PORTS_DIR"
if command -v pidof >/dev/null 2>&1 && pidof emulationstation >/dev/null 2>&1; then
    echo "NOTE: EmulationStation is currently running! Press START -> Game Settings -> Update Games Lists,"
    echo "or stop ES first (batocera-es-swissknife --emukill) so ES does not overwrite gamelist.xml on exit."
else
    echo "Next step on Knulli: press START -> Game Settings -> Update Games Lists, then open Ports."
fi
