/**
 * platform_dc.c — KallistiOS / Dreamcast platform implementation.
 *
 * DC counterpart to sdl/src/platform/platform_sdl.c. Implements the platform.h
 * surface using KOS APIs: maple controllers for input/gamepads, gettimeofday
 * for timing, dbgio_printf for log output. The renderer (PVR) lives in
 * r_pvr_backend.c / render_pvr.c — this file only handles non-render platform
 * concerns.
 */

#include <kos.h>
#include <kos/net.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/maple/vmu.h>
#include <dc/vmu_fb.h>
#include <dc/biosfont.h>
#include <dc/flashrom.h>
#include <dc/net/broadband_adapter.h>
#include <ppp/ppp.h>
#include <dc/modem/modem.h>
#include <stdint.h>

#include "sonicr_invert.XBM"

#ifdef SONICR_PPP_SERIAL
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_MALLOCSTATS);
#else
KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <arch/timer.h>

#include "platform.h"
#include "pad_bits.h"   /* PAD_LEFT / PAD_UP / ... for the pad poller */
#include "sonicr_paths.h"   /* PATH_GENERAL_RAW for the slow-media probe */

/* Filesystem access lock */
mutex_t io_lock;

/* Shared keystate — written by platform_pump_events, read by InputPoll
 * via DIK scan codes (see platform.h DIK_* defines). */
unsigned char s_keystate[256];
static int s_quitRequested = 0;

/* Joystick state — DC has 1 controller per maple slot, up to 4. */
#define MAX_GAMEPADS 4
static int s_gamepadCount = 0;

extern unsigned char g_keyPressState[320];
extern short g_joystickConfigWords[];
extern char g_joystickSlots[4][282];
extern short g_joystickDeviceFlags[8];
extern char g_joystickDeviceNames[4][260]; /* 0x00675C54, stride 0x104 */

/* Every Dreamcast pad reports the same identity as far as we care — a port is
 * either occupied or it isn't. What matters is that the name is STABLE across
 * boots and matches what got written into JOYSTICK.INF, because that compare
 * in SyncJoystickSlots is what decides whether slot N keeps its saved mapping
 * or gets reset to defaults. Changing this string orphans existing VMU pad
 * configs — every slot would look like a new device once and reset. */
#define DC_PAD_NAME "Dreamcast Controller"

/* =====================================================================
 * Display init — for DC, the PVR renderer does its own video setup.
 * Fullscreen is implicit on DC, so the flag is ignored.
 * ===================================================================== */

/* =====================================================================
 * Vblank-driven 30 Hz frame cap.
 *
 * Replaces the gettimeofday-polling WaitForFrameCap on DC. The vblank
 * IRQ bumps `s_vblticker` and wakes one genwait waiter; the waiter
 * blocks until at least 2 vblanks have elapsed since the previous frame
 * (60 Hz vblank × 2 = 30 Hz cap). Thread sleeps instead of spinning,
 * so there's no CPU burn waiting for the deadline.
 * ===================================================================== */
static volatile uint64_t s_vblticker = 0;
static uint64_t s_lastVbltick = 0;

static void vblfunc(uint32_t code, void *data)
{
    (void)code;
    (void)data;
    s_vblticker++;
    genwait_wake_one((void *)&s_vblticker);
}

void DC_WaitForVBlank2(void)
{
    while (s_vblticker <= s_lastVbltick + 1) {
#if KOS_VERSION_BELOW(2, 2, 3)
        genwait_wait((void *)&s_vblticker, NULL, 0, NULL);
#else
        genwait_wait((void *)&s_vblticker, NULL, 0);
#endif
    }
    s_lastVbltick = s_vblticker;
}

extern void PVR_Init(void);
extern void PVR_InitTextures(void);
extern void PVR_InitDrainTpage(void);
extern void PVR_InitPad448Tpage(void);

/* Capacity warning text (or NULL); mode/text live in save_vmu.c. */
extern const char *SaveVMU_CapacityWarning(void);

/* Draw the VMU capacity warning on the plain RGB565 framebuffer BEFORE PVR
 * takes over the display — VF3tb-style: black screen, white BIOS font — and
 * wait for START. No-op when the card has room. Must run after vid_set_mode
 * and after io_lock is initialised, but before PVR_Init. */
static void dc_show_vmu_warning(void)
{
    const char *msg = SaveVMU_CapacityWarning();
    if (!msg) return;

    const int W = vid_mode->width;
    const int H = vid_mode->height;
    uint16_t *fb = vram_s;

    for (int i = 0; i < W * H; i++) fb[i] = 0x0000;      /* black */

    /* BIOS font glyphs are 12x24; draw each newline-separated line. */
    int x = 96, y = 150;
    const char *p = msg;
    char line[64];
    while (*p) {
        int n = 0;
        while (*p && *p != '\n' && n < (int)sizeof(line) - 1) line[n++] = *p++;
        line[n] = '\0';
        if (*p == '\n') p++;
        bfont_draw_str(fb + (size_t)y * W + x, W, 0, line);   /* opaque=0: glyphs only */
        y += 28;
    }

    /* Wait for a fresh START press. Use thd_sleep, not vid_waitvbl: this runs
     * before PVR_Init, where vid_waitvbl does not block, so the old loop spun
     * through instantly. thd_sleep yields so the maple poll updates the button
     * state between checks. prevStart starts held, so a START left down at boot
     * (or a spurious read) must be released before a press counts. If no pad is
     * attached there's nothing to press, so proceed after a short beat. */
    int prevStart = 1;
    for (;;) {
        maple_device_t *c = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if (!c) { thd_sleep(1500); break; }
        cont_state_t *st = (cont_state_t *)maple_dev_status(c);
        int start = (st && (st->buttons & CONT_START)) ? 1 : 0;
        if (start && !prevStart) break;                  /* release -> press edge */
        prevStart = start;
        thd_sleep(16);
    }
    /* Let START release so it doesn't bleed into the title. */
    for (;;) {
        maple_device_t *c = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        cont_state_t *st = c ? (cont_state_t *)maple_dev_status(c) : NULL;
        if (!st || !(st->buttons & CONT_START)) break;
        thd_sleep(16);
    }

    for (int i = 0; i < W * H; i++) fb[i] = 0x0000;      /* clear before PVR starts */
}

/* ── Slow-media probe ─────────────────────────────────────────────────
 * Measured 2026-09-11 with mediatest/ on real hardware:
 *   burned CD-R : ~105 ms per far seek,  490 KB/s sequential
 *   ODE (CDI)   : ~1.7 ms per far seek, 3419 KB/s sequential
 * disc_type can't tell them apart — a CDI reads back as CD_CDROM on both —
 * so time the media instead: three far seeks, each followed by a 16 KB read.
 * Expected ~250 ms on CD-R vs ~20 ms on an ODE; the ~7x throughput gap alone
 * clears the threshold even if the seeks were free. Costs ~0.25 s once at
 * boot on slow media and ~0 elsewhere. Probes PATH_GENERAL_RAW through
 * DATA_DIR so it measures whatever the data really comes from (CD-R, ODE,
 * dcload). Runs before any music stream exists, so no io_lock needed. */
#define SLOWMEDIA_PROBE_CHUNK   (16 * 1024)
#define SLOWMEDIA_THRESHOLD_MS  70

static int s_slowMedia = 0;
static char s_slowMediaBuf[SLOWMEDIA_PROBE_CHUNK];

static void DetectSlowMedia(void)
{
    int fd = open(PATH_GENERAL_RAW, O_RDONLY);
    if (fd < 0) {
        printf("slow-media probe: cannot open %s, assuming fast\n", PATH_GENERAL_RAW);
        return;
    }
    long size = lseek(fd, 0, SEEK_END);
    if (size <= SLOWMEDIA_PROBE_CHUNK) {
        close(fd);
        return;
    }
    long span = size - SLOWMEDIA_PROBE_CHUNK;
    /* start -> near end -> middle: a real head move every time */
    long offs[3] = { 0, span, span / 2 };

    uint64_t t0 = timer_ms_gettime64();
    for (int i = 0; i < 3; i++) {
        lseek(fd, offs[i], SEEK_SET);
        read(fd, s_slowMediaBuf, SLOWMEDIA_PROBE_CHUNK);
    }
    uint32_t ms = (uint32_t)(timer_ms_gettime64() - t0);
    close(fd);

    s_slowMedia = (ms > SLOWMEDIA_THRESHOLD_MS);
    printf("slow-media probe: %lu ms -> %s\n", (unsigned long)ms,
           s_slowMedia ? "SLOW, music held during loads" : "fast");
}

/* Screens with a big texture load bracket it with PauseCD/ResumeCD only when
 * this is set — the original paused CD-DA there; on fast media the stream
 * keeps up on its own. */
int RunningFromSlowMedia(void)
{
    return s_slowMedia;
}

int platform_init(int width, int height, int fullscreen, const char *title)
{
    (void)width; (void)height; (void)fullscreen; (void)title;
#ifdef SONICR_DC_240P
    if (vid_check_cable() != CT_VGA) {
        vid_set_mode(DM_320x240_NTSC, PM_RGB565); // can try 888 also
    } else {
        vid_set_mode(DM_320x240_VGA, PM_RGB565);
    }
#else
    if (vid_check_cable() != CT_VGA) {
        vid_set_mode(DM_640x480_NTSC_IL, PM_RGB565);
    } else {
        vid_set_mode(DM_640x480_VGA, PM_RGB565);
    }
#endif

    vblank_handler_add(&vblfunc, NULL);
    if (mutex_init(&io_lock, MUTEX_TYPE_NORMAL) < 0) {
        return -1;
    }
    /* Check VMU capacity and, if short, show the black-screen warning now,
     * while the framebuffer is still a plain RGB565 surface (before PVR). */
    dc_show_vmu_warning();
    PVR_Init();
#if SONICR_DC_240P
//NO_DITHER
    // do not rely on compiling against a version of KOS
    // that is new enough to include `vid_set_dithering`
    // disable dithering in general
//#define PM_DITHER_BIT 8
    //uint32_t cfg = PVR_GET(PVR_FB_CFG_2);
    //cfg &= ~PM_DITHER_BIT;
    //PVR_SET(PVR_FB_CFG_2, cfg);
    PVR_SET(PVR_SCALER_CFG, 0x400);
#endif
    PVR_InitTextures();

    /* The two reserved always-resident tpages. Both must come AFTER
     * PVR_InitTextures, not from inside PVR_Init: that function loops every
     * slot and clears s_pvrTextures, s_pvrTextureFrozen and s_pvrNoColorKey,
     * so a tpage registered before it survives with g_tpageStateArray still 4
     * — it does not touch that array — but a NULL texture pointer.
     * select_header_pair() then returns NULL and every quad drawn from the
     * slot is silently dropped. Nothing after this point clears either slot.
     *
     * The drain page used to be initialised at the end of PVR_Init and was
     * therefore always NULL. It went unnoticed because HIDE_FPS is 0, so
     * FPS_Render draws its digits from g_tpageParallax1 and those quads do the
     * store-queue draining incidentally. Setting HIDE_FPS to 1 — which exists
     * to keep the drain while hiding the digits — would have aimed them at the
     * null slot instead, silently removing the drain and bringing back the
     * fade-iris hole. Drain first: the out-of-VRAM fallback borrows it. */
    PVR_InitDrainTpage();
    PVR_InitPad448Tpage();

    /* one time logo draw */
    maple_device_t *vmu;
    int index = 0;
    /* VMU framebuffer for logo draw */
    vmufb_t vmubuf;

    vmufb_clear(&vmubuf);

    vmufb_paint_xbm(
        &vmubuf,
        0, /* destination X */
        0, /* destination Y */
        sonicr_invert_width,
        sonicr_invert_height,
        sonicr_invert_bits);

    while((vmu = maple_enum_type(index, MAPLE_FUNC_LCD)) != NULL) {
        vmufb_present(&vmubuf, vmu);
        ++index;
    }

    DetectSlowMedia();

    return 0;
}

void platform_shutdown(void)
{
    /* KOS handles teardown via atexit hooks. */
}

/* No co-located-binary concept on the Dreamcast — data is mounted at DATA_DIR
 * (/cd or /pc). Returning NULL makes main.c fall back to DATA_DIR. */
const char *platform_base_path(void)
{
    return NULL;
}

/* =====================================================================
 * Maple keyboard → DIK scan code mapping.
 *
 * KOS exposes USB HID-style key codes via dc/maple/keyboard.h. Map the
 * common gameplay keys to DIK_* values matching what the binary expects.
 * Most DC users will play with a controller, so this is best-effort.
 * ===================================================================== */

static unsigned char hid_to_dik(int hid)
{
    /* HID usage codes for keyboard. KEY_A == 4, KEY_Z == 29. */
    switch (hid) {
        case 0x04:
            return 0x1E; /* A */
        case 0x05:
            return 0x30; /* B */
        case 0x06:
            return 0x2E; /* C */
        case 0x07:
            return 0x20; /* D */
        case 0x08:
            return 0x12; /* E */
        case 0x09:
            return 0x21; /* F */
        case 0x0A:
            return 0x22; /* G */
        case 0x0B:
            return 0x23; /* H */
        case 0x0C:
            return 0x17; /* I */
        case 0x0D:
            return 0x24; /* J */
        case 0x0E:
            return 0x25; /* K */
        case 0x0F:
            return 0x26; /* L */
        case 0x10:
            return 0x32; /* M */
        case 0x11:
            return 0x31; /* N */
        case 0x12:
            return 0x18; /* O */
        case 0x13:
            return 0x19; /* P */
        case 0x14:
            return 0x10; /* Q */
        case 0x15:
            return 0x13; /* R */
        case 0x16:
            return 0x1F; /* S */
        case 0x17:
            return 0x14; /* T */
        case 0x18:
            return 0x16; /* U */
        case 0x19:
            return 0x2F; /* V */
        case 0x1A:
            return 0x11; /* W */
        case 0x1B:
            return 0x2D; /* X */
        case 0x1C:
            return 0x15; /* Y */
        case 0x1D:
            return 0x2C; /* Z */
        case 0x1E:
            return 0x02; /* 1 */
        case 0x1F:
            return 0x03; /* 2 */
        case 0x20:
            return 0x04; /* 3 */
        case 0x21:
            return 0x05; /* 4 */
        case 0x22:
            return 0x06; /* 5 */
        case 0x23:
            return 0x07; /* 6 */
        case 0x24:
            return 0x08; /* 7 */
        case 0x25:
            return 0x09; /* 8 */
        case 0x26:
            return 0x0A; /* 9 */
        case 0x27:
            return 0x0B; /* 0 */
        case 0x28:
            return 0x1C; /* Return */
        case 0x29:
            return 0x01; /* Escape */
        case 0x2A:
            return 0x0E; /* Backspace */
        case 0x2B:
            return 0x0F; /* Tab */
        case 0x2C:
            return 0x39; /* Space */
        case 0x2D:
            return 0x0C; /* - */
        case 0x2E:
            return 0x0D; /* = */
        case 0x2F:
            return 0x1A; /* [ */
        case 0x30:
            return 0x1B; /* ] */
        case 0x31:
            return 0x2B; /* backslash */
        case 0x33:
            return 0x27; /* ; */
        case 0x34:
            return 0x28; /* ' */
        case 0x35:
            return 0x29; /* ` */
        case 0x36:
            return 0x33; /* , */
        case 0x37:
            return 0x34; /* . */
        case 0x38:
            return 0x35; /* / */
        case 0x3A:
            return 0x3B; /* F1 */
        case 0x3B:
            return 0x3C; /* F2 */
        case 0x3C:
            return 0x3D; /* F3 */
        case 0x3D:
            return 0x3E; /* F4 */
        case 0x3E:
            return 0x3F; /* F5 */
        case 0x3F:
            return 0x40; /* F6 */
        case 0x40:
            return 0x41; /* F7 */
        case 0x41:
            return 0x42; /* F8 */
        case 0x42:
            return 0x43; /* F9 */
        case 0x43:
            return 0x44; /* F10 */
        case 0x44:
            return 0x57; /* F11 */
        case 0x45:
            return 0x58; /* F12 */
        case 0x49:
            return 0xD2; /* Insert */
        case 0x4A:
            return 0xC7; /* Home */
        case 0x4B:
            return 0xC9; /* Page Up */
        case 0x4C:
            return 0xD3; /* Delete */
        case 0x4D:
            return 0xCF; /* End */
        case 0x4E:
            return 0xD1; /* Page Down */
        case 0x4F:
            return 0xCD; /* Right arrow */
        case 0x50:
            return 0xCB; /* Left arrow */
        case 0x51:
            return 0xD0; /* Down arrow */
        case 0x52:
            return 0xC8; /* Up arrow */
        /* Numeric keypad — all sixteen are remap-eligible per the ROM
         * table at 0x4FECBC. HID reports the physical key, so NumLock
         * state is irrelevant here. */
        case 0x54:
            return 0xB5; /* KP / */
        case 0x55:
            return 0x37; /* KP * */
        case 0x56:
            return 0x4A; /* KP - */
        case 0x57:
            return 0x4E; /* KP + */
        case 0x58:
            return 0x9C; /* KP Enter */
        case 0x59:
            return 0x4F; /* KP 1 */
        case 0x5A:
            return 0x50; /* KP 2 */
        case 0x5B:
            return 0x51; /* KP 3 */
        case 0x5C:
            return 0x4B; /* KP 4 */
        case 0x5D:
            return 0x4C; /* KP 5 */
        case 0x5E:
            return 0x4D; /* KP 6 */
        case 0x5F:
            return 0x47; /* KP 7 */
        case 0x60:
            return 0x48; /* KP 8 */
        case 0x61:
            return 0x49; /* KP 9 */
        case 0x62:
            return 0x52; /* KP 0 */
        case 0x63:
            return 0x53; /* KP . */
        case 0xE0:
            return 0x1D; /* Left Ctrl */
        case 0xE1:
            return 0x2A; /* Left Shift */
        case 0xE2:
            return 0x38; /* Left Alt */
        case 0xE4:
            return 0x9D; /* Right Ctrl */
        case 0xE5:
            return 0x36; /* Right Shift */
        case 0xE6:
            return 0xB8; /* Right Alt */
        default:
            return 0;
    }
}

int platform_poll_events(unsigned char *keystateOut, int keystateSize)
{
    if (keystateOut && keystateSize > 0) {
        int n = (keystateSize < 256) ? keystateSize : 256;
        memcpy(keystateOut, s_keystate, n);
    }
    return s_quitRequested;
}

#define ALLOW_FRAME_DUMP 0

void platform_pump_events(void)
{
    static int ssnum=0;
    static int frameDumpEnabled = 0;
    static int frameDumpNum = 0;
    static int scrollLockPrev = 0;

    /* Update keyboard state from any attached USB keyboard. */
    memset(s_keystate, 0, sizeof(s_keystate));
    maple_device_t *kb = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    if (kb) {
        kbd_state_t *st = (kbd_state_t *)maple_dev_status(kb);
        if (st) {
            for (int i = 0; i < 256; i++) {
                if (st->key_states[i].is_down) {
                    unsigned char dik = hid_to_dik(i);
                    if (dik) {
                        s_keystate[dik] = 0x80;
                    }
                }
            }
        }
    }

#if ALLOW_FRAME_DUMP
    /* Scroll Lock toggles per-frame VRAM dump to /pc/frame_NNNNN.ppm */
    if (kb) {
        kbd_state_t *st = (kbd_state_t *)maple_dev_status(kb);
        int scrollLockDown = st && st->key_states[KBD_KEY_SCRLOCK].is_down;
        if (scrollLockDown && !scrollLockPrev) {
            frameDumpEnabled = !frameDumpEnabled;
            if (frameDumpEnabled) {
                printf("[FRAMEDUMP] Recording started\n");
            }
            else {
                printf("[FRAMEDUMP] Recording stopped at frame %d\n", frameDumpNum);
            }
        }
        scrollLockPrev = scrollLockDown;
    }
    if (frameDumpEnabled) {
        char fn[64];
        sprintf(fn, "/pc/frame_%05d.ppm", frameDumpNum++);
        vid_screen_shot(fn);
    }
#endif

    /* Quit hotkey: L+R+Start on controller 0. The engine has nested
     * screen-state loops with no single exit point, and nothing in the
     * codebase actually consumes platform_poll_events' return value, so
     * exit() directly. KOS's atexit hooks handle teardown. */
    maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (cont) {
        cont_state_t *cs = (cont_state_t *)maple_dev_status(cont);
        if (cs && (cs->buttons & CONT_START) && (cs->buttons & CONT_A) && (cs->buttons & CONT_B) && (cs->buttons & CONT_X) && (cs->buttons & CONT_Y)) {
            extern void PauseCD();
            PauseCD();
            thd_sleep(250);
            exit(0);
        }
#if ALLOW_FRAME_DUMP
        if (cs && (cs->buttons & CONT_A) && cs->ltrig && cs->rtrig) {
            char fn[256];
            sprintf(fn, "/pc/screen%d.ppm", ssnum++);
            vid_screen_shot(fn);
        }
#endif
    }
}

/* =====================================================================
 * Gamepad polling — matches the SDL platform_poll_gamepads contract so
 * the engine's remap UI and action-bit lookup work unchanged.
 *
 * Direction bits (D-pad / analog stick) are inherent to the device and
 * land at fixed positions: 0x4000 left, 0x8000 right, 0x1000 up, 0x2000
 * down. Action buttons (A/B/X/Y/L/R/Start) go through the per-slot
 * config block at g_joystickSlots[slot][0x104 + b*2], which the in-game
 * remap UI commits to. We expose 7 stable button indices on DC:
 *
 *   index 0 = A      index 4 = L trigger
 *   index 1 = B      index 5 = R trigger
 *   index 2 = X      index 6 = Start
 *   index 3 = Y
 *
 * The remap UI scans g_keyPressState[slot*80 + b] to detect button
 * presses, so we populate that array per button held. ===================================================================== */

#define JOY_BUTTONS_PER_SLOT 80
#define DC_BUTTONS_PER_PAD 7
#define JOY_CFG_MAX 32       /* g_joystickConfigWords array size */
#define DC_STICK_DEADZONE 48 /* signed range is ~[-128..127], 0.4 of full scale */
#define DC_TRIGGER_THRESH 1

int platform_init_gamepads(void)
{
    s_gamepadCount = 0;
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (maple_enum_type(i, MAPLE_FUNC_CONTROLLER)) {
            /* Seed device-flags so SyncJoystickSlots (called from
             * InitPadTypes) propagates the button count to slot[0x118],
             * which ScanKeyRemap reads as the loop bound. The name is what
             * that same call compares against the slot's stored name to keep
             * a mapping loaded from the VMU instead of resetting it. Each
             * connected pad gets its own slot and its own saved mapping. */
            g_joystickDeviceFlags[s_gamepadCount] = (short)DC_BUTTONS_PER_PAD;
            strcpy(g_joystickDeviceNames[s_gamepadCount], DC_PAD_NAME);
            s_gamepadCount++;
        }
    }
    return s_gamepadCount;
}

/* Raw controller buttons for menu use — physical buttons, bypassing the
 * per-slot gameplay remap, OR'd across all four ports so any pad drives it.
 * The network screens map the keyboard-only F-keys onto these (no keyboard on
 * a stock DC). See MENUBTN_* in platform.h. */
unsigned int platform_menu_buttons(void)
{
    unsigned int m = 0;
    for (int s = 0; s < MAX_GAMEPADS; s++) {
        maple_device_t *cont = maple_enum_type(s, MAPLE_FUNC_CONTROLLER);
        if (!cont) continue;
        cont_state_t *cs = (cont_state_t *)maple_dev_status(cont);
        if (!cs) continue;
        if (cs->buttons & CONT_A)          m |= MENUBTN_A;
        if (cs->buttons & CONT_B)          m |= MENUBTN_B;
        if (cs->buttons & CONT_X)          m |= MENUBTN_X;
        if (cs->buttons & CONT_Y)          m |= MENUBTN_Y;
        if (cs->buttons & CONT_START)      m |= MENUBTN_START;
        if (cs->ltrig > DC_TRIGGER_THRESH) m |= MENUBTN_L;
        if (cs->rtrig > DC_TRIGGER_THRESH) m |= MENUBTN_R;
        if (cs->buttons & CONT_DPAD_UP)    m |= MENUBTN_UP;
        if (cs->buttons & CONT_DPAD_DOWN)  m |= MENUBTN_DOWN;
        if (cs->buttons & CONT_DPAD_LEFT)  m |= MENUBTN_LEFT;
        if (cs->buttons & CONT_DPAD_RIGHT) m |= MENUBTN_RIGHT;
    }
    return m;
}

int platform_poll_gamepads(unsigned short *joySlotState, int maxSlots)
{
    if (maxSlots > MAX_GAMEPADS) {
        maxSlots = MAX_GAMEPADS;
    }
    int found = 0;

    for (int s = 0; s < maxSlots; s++) {
        joySlotState[s] = 0;
        maple_device_t *cont = maple_enum_type(s, MAPLE_FUNC_CONTROLLER);
        unsigned char *pressBase = &g_keyPressState[s * JOY_BUTTONS_PER_SLOT];

        if (!cont) {
            memset(pressBase, 0, JOY_BUTTONS_PER_SLOT);
            continue;
        }
        cont_state_t *cs = (cont_state_t *)maple_dev_status(cont);
        if (!cs) {
            memset(pressBase, 0, JOY_BUTTONS_PER_SLOT);
            continue;
        }
        found++;

        unsigned short bits = 0;

        /* Direction bits — D-pad and left analog stick both feed the
         * same four bits the engine uses for steering / menu nav. */
        if (cs->buttons & CONT_DPAD_LEFT) {
            bits |= PAD_LEFT;
        }
        if (cs->buttons & CONT_DPAD_RIGHT) {
            bits |= PAD_RIGHT;
        }
        if (cs->buttons & CONT_DPAD_UP) {
            bits |= PAD_UP;
        }
        if (cs->buttons & CONT_DPAD_DOWN) {
            bits |= PAD_DOWN;
        }

        if (cs->joyx < -DC_STICK_DEADZONE) {
            bits |= PAD_LEFT;
        }
        if (cs->joyx > DC_STICK_DEADZONE) {
            bits |= PAD_RIGHT;
        }
        if (cs->joyy < -DC_STICK_DEADZONE) {
            bits |= PAD_UP;
        }
        if (cs->joyy > DC_STICK_DEADZONE) {
            bits |= PAD_DOWN;
        }

        /* Action buttons — packed into a stable 7-button index space, then
         * mapped to engine action bits via the per-slot config table the
         * remap UI commits to. Mirrors the SDL polling exactly. */
        int held[DC_BUTTONS_PER_PAD] = {
            (cs->buttons & CONT_A) != 0,     /* 0 */
            (cs->buttons & CONT_B) != 0,     /* 1 */
            (cs->buttons & CONT_X) != 0,     /* 2 */
            (cs->buttons & CONT_Y) != 0,     /* 3 */
            cs->ltrig > DC_TRIGGER_THRESH,   /* 4 */
            cs->rtrig > DC_TRIGGER_THRESH,   /* 5 */
            (cs->buttons & CONT_START) != 0, /* 6 */
        };

        const short *slotCfg = (const short *)&g_joystickSlots[s][0x104];
        for (int b = 0; b < DC_BUTTONS_PER_PAD; b++) {
            pressBase[b] = held[b] ? 0x80 : 0x00;
            if (held[b]) {
                if (b < 10) {
                    bits |= (unsigned short)slotCfg[b];
                }
                else if (b < JOY_CFG_MAX) {
                    bits |= (unsigned short)g_joystickConfigWords[b];
                }
            }
        }
        for (int b = DC_BUTTONS_PER_PAD; b < JOY_BUTTONS_PER_SLOT; b++) {
            pressBase[b] = 0x00;
        }

        joySlotState[s] = bits;
    }

    /* Clear unused slots. */
    for (int s = found; s < maxSlots; s++) {
        joySlotState[s] = 0;
    }
    for (int s = (found > MAX_GAMEPADS ? MAX_GAMEPADS : found);
         s < MAX_GAMEPADS; s++)
    {
        memset(&g_keyPressState[s * JOY_BUTTONS_PER_SLOT], 0,
               JOY_BUTTONS_PER_SLOT);
    }

    return found;
}

/* =====================================================================
 * Timing
 * ===================================================================== */

uint32_t platform_get_time_ms(void)
{
    /* KOS provides timer_ms_gettime; gettimeofday-based fallback is fine. */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    static uint64_t base_us = 0;
    uint64_t now_us = (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
    if (!base_us) {
        base_us = now_us;
    }
    return (uint32_t)((now_us - base_us) / 1000ULL);
}

void platform_sleep_ms(int ms)
{
    if (ms <= 0) {
        return;
    }
    thd_sleep(ms);
}

/* =====================================================================
 * Audio init — handled by music_dc.c / sfx_dc.c via snd_stream
 * ===================================================================== */

int platform_audio_init(void)
{
    return 0;
}

void platform_audio_shutdown(void)
{
    /* */ ;
}

/* =====================================================================
 * GL helpers — no-ops on DC. The PVR renderer doesn't need these.
 * ===================================================================== */

void platform_gl_swap(void)
{
    /* PVR auto-flips at scene_finish */
}

void platform_get_drawable_size(int *w, int *h)
{
    if (w) {
        *w = 640;
    }
    if (h) {
        *h = 480;
    }
}

/* SDL_GetTicks shim — main_dc.c may still reference it via platform.h-era
 * code paths. Keep it consistent with platform_get_time_ms. */
uint32_t SDL_GetTicks(void)
{
    return platform_get_time_ms();
}

/* =====================================================================
 * Network init — modem-first detect, BBA fallback.
 *
 * INIT_NET in KOS_INIT_FLAGS brings up the net stack and probes BBA
 * at boot. Here we check for a modem first (mutually exclusive with
 * BBA — same expansion slot), and fall back to BBA if no modem.
 * ===================================================================== */

static int s_netInitted = 0;
static int s_isModem    = 0;

int platform_net_init(void)
{
    if (s_netInitted) {
        return 0;
    }
#if 0
    /* Modem and BBA are mutually exclusive hardware — check modem first. */
    if (modem_init()) {
        fprintf(stderr, "net: modem detected, trying PPP\n");

        if (ppp_init() < 0) {
            fprintf(stderr, "net: ppp_init failed\n");
            modem_shutdown();
            return -1;
        }

        flashrom_ispcfg_t isp;
        memset(&isp, 0, sizeof(isp));

        const char *phone = "1111111";
        const char *user  = "jn64";
        const char *pass  = "password";

#if 0
        if (flashrom_get_ispcfg(&isp) == 0) {
            if (isp.valid_fields & FLASHROM_ISP_PHONE1)
                phone = isp.phone1;
            if (isp.valid_fields & FLASHROM_ISP_PPP_USER)
                user = isp.ppp_login;
            if (isp.valid_fields & FLASHROM_ISP_PPP_PASS)
                pass = isp.ppp_passwd;
            fprintf(stderr, "net: flashrom ISP config loaded (method=%d)\n", isp.method);
        } else if (flashrom_get_pw_ispcfg(&isp) == 0) {
            if (isp.valid_fields & FLASHROM_ISP_PHONE1)
                phone = isp.phone1;
            if (isp.valid_fields & FLASHROM_ISP_PPP_USER)
                user = isp.ppp_login;
            if (isp.valid_fields & FLASHROM_ISP_PPP_PASS)
                pass = isp.ppp_passwd;
            fprintf(stderr, "net: PlanetWeb ISP config loaded (method=%d)\n", isp.method);
        } else {
            fprintf(stderr, "net: no ISP config in flashrom, using defaults\n");
            phone = "1111111";
            user  = "jn64";
            pass  = "password";
        }

        if (!phone || phone[0] == '\0') {
            fprintf(stderr, "net: no phone number in ISP config\n");
            ppp_shutdown();
            return -1;
        }

        if (user && user[0] != '\0')
            ppp_set_login(user, (pass && pass[0]) ? pass : "");
#endif
        ppp_set_login("jn64", "password");

        int conn_rate = 0;
        thd_sleep(2500); 
        int dial_result = ppp_modem_init(phone, 1, &conn_rate);
        if (dial_result < 0) {
            fprintf(stderr, "net: ppp_modem_init failed (%d)\n", dial_result);
            ppp_shutdown();
            return -1;
        }
        fprintf(stderr, "net: modem connected at %d bps\n", conn_rate);

        if (ppp_connect() < 0) {
            fprintf(stderr, "net: ppp_connect failed\n");
            ppp_shutdown();
            return -1;
        }

        fprintf(stderr, "net: PPP link established\n");
        fs_socket_init();
        net_udp_init();
        net_tcp_init();
        fprintf(stderr, "net: socket/udp/tcp initialized\n");
        extern const char *g_cmdHostIP;
        g_cmdHostIP = "10.0.0.6";
        s_isModem = 1;
        s_netInitted = 1;
        return 0;
    }
#endif
#ifdef SONICR_PPP_SERIAL
    if (ppp_init() < 0) {
        fprintf(stderr, "net: ppp_init failed\n");
        return -1;
    }
    int err = ppp_scif_init(57600);
    if (err != 0) {
        fprintf(stderr, "net: SCIF init failed (%d)\n", err);
        return -1;
    }
    fs_socket_init();
    net_udp_init();
    net_tcp_init();
    err = ppp_connect();
    if (err != 0) {
        fprintf(stderr, "net: PPP connect failed (%d)\n", err);
        return -1;
    }
    fprintf(stderr, "net: PPP link established\n");
    extern const char *g_cmdHostIP;
    g_cmdHostIP = "10.0.0.6";
    s_isModem = 1;
    s_netInitted = 1;
    return 0;
#endif

    /* No modem — INIT_NET already probed BBA at boot. */
    if (net_default_dev != NULL) {
        fprintf(stderr, "net: BBA detected\n");
        s_isModem = 0;
        s_netInitted = 1;
        return 0;
    }
    else {
        fs_socket_init();
        net_udp_init();
        net_tcp_init();
        if (net_default_dev != NULL) {
            fprintf(stderr, "net: BBA detected\n");
            s_isModem = 0;
            s_netInitted = 1;
            return 0;
        }
    }

    fprintf(stderr, "net: no modem and no BBA\n");
    return -1;
}

void platform_net_shutdown(void)
{
    if (!s_netInitted) {
        return;
    }

    if (s_isModem) {
        ppp_shutdown();
    }

    s_netInitted = 0;
    s_isModem = 0;
}

int platform_net_is_modem(void)
{
    return s_isModem;
}

int platform_get_region(void)
{
    return flashrom_get_region();
}
