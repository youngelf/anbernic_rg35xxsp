# Anbernic RG35XX SP Pomodoro Timer (Knulli Scarab / Batocera & Linux Desktop)

A Pomodoro work timer and work-hours tracker built from the unified `pomodoro.c` source for the **Anbernic RG35XX SP** clamshell handheld running **Knulli (Scarab / Batocera)** (Allwinner H700 `aarch64`, **640×480** IPS display), plus a native **x86_64 Linux (X11)** target for desktop testing.

## Features
- **Native 640×480 Display**: Crisp 2× scaled UI (**640×480** 32-bit ARGB) rendered via Knulli's `libSDL2-2.0.so.0` (with automatic fallback to `/dev/fb0`).
- **Clear Status Header**: Displays `"WORKING: DO NOT DISTURB"` centered in red when in Work mode, plus clear banners for Short Break, Long Break, Paused, and Completed states.
- **Persistent Work Hours Tracking**: Tracks lifetime hours worked (`WORKED: X.XX HRS` and exact `Xh YYm ZZs`), completed Pomodoros, today's hours, and current session time. Auto-saves every 30 seconds while working, on pause/stop/exit, and immediately when closing the RG35XX SP clamshell lid:
  - **On RG35XX SP (Knulli)**: Saves to `/userdata/roms/ports/pomodoro/.pomodoro_stats.cfg` and `/userdata/roms/ports/pomodoro/pomodoro_work_log.txt`.
  - **On Linux Desktop**: Saves to `~/.pomodoro_stats.cfg` and `~/pomodoro_work_log.txt`.
- **Audio & Haptic Completion Alert**: Plays a double-beep completion tone via SDL2 audio and triggers a brief vibration pulse via `/usr/bin/knulli-rumble` when a timer completes.

---

## Controls (Anbernic RG35XX SP & Linux Desktop)

| Action | Anbernic RG35XX SP Button | Linux Desktop Key |
| :--- | :--- | :--- |
| **Toggle Start / Stop (Pause)** | **A Button** | `A`, `Space`, or `Left Alt` |
| **Switch Mode** (`Work` / `Short Break` / `Long Break`) | **X Button** or **D-Pad LEFT / RIGHT** | `X`, `Left Shift`, or `Left` / `Right Arrow` |
| **Reset Current Timer** | **Y Button** | `Y` or `R` |
| **Stop / Pause Timer** | **B Button** | `B` or `Left Ctrl` |
| **Adjust Duration (±1 min while stopped)** | **D-Pad UP / DOWN** or **R1 / L1** | `Up` / `Down Arrow` or `Backspace` / `Tab` |
| **Fast Adjust Duration (±5 min while stopped)** | **R2 / L2** | `Page Down` / `Page Up` |
| **Save & Exit to Knulli** | **SELECT**, **MENU (`M`)**, or **START + SELECT** | `Q`, `Esc`, or close window |

---

## Copying to Your Anbernic RG35XX SP (Knulli SD Card)

Run `make` (or `make package`) to generate the ready-to-copy `sdcard_package/` folder:
```bash
cd pomodoro
make
```

Copy the contents of `sdcard_package/roms/ports/` to the **`roms/ports/`** directory on your Knulli SD card (`SHARE` / `/userdata` partition):
- `sdcard_package/roms/ports/Pomodoro.sh` -> `/userdata/roms/ports/Pomodoro.sh` (or `roms/ports/Pomodoro.sh` on the SD card)
- `sdcard_package/roms/ports/pomodoro/` -> `/userdata/roms/ports/pomodoro/` (or `roms/ports/pomodoro/` on the SD card)

```bash
cp -r sdcard_package/roms/ports/* /path/to/sdcard/roms/ports/
```

Then in Knulli (EmulationStation), press **START -> Game Settings -> Update Games Lists**, open the **Ports** section, and launch **Pomodoro**.

---

## Building & Running on Linux Desktop (`x86_64`)

```bash
cd pomodoro
make linux       # builds ./pomodoro_linux
make run-linux   # builds and opens the 640x480 X11 window
```
