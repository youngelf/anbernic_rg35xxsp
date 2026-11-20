#!/bin/sh
# Anbernic RG35XXSP (Knulli / Batocera / MuOS / Stock OS) Port Launcher for Night Clock
# Place this script at: /userdata/roms/ports/Night_Clock.sh
# And the binary at:    /userdata/roms/ports/night_clock/night_clock

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if [ -x "$SCRIPT_DIR/night_clock/night_clock" ]; then
    APP_DIR="$SCRIPT_DIR/night_clock"
elif [ -x "/userdata/roms/ports/night_clock/night_clock" ]; then
    APP_DIR="/userdata/roms/ports/night_clock"
else
    APP_DIR="$SCRIPT_DIR"
fi

# Prevent Knulli idlewatcher / battery-saver from blanking screen or muting alarm audio
PAUSE_DIR="/var/run/battery-saver"
PAUSE_FILE="${PAUSE_DIR}/night_clock.pause"
LEGACY_PAUSE="/var/run/battery_saver.pause"
CREATED_LEGACY=0
STOPPED_FIREFLY_SAVER=0

cleanup_keepawake() {
    rm -f "$PAUSE_FILE" 2>/dev/null || true
    if [ "$CREATED_LEGACY" -eq 1 ]; then
        CREATED_LEGACY=0
        rm -f "$LEGACY_PAUSE" 2>/dev/null || true
    fi
    if [ "$STOPPED_FIREFLY_SAVER" -eq 1 ]; then
        STOPPED_FIREFLY_SAVER=0
        if [ -x /etc/init.d/S96battery-saver-daemon ]; then
            /etc/init.d/S96battery-saver-daemon start >/dev/null 2>&1 || true
        fi
    fi
}
trap cleanup_keepawake EXIT INT TERM HUP

if [ -w /var/run ] || [ -w "$PAUSE_DIR" ]; then
    mkdir -p "$PAUSE_DIR" 2>/dev/null || true
    touch "$PAUSE_FILE" 2>/dev/null || true
    if [ ! -e "$LEGACY_PAUSE" ]; then
        touch "$LEGACY_PAUSE" 2>/dev/null && CREATED_LEGACY=1 || true
    fi
    if [ -f /usr/bin/battery-saver.sh ] && ! grep -q "pause" /usr/bin/battery-saver.sh 2>/dev/null; then
        if [ -x /etc/init.d/S96battery-saver-daemon ] && [ -f /var/run/battery-saver.pid ]; then
            /etc/init.d/S96battery-saver-daemon stop >/dev/null 2>&1 && STOPPED_FIREFLY_SAVER=1 || true
        fi
    fi
fi

if command -v setterm >/dev/null 2>&1; then
    setterm -blank 0 -powersave off </dev/tty0 >/dev/tty0 2>/dev/null || true
fi

cd "$APP_DIR" || exit 1
chmod +x ./night_clock 2>/dev/null || true

./night_clock "$@"
exit $?
