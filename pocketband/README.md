# Anbernic RG 35xxSP PocketBand — Drum Machine, Bassline & Backing Band (`640x480`)

A **5-track drum machine, walking bassline sequencer, stereo chord-comping backing band, and 12-fret guitar soloing map** built in pure C for the **Anbernic RG 35xxSP** (Allwinner H700 `aarch64`, `640x480` IPS display) and **Linux Desktop (`x86_64` X11)**.

Plug the RG 35xxSP's 3.5mm audio output into your guitar amp's AUX input (or headphones) and jam immediately over **Blues, Rock, and Jazz** backing tracks in **`4/4`** and **`3/4`**.

---

## Screenshots

| Guitarist Jam View (`Blues Shuffle 4/4`, Intervals) | 5-Track Step & Bassline Editor (`Classic Rock 4/4`) |
| :---: | :---: |
| ![Jam View](promo/pocketband_jam.png) | ![Step Editor](promo/pocketband_steps.png) |

| `3/4` Waltz Jam View (`Jazz Waltz 3/4`, Note Names) | Quick Guide & Groove Settings (`Y`) |
| :---: | :---: |
| ![Jazz Waltz View](promo/pocketband_waltz.png) | ![Help Modal](promo/pocketband_help.png) |

---

## Key Features

1. **44.1 kHz Stereo SoundFont 2 + Multi-Track DSP Background Audio Engine**:
   - Loads **`GeneralUser-GS.sf2`** or the bundled **`pocketband.sf2`** automatically via `../rg35xxsp_sf2.h` with stereo Schroeder/Freeverb room reverb for studio drum kits, fingered/upright bass, and Rhodes/jazz guitar comping.
   - Includes a full 5-voice floating-point DSP synthesizer fallback (analog kick, dual-mode snare, 6-oscillator hi-hat/ride bank, resonant SVF electric bass, and 4-voice stereo Rhodes/rhythm guitar comping).
   - Runs 100% of sequencing and audio synthesis on a **dedicated background audio thread** (`audio_thread_worker`) so the `640x480` UI stays at 60 FPS.
2. **Native `640x480` Guitarist Jam View & 5-Track Step Editor (`SELECT`)**:
   - **Live Per-Track VU Meters (`KCK`, `SNR`, `HAT`, `BAS`, `CMP`)**, active chord progression tracker, and groove inspector card.
   - **12-Fret Rosewood Soloing Scale Map** with labeled dots that toggle between **Intervals (`R, b3, 4, b5, 5, b7`)** and **Note Names (`A, C, D, Eb, E, G`)** via **`L2`**.
   - **5-Track Step Editor (`SELECT`)** displaying drum hit accents (`OFF`/`ON`/`ACC`) and transposed bassline notes (`R, 3, 5, 6, b7, 8`) on every step.
3. **Interactive Groove Settings (`Y`) & Live Controls**:
   - Open **`Y`** Help to fine-tune **Swing %** (`LEFT`/`RIGHT`) and toggle **4-Bar Auto-Fill** (`A`), or queue an instant end-of-bar drum fill in Jam View with **`B`**.

---

## Controls

| RG 35xxSP Button | Desktop X11 Key (`pocketband_linux`) | Guitarist Jam View | Step Editor View |
| :--- | :--- | :--- | :--- |
| **`SELECT`** | `Tab` | Switch to **5-Track Step Editor** | Switch back to **Guitarist Jam View** |
| **`A`** | `A`, `Space`, `Z`, or `Alt` | **Play / Stop** backing band | Cycle drum hit (`OFF`/`ON`/`ACC`) or bass degree (`R,3,5,6,b7,8`) |
| **`B`** | `B`, `Backspace`, or `Ctrl` | **Queue Drum Fill** at end of bar | Clear selected step (`OFF`) |
| **`X`** | `X` | **Cycle Backing Mix**: `Full Band` -> `Drums+Bass` -> `Drums Only` | **Cycle Backing Mix** (`Full Band` / `Drums+Bass` / `Drums Only`) |
| **`L1 / R1`** | `L`/`Q`/`[`/`PgUp` and `R`/`E`/`]`/`PgDn` | Adjust **Tempo** (`-5` / `+5` BPM, `45-220 BPM`) | Adjust **Tempo** (`-5` / `+5` BPM) |
| **`L2 / R2`** | `1` / `2` | `L2`: Fretboard **Intervals / Notes** \| `R2`: Cycle **Swing %** | `L2`: Fretboard **Intervals / Notes** \| `R2`: Cycle **Swing %** |
| **`D-Pad LEFT / RIGHT`** | `Left` / `Right Arrow` | Cycle **6 Styles** (`Blues`, `Rock`, `Jazz` in `4/4` & `3/4`) | Move Step Cursor across beats |
| **`D-Pad UP / DOWN`** | `Up` / `Down Arrow` | Transpose **Key** (`C`..`B`) | Select Track (`KICK`, `SNARE`, `CL HAT`, `OP HAT`, `BASS`) |
| **`Y`** | `Y` | Toggle **Quick Guide & Groove** (`L/R`: Swing, `A`: Auto-Fill, `Y`/`B`: Close) | Toggle **Quick Guide & Groove** |
| **`MENU (`M`)`** | `Esc` | Save State & **Exit** | Save State & **Exit** |

---

## Building & Running

```bash
cd pocketband
make all
```
- **RG 35xxSP Binary & SD Card Package**: `sdcard_package/roms/ports/PocketBand.sh` and `sdcard_package/roms/ports/pocketband/` (includes bundled `pocketband.sf2`)
- **Full GM SoundFont (Optional Upgrade)**: Copy `../soundfonts/GeneralUser-GS.sf2` to `/userdata/roms/ports/soundfonts/GeneralUser-GS.sf2` on your SD card.
- **Linux Desktop X11 Simulator**: `./pocketband_linux` (or `make run-linux`)
