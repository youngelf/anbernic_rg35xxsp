# Anbernic RG35XX SP Metronome (Knulli Scarab / Batocera & Linux Desktop)

A precision C metronome built for the **Anbernic RG35XX SP** clamshell handheld running **Knulli (Scarab / Batocera)** (Allwinner H700 `aarch64`, **640×480** IPS display), plus a native **x86_64 Linux (X11)** target for desktop testing.

![Metronome Screenshot](promo/metronome_grab.png)

## Features
- **Native 640×480 Resolution**: Crisp 640×480 display output via Knulli's `libSDL2-2.0.so.0` (with automatic fallback to `/dev/fb0`).
- **Maximum-Loudness Audio**: Full-scale (`±32767` rail-to-rail) saturated harmonic clicks across both stereo channels at 44.1 kHz via SDL2 Audio (with automatic ALSA `libasound.so.2` fallback). Use the RG35XX SP's physical side volume buttons (`VOL+` / `VOL-`) to adjust loudness.
- **5 Time Signatures**: Cycle between **`4/4`**, **`3/4`**, **`2/4`**, **`5/4`**, and **`7/4`** time.
- **Beat 1 Accent**: High-pitched (`1760 Hz`) accented Beat 1 click + red `ACC` beat box flash, and `1000 Hz` normal beat clicks + green beat box flashes, plus a swinging pendulum bar.
- **Persistent Settings**: Automatically saves BPM and time signature to `/userdata/system/.metronome.cfg` on Knulli (or `~/.metronome.cfg` on Linux).

---

## Controls (Anbernic RG35XX SP & Linux Desktop)

| Action | Anbernic RG35XX SP Button | Linux Desktop Key |
| :--- | :--- | :--- |
| **Play / Pause** | **A** | `A`, `Space`, `Enter`, or `Left Alt` |
| **Random Beat Drop (`0%`, `15%`, `30%`, `50%`)** | **B** | `B` or `Left Ctrl` |
| **Increase / Decrease Tempo (±1 BPM, hold for continuous)** | **D-Pad UP** / **DOWN** | `Up Arrow` / `Down Arrow` |
| **Cycle Time Signature (`4/4`, `3/4`, `2/4`, `5/4`, `7/4`)** | **D-Pad LEFT** / **RIGHT** | `Left Arrow` / `Right Arrow` |
| **Fast Tempo Step (±5 BPM)** | **R1 / R2** (+5) and **L1 / L2** (-5) | `Page Up` / `Page Down` or `]` / `[` |
| **Reset to 120 BPM** | **X** | `R` or `X` |
| **Save & Quit to Knulli** | **MENU (`M`)** | `Q` or `ESC` |

---

## Copying to Your Anbernic RG35XX SP (Knulli SD Card)

Run `make` (or `make package`) to generate the ready-to-copy `sdcard_package/` folder:
```bash
cd metronome
make
```

Copy the contents of `sdcard_package/roms/ports/` to the **`roms/ports/`** directory on your Knulli SD card (`SHARE` / `/userdata` partition):
- `sdcard_package/roms/ports/Metronome.sh` -> `/userdata/roms/ports/Metronome.sh` (or `roms/ports/Metronome.sh` on the SD card)
- `sdcard_package/roms/ports/metronome/` -> `/userdata/roms/ports/metronome/` (or `roms/ports/metronome/` on the SD card)

Then in Knulli (EmulationStation), press **START -> Game Settings -> Update Games Lists**, open the **Ports** system, and launch **Metronome**.

---

## Building & Running on Linux Desktop (`x86_64`)

```bash
cd metronome
make linux       # builds ./metronome_linux
make run-linux   # builds and opens the 640x480 X11 window
```
