# Surgical Boot Logo Customizer (`boot_logo`) — Anbernic RG 35xxSP

A native `640x480` on-device utility and desktop simulator for the **Anbernic RG 35xxSP** (running Knulli Scarab / Batocera) that surgically updates the hardware U-Boot / kernel splash screen (`/boot/bootlogo.bmp` on Partition 3 `boot-resource`) while preserving the original stock image for instant 1-click restoration.

![Theme 2: GBA Pixel Kitchen](promo/shot_theme2_gba_kitchen.png)

---

## Included 640x480 Themes (`1,228,938` Bytes, 32-Bit `BITMAPV5HEADER` BGRA)

Every `.bmp` file in `logos/` and `backup/` is formatted to match Knulli Scarab's exact `BOOTLOGO.BMP` specification (`138`-byte `BITMAPV5HEADER` + `640x480x4` bottom-up BGRA pixel array = `1,228,938` bytes):

1. **Theme 1 (`logos/theme1.bmp`) — Starcrossed Kiss**: Soulful illustration of a girl blowing a glowing heart kiss from the bottom left (adapted from the bottom screen of Theme 1 on the RG DS Plus, with no boy in the top right).
2. **Theme 2 (`logos/theme2.bmp`) — GBA Pixel Kitchen**: Chef Wario and Sous-Chef Tux whimsically cooking up a storm of glowing 8-bit/16-bit pixels, stars, and gold coins on a clamshell GameBoy Advance SP skillet.
3. **Theme 3 (`logos/theme3.bmp`) — Tux Starry Sky**: Tux standing on a glowing twilight wildflower cliff looking up at the night sky (adapted from the bottom screen of Theme 3 on the RG DS Plus).
4. **Theme 4 (`logos/theme4.bmp`) — Midnight Poetry**: Minimalist sleek illustration of a girl writing poetry under a warm desk lamp (adapted from the bottom screen of Theme 4 on the RG DS Plus).
5. **Theme 5 (`logos/stock.bmp` & `backup/bootlogo.stock.bmp`) — Knulli Scarab Stock**: Exact bit-for-bit factory Knulli Scarab `bootlogo.bmp` (`SHA-256: de400d257de3be2ed4417a09c9b4ab5077bf99c3c5560e49792bf7db831a5b88`) so you can revert to stock at any time.

---

## How Surgical Flashing Works (Option A)

1. **Stock Backup**:
   - Bundles the factory Knulli Scarab `bootlogo.bmp` in `backup/bootlogo.stock.bmp` and `logos/stock.bmp`.
   - Before modifying `/boot/bootlogo.bmp` for the first time, backs up the live `/boot/bootlogo.bmp` to `backup/bootlogo.device_orig.bmp` and `/boot/bootlogo.stock.bmp`.
2. **Surgical Write & SHA-256 Read-Back Verification**:
   - Remounts `/boot` read-write (`mount -o remount,rw /boot`).
   - Writes the `1,228,938`-byte `.bmp` to `/boot/bootlogo.bmp` and flushes via `fsync()` + `sync()`.
   - Reads `/boot/bootlogo.bmp` back from disk and verifies its SHA-256 hash matches the source `.bmp`.
   - Remounts `/boot` read-only (`mount -o remount,ro /boot`) to protect the FAT32 partition.

---

## Controls (Hardware & Desktop Linux X11)

| Button / Key | Action |
| :--- | :--- |
| **D-Pad Up / Down / Left / Right** (or Arrow Keys) | Cycle through the 5 boot logo themes |
| **Button A** (or `A` / `Space` / `Z`) | Open **Flash Boot Logo** confirmation modal; press **A** again to flash & verify |
| **Button B** (or `B`) | Open **Revert to Stock** confirmation modal; press **A** to restore factory `bootlogo.bmp` (or cancel modal) |
| **Button SELECT** (or `Tab` / `Backspace`) | Toggle unobstructed **1:1 Full-Screen `640x480` View** (hides top/bottom HUD bars) |
| **Button Y** (or `Y` / `H`) | Toggle **Controls Help** overlay; press **Y** or **B** to resume |
| **MENU** (or `Esc` / `Q` on Desktop) | Exit Boot Logo Customizer |

---

## Building & Running

```bash
# Build both the RG35XXSP aarch64 binary (boot_logo), x86_64 desktop binary (boot_logo_linux), and sdcard_package/
make all

# Run the desktop Linux X11 simulator (uses /tmp/rg35xxsp_boot_logo_sim/boot/bootlogo.bmp)
make run-linux
```

## Installing on the Anbernic RG 35xxSP

Copy the contents of `sdcard_package/roms/ports/` to `/userdata/roms/ports/` on your RG 35xxSP SD card:
- `/userdata/roms/ports/Boot_Logo.sh`
- `/userdata/roms/ports/boot_logo/boot_logo`
- `/userdata/roms/ports/boot_logo/boot_logo.png`
- `/userdata/roms/ports/boot_logo/logos/*.bmp`
- `/userdata/roms/ports/boot_logo/backup/bootlogo.stock.bmp`
