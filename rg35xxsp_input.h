/*
 * rg35xxsp_input.h - Canonical Input & Keymapping Reference for Anbernic RG 35xxSP
 *
 * Target Hardware : Anbernic RG 35xxSP (Allwinner H700 SoC)
 * Kernel Drivers  :
 *   1. "Anbernic RG35XX-SP Controller" (gpio-keys-polled, vendor=0x0001, product=0x0001)
 *   2. "axp2202-pek"                   (AXP2202 PMIC Power Button)
 *   3. "hall-switch"                   (Clamshell Lid Hall Sensor)
 *   4. "dierct-keys-polled"            (Legacy Stock OS keyboard driver; unbound on Knulli)
 *
 * =========================================================================================
 * 1. STANDARD APPLICATION CONTROL CONVENTIONS (ALL RG 35xxSP APPS)
 * =========================================================================================
 *
 * All applications written for the Anbernic RG 35xxSP MUST follow these consistent UX roles:
 *
 *   (1) QUIT / EXIT APP       : MENU / RESET (`BTN_MENU`, or `Esc` on Desktop/x64)
 *                               - Consistently exits the application across all programs.
 *                               - No other key (such as Start, Select, or B) quits the app.
 *
 *   (1b) PRIMARY SWITCHERS    : D-PAD (`BTN_UP`, `BTN_DOWN`, `BTN_LEFT`, `BTN_RIGHT`)
 *                               and L1 / R1 (`BTN_L1`, `BTN_R1`)
 *                               - Used as the primary navigation, parameter, and page/tab
 *                                 switchers across all programs.
 *
 *   (2) HELP OVERLAY          : Y (`BTN_Y`, or `Y` on Desktop/x64)
 *                               - Consistently toggles the Help screen/overlay in all apps.
 *
 *   (3) MODAL SHIFTS          : SELECT (`BTN_SELECT`, or `Tab` on Desktop/x64)
 *                               - Consistently cycles or shifts between modes if the app
 *                                 has multiple operating modes.
 *
 *   (4) PRIMARY / SECONDARY   : A (`BTN_A`) = Start/Stop playback or perform Primary Action.
 *       ACTIONS                 B (`BTN_B`) = Perform Secondary Action (or Cancel/Back
 *                                             within sub-views/overlays).
 *
 * =========================================================================================
 * 2. COMPLETE HARDWARE KEYMAPPING SHEET (Allwinner H700 Device Tree & Linux evdev)
 * =========================================================================================
 *
 * A. D-PAD (Physical Directional Pad - GPIO Polled via `gpio-keys-polled`)
 * -----------------------------------------------------------------------------------------
 * Physical  FDT Label  GPIO     Normal Mode (nds_pwrkey=0)    Virtual Stick Mode (nds_pwrkey=1)
 * --------  ---------  -------  ----------------------------  ---------------------------------
 * UP        keyUP      PE8  (L) EV_ABS (3) ABS_HAT0Y (17) -1  EV_ABS (3) ABS_RY (4) -1
 * DOWN      keyDOWN    PE9  (L) EV_ABS (3) ABS_HAT0Y (17) +1  EV_ABS (3) ABS_RY (4) +1
 * LEFT      keyLeft    PA8  (L) EV_ABS (3) ABS_HAT0X (16) -1  EV_ABS (3) ABS_RX (3) -1
 * RIGHT     keyRight   PA9  (L) EV_ABS (3) ABS_HAT0X (16) +1  EV_ABS (3) ABS_RX (3) +1
 *
 * Note on D-Pad evdev `EV_KEY` fallbacks across CFWs (MuOS / Rocknix / PortMaster / Stock):
 *   - Standard Linux D-Pad keys : 544 (BTN_DPAD_UP), 545 (BTN_DPAD_DOWN),
 *                                 546 (BTN_DPAD_LEFT), 547 (BTN_DPAD_RIGHT)
 *   - Keyboard cursor keys      : 103 (KEY_UP), 108 (KEY_DOWN), 105 (KEY_LEFT), 106 (KEY_RIGHT)
 *   - Trigger-Happy D-Pad keys  : 704 (BTN_TRIGGER_HAPPY1 = Left),
 *                                 705 (BTN_TRIGGER_HAPPY2 = Right),
 *                                 706 (BTN_TRIGGER_HAPPY3 = Up),
 *                                 707 (BTN_TRIGGER_HAPPY4 = Down)
 *     CRITICAL BUG NOTE: Previously, `boot_logo.c` and `chordwheel.c` mapped codes 704..706
 *     to `BTN_MENU` (Quit)! On CFWs/wrappers that emit `BTN_TRIGGER_HAPPY1..4` for D-Pad,
 *     pressing the D-Pad immediately triggered `BTN_MENU` and exited the application.
 *
 * B. FACE, SHOULDER, SYSTEM & VOLUME BUTTONS (`EV_KEY` type = 1)
 * -----------------------------------------------------------------------------------------
 * Physical   FDT Label  GPIO      Code (Dec/Hex)   Linux Header Macro  Standard RG35xxSP Role
 * ---------  ---------  --------  ---------------  ------------------  ----------------------------------
 * A (East)   keyA       PA0  (L)  304  (0x130)     BTN_SOUTH / BTN_A   Start/Stop Playback / Primary Act
 * B (South)  keyB       PA1  (L)  305  (0x131)     BTN_EAST  / BTN_B   Secondary Action / Back
 * X / Y (*)  keyX       PA2  (L)  306  (0x132)     BTN_C               Auxiliary Action / Help (*)
 * Y / X (*)  keyY       PA3  (L)  307  (0x133)     BTN_NORTH / BTN_X   Help Overlay (Y) (*)
 * L1         keyL1      PA10 (L)  308  (0x134)     BTN_WEST  / BTN_Y   Primary Switcher (Prev / Dec)
 * R1         keyR1      PA12 (L)  309  (0x135)     BTN_Z               Primary Switcher (Next / Inc)
 * SELECT     keySEl     PA4  (L)  310  (0x136)     BTN_TL              Modal Shift (Switch App Mode)
 * START      keyStart   PA5  (L)  311  (0x137)     BTN_TR              Start Button (App-specific)
 * MENU (M)   keyMenu    PE3  (L)  312  (0x138)     BTN_TL2             MENU / RESET [QUIT APPLICATION]
 *   + synth  (kernel)   --        354  (0x162)     KEY_GOTO            Emitted with Menu (312)
 *   + gamepad(uinput)   --        316  (0x13c)     BTN_MODE            Emitted by SDL/uinput Menu
 * L2         keyL2      PE1  (L)  314  (0x13a)     BTN_SELECT (!!)     L2 Trigger (NOT Select!)
 * R2         keyR2      PE2  (L)  315  (0x13b)     BTN_START  (!!)     R2 Trigger (NOT Start!)
 * VOL +      keyVol+    PE0  (L)  115  (0x073)     KEY_VOLUMEUP        Volume Up
 * VOL -      keyvol-    PE4  (L)  114  (0x072)     KEY_VOLUMEDOWN      Volume Down
 * POWER      axp2202    PMIC      116  (0x074)     KEY_POWER           Short=StickMode, Hold=Off
 * LID CLOSE  hall-sw    GPIO      110  (0x06e)     KEY_INSERT          Clamshell Closed
 * LID OPEN   hall-sw    GPIO      111  (0x06f)     KEY_DELETE          Clamshell Opened
 *
 * (*) Note on 306 vs 307 (X and Y):
 *     In the Allwinner H700 Device Tree (`gpio_keys`), PA2 (`keyX`) is code 306 and PA3 (`keyY`)
 *     is code 307. However, in Knulli's `es_input.cfg`, code 306 is bound to `y` (West) and
 *     code 307 is bound to `x` (North). See `RG35XXSP_KNULLI_ES_XY` below to toggle if needed.
 *
 * (!!) Note on 314 (`BTN_SELECT`) and 315 (`BTN_START`):
 *     Because `<linux/input-event-codes.h>` names 0x13a (314) `BTN_SELECT` and 0x13b (315)
 *     `BTN_START`, standard Linux code that switches on `BTN_SELECT`/`BTN_START` macros
 *     accidentally binds L2 (314) and R2 (315) to Select/Start instead of 310 and 311!
 *
 * =========================================================================================
 * 3. DESKTOP / X64 (`HOST_X11`) KEYBOARD SIMULATION MAPPING
 * =========================================================================================
 *
 * RG 35xxSP Button   Desktop X11 Key(s)                Standard Role / Convention
 * ----------------   -------------------------------   ------------------------------------
 * MENU / RESET (M)   Esc (`XK_Escape`)                 QUIT APP (Consistent across all apps)
 * D-Pad UP           Up Arrow (`XK_Up`)                Primary Switcher (Up)
 * D-Pad DOWN         Down Arrow (`XK_Down`)            Primary Switcher (Down)
 * D-Pad LEFT         Left Arrow (`XK_Left`)            Primary Switcher (Left)
 * D-Pad RIGHT        Right Arrow (`XK_Right`)          Primary Switcher (Right)
 * L1 Shoulder        `L`, `Q`, `[`, `Page_Up`          Primary Switcher (Prev / L1)
 * R1 Shoulder        `R`, `E`, `]`, `Page_Down`        Primary Switcher (Next / R1)
 * A (East)           `A`, `Space`, `Z`                 Start/Stop Playback / Primary Action
 * B (South)          `B`, `Backspace`                  Secondary Action / Back
 * Y (West)           `Y`                               Help Overlay (Consistent in all apps)
 * X (North)          `X`                               Auxiliary / X Action
 * SELECT             `Tab`                             Modal Shift (If app has modes)
 * START              `Enter` (`XK_Return`)             Start Button (NEVER quits app)
 * L2 Trigger         `1`                               L2 Action
 * R2 Trigger         `2`                               R2 Action
 * =========================================================================================
 */

#ifndef RG35XXSP_INPUT_H
#define RG35XXSP_INPUT_H

#include <stdint.h>

/* Undefine any colliding <linux/input-event-codes.h> BTN_* macros */
#undef BTN_UP
#undef BTN_DOWN
#undef BTN_LEFT
#undef BTN_RIGHT
#undef BTN_A
#undef BTN_B
#undef BTN_X
#undef BTN_Y
#undef BTN_L1
#undef BTN_R1
#undef BTN_L2
#undef BTN_R2
#undef BTN_SELECT
#undef BTN_START
#undef BTN_MENU
#undef BTN_VOL_UP
#undef BTN_VOL_DOWN
#undef BTN_LID_CLOSE
#undef BTN_LID_OPEN

/* Canonical button bitmasks used across all RG 35xxSP applications */
#define BTN_UP         (1u << 0)
#define BTN_DOWN       (1u << 1)
#define BTN_LEFT       (1u << 2)
#define BTN_RIGHT      (1u << 3)
#define BTN_A          (1u << 4)
#define BTN_B          (1u << 5)
#define BTN_X          (1u << 6)
#define BTN_Y          (1u << 7)
#define BTN_L1         (1u << 8)
#define BTN_R1         (1u << 9)
#define BTN_L2         (1u << 10)
#define BTN_R2         (1u << 11)
#define BTN_SELECT     (1u << 12)
#define BTN_START      (1u << 13)
#define BTN_MENU       (1u << 14)
#define BTN_VOL_UP     (1u << 15)
#define BTN_VOL_DOWN   (1u << 16)
#define BTN_LID_CLOSE  (1u << 17)
#define BTN_LID_OPEN   (1u << 18)

/* Backwards-compatible aliases for apps that use BTN_L / BTN_R for L1 / R1 */
#ifndef BTN_L
#define BTN_L          BTN_L1
#endif
#ifndef BTN_R
#define BTN_R          BTN_R1
#endif

/*
 * Standard semantic action roles across all Anbernic RG 35xxSP programs:
 *   1. MENU/RESET (`BTN_MENU`)        -> Quit application
 *   2. DPAD + L1/R1                   -> Primary switchers
 *   3. Y (`BTN_Y`)                    -> Toggle Help overlay
 *   4. SELECT (`BTN_SELECT`)          -> Modal shift (for apps with modes)
 *   5. A (`BTN_A`)                    -> Start/stop playback or Primary action
 *   6. B (`BTN_B`)                    -> Secondary action
 */
#define BTN_DEFAULT_EXIT       BTN_MENU
#define BTN_ACTION_QUIT        BTN_MENU
#define BTN_ACTION_HELP        BTN_Y
#define BTN_ACTION_MODE_SHIFT  BTN_SELECT
#define BTN_ACTION_PRIMARY     BTN_A
#define BTN_ACTION_SECONDARY   BTN_B
#define BTN_SWITCHER_DPAD_MASK (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)
#define BTN_SWITCHER_SHOULDER  (BTN_L1 | BTN_R1)
#define BTN_PRIMARY_SWITCHERS  (BTN_SWITCHER_DPAD_MASK | BTN_SWITCHER_SHOULDER)

/* Raw Linux evdev keycodes on the Anbernic RG 35xxSP (`Anbernic RG35XX-SP Controller`) */
#define RG35XXSP_EVKEY_A          304  /* 0x130 (BTN_SOUTH)  - Physical A (East)  */
#define RG35XXSP_EVKEY_B          305  /* 0x131 (BTN_EAST)   - Physical B (South) */
#define RG35XXSP_EVKEY_KEYX_RAW   306  /* 0x132 (BTN_C)      - FDT keyX / Knulli Y */
#define RG35XXSP_EVKEY_KEYY_RAW   307  /* 0x133 (BTN_NORTH)  - FDT keyY / Knulli X */
#define RG35XXSP_EVKEY_L1         308  /* 0x134 (BTN_WEST)   - Physical L1 */
#define RG35XXSP_EVKEY_R1         309  /* 0x135 (BTN_Z)      - Physical R1 */
#define RG35XXSP_EVKEY_SELECT     310  /* 0x136 (BTN_TL)     - Physical SELECT */
#define RG35XXSP_EVKEY_START      311  /* 0x137 (BTN_TR)     - Physical START */
#define RG35XXSP_EVKEY_MENU       312  /* 0x138 (BTN_TL2)    - Physical MENU (M) */
#define RG35XXSP_EVKEY_L2         314  /* 0x13a (BTN_SELECT) - Physical L2 */
#define RG35XXSP_EVKEY_R2         315  /* 0x13b (BTN_START)  - Physical R2 */
#define RG35XXSP_EVKEY_MODE       316  /* 0x13c (BTN_MODE)   - Virtual Gamepad Menu */
#define RG35XXSP_EVKEY_MENU_GOTO  354  /* 0x162 (KEY_GOTO)   - Synthesized by H700 Menu */

#define RG35XXSP_EVKEY_LID_CLOSE  110  /* 0x06e (KEY_INSERT)     - Hall sensor close */
#define RG35XXSP_EVKEY_LID_OPEN   111  /* 0x06f (KEY_DELETE)     - Hall sensor open  */
#define RG35XXSP_EVKEY_VOL_DOWN   114  /* 0x072 (KEY_VOLUMEDOWN) - Volume - */
#define RG35XXSP_EVKEY_VOL_UP     115  /* 0x073 (KEY_VOLUMEUP)   - Volume + */
#define RG35XXSP_EVKEY_POWER      116  /* 0x074 (KEY_POWER)      - PMIC Power key */

/*
 * By default we follow the H700 Device Tree (`keyX = 306`, `keyY = 307`).
 * Define `RG35XXSP_KNULLI_ES_XY` to match Knulli `es_input.cfg` (`y = 306`, `x = 307`).
 */
#ifdef RG35XXSP_KNULLI_ES_XY
#define RG35XXSP_EVKEY_Y          RG35XXSP_EVKEY_KEYX_RAW
#define RG35XXSP_EVKEY_X          RG35XXSP_EVKEY_KEYY_RAW
#else
#define RG35XXSP_EVKEY_X          RG35XXSP_EVKEY_KEYX_RAW
#define RG35XXSP_EVKEY_Y          RG35XXSP_EVKEY_KEYY_RAW
#endif

/*
 * Translate a Linux `EV_KEY` code (`ev.code`) into a `BTN_*` bitmask.
 * Returns 0 for unrecognized keys.
 */
static inline uint32_t rg35xxsp_map_evkey(int code) {
    switch (code) {
        /* 1. D-Pad discrete keycodes (cursor keys, standard DPAD, and Trigger-Happy 704..707) */
        case 103: /* KEY_UP */
        case 544: /* BTN_DPAD_UP */
        case 706: /* BTN_TRIGGER_HAPPY3 (D-Pad Up on uinput/CFW gamepads) */
            return BTN_UP;

        case 108: /* KEY_DOWN */
        case 545: /* BTN_DPAD_DOWN */
        case 707: /* BTN_TRIGGER_HAPPY4 (D-Pad Down on uinput/CFW gamepads) */
            return BTN_DOWN;

        case 105: /* KEY_LEFT */
        case 546: /* BTN_DPAD_LEFT */
        case 704: /* BTN_TRIGGER_HAPPY1 (D-Pad Left on uinput/CFW gamepads) */
            return BTN_LEFT;

        case 106: /* KEY_RIGHT */
        case 547: /* BTN_DPAD_RIGHT */
        case 705: /* BTN_TRIGGER_HAPPY2 (D-Pad Right on uinput/CFW gamepads) */
            return BTN_RIGHT;

        /* 2. Face buttons (Anbernic RG35XX-SP Controller) */
        case RG35XXSP_EVKEY_A:        return BTN_A;
        case RG35XXSP_EVKEY_B:        return BTN_B;
        case RG35XXSP_EVKEY_X:        return BTN_X;
        case RG35XXSP_EVKEY_Y:        return BTN_Y;

        /* 3. Shoulder & Trigger buttons */
        case RG35XXSP_EVKEY_L1:       return BTN_L1;
        case RG35XXSP_EVKEY_R1:       return BTN_R1;
        case RG35XXSP_EVKEY_L2:       return BTN_L2;
        case RG35XXSP_EVKEY_R2:       return BTN_R2;

        /* 4. System buttons (Select, Start, Menu) */
        case RG35XXSP_EVKEY_SELECT:   return BTN_SELECT;
        case RG35XXSP_EVKEY_START:    return BTN_START;
        case RG35XXSP_EVKEY_MENU:
        case RG35XXSP_EVKEY_MODE:
        case RG35XXSP_EVKEY_MENU_GOTO:
        case 1:   /* KEY_ESC (USB keyboard / desktop evdev) */
            return BTN_MENU;

        /* 5. Volume & Clamshell Hall-Sensor keys */
        case RG35XXSP_EVKEY_VOL_UP:    return BTN_VOL_UP;
        case RG35XXSP_EVKEY_VOL_DOWN:  return BTN_VOL_DOWN;
        case RG35XXSP_EVKEY_LID_CLOSE: return BTN_LID_CLOSE;
        case RG35XXSP_EVKEY_LID_OPEN:  return BTN_LID_OPEN;

        default:
            return 0;
    }
}

/*
 * Update `held` bitmask from a Linux `EV_ABS` event (`ev.code`, `ev.value`).
 * Handles both Normal D-Pad mode (`ABS_HAT0X=16`, `ABS_HAT0Y=17`, range -1..+1)
 * and Virtual Stick mode (`ABS_RX=3`, `ABS_RY=4`, range -1..+1, plus `ABS_X=0`, `ABS_Y=1`).
 */
static inline uint32_t rg35xxsp_update_evabs(uint32_t held, int code, int val,
                                             int axis_min, int axis_max) {
    int neg = 0, pos = 0;
    if (axis_max > axis_min) {
        int mid = (axis_min + axis_max) / 2;
        int deadzone = (axis_max - axis_min) / 4;
        if (deadzone < 1) {
            neg = (val < mid);
            pos = (val > mid);
        } else {
            neg = (val < mid - deadzone);
            pos = (val > mid + deadzone);
        }
    } else {
        neg = (val < 0);
        pos = (val > 0);
    }

    if (code == 16 || code == 0 || code == 3) { /* ABS_HAT0X (16), ABS_X (0), ABS_RX (3) */
        held &= ~(BTN_LEFT | BTN_RIGHT);
        if (neg) held |= BTN_LEFT;
        if (pos) held |= BTN_RIGHT;
    } else if (code == 17 || code == 1 || code == 4) { /* ABS_HAT0Y (17), ABS_Y (1), ABS_RY (4) */
        held &= ~(BTN_UP | BTN_DOWN);
        if (neg) held |= BTN_UP;
        if (pos) held |= BTN_DOWN;
    }
    return held;
}

#ifdef HOST_X11
#include <X11/Xlib.h>
#include <X11/keysym.h>

/*
 * Translate an X11 `KeySym` into a `BTN_*` bitmask for Desktop/x64 builds.
 * `XK_Escape` maps to `BTN_MENU` (the sole default exit key).
 */
static inline uint32_t rg35xxsp_map_x11_keysym(unsigned long ks) {
    switch (ks) {
        case XK_Up:
        case XK_KP_Up:     return BTN_UP;
        case XK_Down:
        case XK_KP_Down:   return BTN_DOWN;
        case XK_Left:
        case XK_KP_Left:   return BTN_LEFT;
        case XK_Right:
        case XK_KP_Right:  return BTN_RIGHT;

        case XK_a:
        case XK_A:
        case XK_space:
        case XK_KP_Space:
        case XK_Alt_L:
        case XK_Alt_R:
        case XK_z:
        case XK_Z:         return BTN_A;

        case XK_b:
        case XK_B:
        case XK_Control_L:
        case XK_Control_R:
        case XK_BackSpace: return BTN_B;

        case XK_x:
        case XK_X:
        case XK_Shift_L:   return BTN_X;

        case XK_y:
        case XK_Y:         return BTN_Y;

        case XK_l:
        case XK_L:
        case XK_q:
        case XK_Q:
        case XK_bracketleft:
        case XK_Page_Up:
        case XK_KP_Page_Up:return BTN_L1;

        case XK_r:
        case XK_R:
        case XK_e:
        case XK_E:
        case XK_bracketright:
        case XK_Page_Down:
        case XK_KP_Page_Down: return BTN_R1;

        case XK_1:         return BTN_L2;
        case XK_2:         return BTN_R2;

        case XK_Tab:
        case XK_ISO_Left_Tab: return BTN_SELECT;
        case XK_Return:
        case XK_KP_Enter:  return BTN_START;

        /* Esc is the Desktop equivalent of the RG 35xxSP Menu key (Default Exit Key) */
        case XK_Escape:    return BTN_MENU;

        default:           return 0;
    }
}
#endif /* HOST_X11 */

#endif /* RG35XXSP_INPUT_H */
