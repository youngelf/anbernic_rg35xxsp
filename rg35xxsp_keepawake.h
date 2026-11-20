/*
 * rg35xxsp_keepawake.h - Keep-Awake / Anti-Blanking Helper for Anbernic RG 35xxSP
 *
 * Prevents Knulli / Batocera power-management daemons and Linux VT console blanking
 * from turning off the display or muting audio during hands-off apps (Pomodoro,
 * Night Clock, Metronome, PocketBand, MusicPlayer, LickTrainer, SightRead):
 *
 *   1. Knulli idlewatcher / battery-saver pause flags:
 *        - /var/run/battery-saver/<app>.pause  (Knulli Gladiator II & knulli-main idlewatcher)
 *        - /var/run/battery_saver.pause        (Knulli Gladiator 20250505)
 *      Cleaned up on normal exit (and via atexit) so power saving resumes afterward.
 *      Note: Clamshell lid-close (lid-control via triggerhappy) does NOT check *.pause,
 *      so closing the RG 35xxSP lid still sleeps/turns off the screen normally.
 *   2. SDL2 screensaver inhibition:
 *        - Calls SDL_DisableScreenSaver() via dlsym() if libSDL2 is loaded.
 *   3. Linux kernel VT / framebuffer console blanking inhibition:
 *        - Sends ANSI ESC[9;0] / ESC[14;0] to /dev/tty0 on startup (same as setterm -blank 0).
 *        - Unblanks /sys/class/graphics/fb0/blank on startup and periodically (every 30s)
 *          ONLY when the clamshell lid is open and Knulli has not intentionally turned off
 *          the backlight (checked via /var/run/{knulli,batocera}-brightness & lcd_off.flag).
 *
 * No-op on HOST_X11 desktop builds.
 */
#ifndef RG35XXSP_KEEPAWAKE_H
#define RG35XXSP_KEEPAWAKE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef TARGET_RG35XXSP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/stat.h>

static char     g_ka_pause_file[128] = {0};
static bool     g_ka_created_legacy  = false;
static bool     g_ka_initialized     = false;
static bool     g_ka_lid_closed      = false;
static uint32_t g_ka_last_poke_ms    = 0;

static inline bool rg35_ka_write_str(const char *path, const char *str) {
    int fd = open(path, O_WRONLY | O_NOCTTY);
    if (fd < 0) return false;
    size_t len = strlen(str);
    ssize_t n = write(fd, str, len);
    close(fd);
    return (n == (ssize_t)len);
}

static inline void rg35_ka_touch(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) close(fd);
}

static inline bool rg35_ka_display_intentionally_off(void) {
    if (g_ka_lid_closed) return true;
    if (access("/var/run/lcd_off.flag", F_OK) == 0) return true;
    if (access("/var/run/knulli-brightness", F_OK) == 0) return true;
    if (access("/var/run/batocera-brightness", F_OK) == 0) return true;
    return false;
}

static inline void rg35_ka_unblank_fb0(void) {
    if (rg35_ka_display_intentionally_off()) return;
    if (access("/sys/class/graphics/fb0/blank", W_OK) == 0) {
        (void)rg35_ka_write_str("/sys/class/graphics/fb0/blank", "0\n");
    }
}

static inline void rg35xxsp_keepawake_shutdown(void) {
    if (g_ka_pause_file[0] != '\0') {
        unlink(g_ka_pause_file);
        g_ka_pause_file[0] = '\0';
    }
    if (g_ka_created_legacy) {
        unlink("/var/run/battery_saver.pause");
        g_ka_created_legacy = false;
    }
    g_ka_initialized = false;
}

static inline void rg35xxsp_keepawake_init(const char *app_name, void *sdl2_lib) {
    if (sdl2_lib) {
        void (*fn_disable_ss)(void) = NULL;
        *(void **)(&fn_disable_ss) = dlsym(sdl2_lib, "SDL_DisableScreenSaver");
        if (fn_disable_ss) fn_disable_ss();
    }

    if (g_ka_initialized) return;
    g_ka_initialized = true;
    g_ka_lid_closed = false;
    g_ka_last_poke_ms = 0;

    if (app_name && app_name[0]) {
        mkdir("/var/run/battery-saver", 0755);
        snprintf(g_ka_pause_file, sizeof(g_ka_pause_file),
                 "/var/run/battery-saver/%s.pause", app_name);
        rg35_ka_touch(g_ka_pause_file);

        if (access("/var/run/battery_saver.pause", F_OK) != 0) {
            int fd = open("/var/run/battery_saver.pause", O_WRONLY | O_CREAT | O_EXCL, 0644);
            if (fd >= 0) {
                close(fd);
                g_ka_created_legacy = true;
            }
        }
    }

    /* Disable Linux kernel VT blanking (equivalent to `setterm -blank 0 -powersave off`) */
    (void)rg35_ka_write_str("/dev/tty0", "\033[9;0]\033[14;0]");
    (void)rg35_ka_write_str("/dev/tty3", "\033[9;0]\033[14;0]");

    /* Ensure framebuffer is unblanked on startup */
    rg35_ka_unblank_fb0();

    atexit(rg35xxsp_keepawake_shutdown);
}

static inline void rg35xxsp_keepawake_set_lid_closed(bool closed) {
    g_ka_lid_closed = closed;
}

/*
 * Call once per frame from the main loop (safe to call at 20..60 FPS; rate-limited to 30s).
 */
static inline void rg35xxsp_keepawake_tick(uint32_t now_ms) {
    if (!g_ka_initialized) return;
    if (g_ka_last_poke_ms == 0 || (uint32_t)(now_ms - g_ka_last_poke_ms) >= 30000u) {
        g_ka_last_poke_ms = now_ms;
        rg35_ka_unblank_fb0();
    }
}

static inline void rg35xxsp_keepawake_tick_now(void) {
    if (!g_ka_initialized) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t now_ms = (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull);
    rg35xxsp_keepawake_tick(now_ms);
}

#else /* !TARGET_RG35XXSP */

static inline void rg35xxsp_keepawake_init(const char *app_name, void *sdl2_lib) {
    (void)app_name;
    (void)sdl2_lib;
}
static inline void rg35xxsp_keepawake_set_lid_closed(bool closed) {
    (void)closed;
}
static inline void rg35xxsp_keepawake_tick(uint32_t now_ms) {
    (void)now_ms;
}
static inline void rg35xxsp_keepawake_tick_now(void) {}
static inline void rg35xxsp_keepawake_shutdown(void) {}

#endif /* TARGET_RG35XXSP */

#endif /* RG35XXSP_KEEPAWAKE_H */
