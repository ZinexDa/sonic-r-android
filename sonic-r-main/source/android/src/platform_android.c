/**
 * platform_android.c — Android platform implementation via SDL2
 *
 * Implements platform.h for Android using SDL2 and OpenGL ES 2.0.
 */

#include <SDL.h>
#include <SDL_mixer.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>
#include "platform.h"
#include "touch_overlay.h"
#include "pad_bits.h"
#include "gamepad_buttons.h"

_Static_assert(GCBTN_A == SDL_CONTROLLER_BUTTON_A, "GCBTN_A != SDL");
_Static_assert(GCBTN_START == SDL_CONTROLLER_BUTTON_START, "GCBTN_START != SDL");
_Static_assert(GCBTN_LEFTSHOULDER == SDL_CONTROLLER_BUTTON_LEFTSHOULDER, "GCBTN_LEFTSHOULDER != SDL");
_Static_assert(GCBTN_DPAD_UP == SDL_CONTROLLER_BUTTON_DPAD_UP, "GCBTN_DPAD_UP != SDL");
_Static_assert(GCBTN_DPAD_RIGHT == SDL_CONTROLLER_BUTTON_DPAD_RIGHT, "GCBTN_DPAD_RIGHT != SDL");
_Static_assert(GCBTN_TRIGGER_LEFT > GCBTN_DPAD_RIGHT, "trigger indices overlap a mirrored SDL button");
_Static_assert(GC_BUTTON_COUNT == GCBTN_TRIGGER_RIGHT + 1, "GC_BUTTON_COUNT does not cover both triggers");

#define MAX_GAMEPADS 4
#define JOY_BUTTONS_PER_SLOT 80
#define TRIGGER_THRESHOLD_I 8192
#define STICK_DEADZONE_I 8192   /* 0.25 * 32767: responsive steering without stick drift */
#define JOY_CFG_MAX      32     /* g_joystickConfigWords array size */

typedef struct {
    SDL_GameController *ctrl;
    SDL_Joystick       *joy;
    SDL_JoystickID      id;
    int                 nButtons;
} GamepadSlot;

static GamepadSlot s_pads[MAX_GAMEPADS];
static int s_padCount = 0;

extern unsigned char g_keyPressState[320];
extern short g_joystickConfigWords[];
extern char g_joystickSlots[4][282];
extern char g_joystickDeviceNames[4][260];
extern short g_joystickDeviceFlags[8];
extern int g_initFeatureC;
extern void SyncJoystickSlots(void);

extern void Music_SetMasterVolume(float vol);
extern float Music_GetMasterVolume(void);
extern void SFX_SetMasterVolume(float vol);
extern float SFX_GetMasterVolume(void);

SDL_Window *g_sdlWindow = NULL;
SDL_GLContext g_sdlGLContext = NULL;

unsigned char s_keystate[256];
unsigned char s_physicalKeystate[256];
static int s_quitRequested = 0;

static void platform_publish_joystick_name(int slot, const char *name)
{
    if (slot < 0 || slot >= 4) {
        return;
    }
    char *dst = g_joystickDeviceNames[slot];
    if (name == NULL) {
        name = "Gamepad";
    }
    size_t n = strlen(name);
    if (n > 258) {
        n = 258;
    }
    memcpy(dst, name, n);
    dst[n] = '\0';
}

static void LoadGameControllerMappings(void)
{
    static int s_mappingsLoaded = 0;
    if (s_mappingsLoaded) {
        return;
    }
    int totalMappings = 0;

    /* 1. Try opening via SDL_RWops / Android AssetManager (packaged in APK assets) */
    SDL_RWops *rw = SDL_RWFromFile("gamecontrollerdb.txt", "rb");
    if (rw) {
        int n = SDL_GameControllerAddMappingsFromRW(rw, 1);
        if (n > 0) {
            SDL_Log("Loaded %d controller mappings from APK assets (gamecontrollerdb.txt)", n);
            totalMappings += n;
        } else {
            SDL_Log("SDL_GameControllerAddMappingsFromRW returned %d for APK assets", n);
        }
    } else {
        SDL_Log("gamecontrollerdb.txt not found in APK assets: %s", SDL_GetError());
    }

    /* 2. Try internal storage path */
    const char *internalPath = SDL_AndroidGetInternalStoragePath();
    if (internalPath && *internalPath) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/gamecontrollerdb.txt", internalPath);
        int n = SDL_GameControllerAddMappingsFromFile(path);
        if (n > 0) {
            SDL_Log("Loaded %d controller mappings from internal storage (%s)", n, path);
            totalMappings += n;
        }
    }

    /* 3. Try external storage path */
    const char *externalPath = SDL_AndroidGetExternalStoragePath();
    if (externalPath && *externalPath) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/gamecontrollerdb.txt", externalPath);
        int n = SDL_GameControllerAddMappingsFromFile(path);
        if (n > 0) {
            SDL_Log("Loaded %d controller mappings from external storage (%s)", n, path);
            totalMappings += n;
        }
    }

    /* 4. Try common storage / sdcard fallback paths */
    const char *extraPaths[] = {
        "/sdcard/Download/ssr/gamecontrollerdb.txt",
        "/sdcard/ssr/gamecontrollerdb.txt",
        NULL
    };
    for (int i = 0; extraPaths[i]; i++) {
        int n = SDL_GameControllerAddMappingsFromFile(extraPaths[i]);
        if (n > 0) {
            SDL_Log("Loaded %d controller mappings from %s", n, extraPaths[i]);
            totalMappings += n;
            break;
        }
    }

    SDL_Log("Total controller mappings registered: %d", totalMappings);
    s_mappingsLoaded = 1;
}

static int gamepad_open(int deviceIndex)
{
    if (s_padCount >= MAX_GAMEPADS) {
        return 0;
    }

    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(deviceIndex);
    if (id < 0) {
        return 0;
    }
    for (int i = 0; i < s_padCount; i++) {
        if (s_pads[i].id == id) {
            /* Upgrade raw joystick to game controller if mapping now available */
            if (!s_pads[i].ctrl && SDL_IsGameController(deviceIndex)) {
                SDL_GameController *ctrl = SDL_GameControllerOpen(deviceIndex);
                if (ctrl) {
                    if (s_pads[i].joy) {
                        SDL_JoystickClose(s_pads[i].joy);
                        s_pads[i].joy = NULL;
                    }
                    s_pads[i].ctrl = ctrl;
                    s_pads[i].nButtons = GC_BUTTON_COUNT;
                    const char *cname = SDL_GameControllerName(ctrl);
                    platform_publish_joystick_name(i, cname);
                    g_joystickDeviceFlags[i] = (short)(s_pads[i].nButtons < JOY_SLOT_CFG_WORDS
                                                       ? s_pads[i].nButtons : JOY_SLOT_CFG_WORDS);
                    SDL_Log("Gamepad slot %d upgraded to GameController: %s (%d buttons)",
                            i, cname ? cname : "unnamed", s_pads[i].nButtons);
                    return 1;
                }
            }
            return 0;
        }
    }

    GamepadSlot pad = { NULL, NULL, id, 0 };
    const char *name = NULL;

    if (SDL_IsGameController(deviceIndex)) {
        pad.ctrl = SDL_GameControllerOpen(deviceIndex);
        if (pad.ctrl) {
            pad.nButtons = GC_BUTTON_COUNT;
            name = SDL_GameControllerName(pad.ctrl);
        }
    }
    if (!pad.ctrl) {
        pad.joy = SDL_JoystickOpen(deviceIndex);
        if (!pad.joy) {
            return 0;
        }
        pad.nButtons = SDL_JoystickNumButtons(pad.joy);
        if (pad.nButtons > JOY_BUTTONS_PER_SLOT) {
            pad.nButtons = JOY_BUTTONS_PER_SLOT;
        }
        name = SDL_JoystickName(pad.joy);
    }

    int s = s_padCount;
    s_pads[s] = pad;

    platform_publish_joystick_name(s, name);
    g_joystickDeviceFlags[s] = (short)(pad.nButtons < JOY_SLOT_CFG_WORDS
                                       ? pad.nButtons : JOY_SLOT_CFG_WORDS);

    s_padCount++;
    g_initFeatureC = s_padCount;

    SDL_Log("Gamepad slot %d connected: %s (%s, %d buttons, ID %d)",
            s, name ? name : "unnamed",
            pad.ctrl ? "mapped controller" : "raw joystick", pad.nButtons, (int)id);
    return 1;
}

static void gamepad_close(int slot)
{
    if (s_pads[slot].ctrl) {
        SDL_GameControllerClose(s_pads[slot].ctrl);
    } else if (s_pads[slot].joy) {
        SDL_JoystickClose(s_pads[slot].joy);
    }
    s_pads[slot].ctrl     = NULL;
    s_pads[slot].joy      = NULL;
    s_pads[slot].id       = -1;
    s_pads[slot].nButtons = 0;
}

static void gamepad_remove_by_id(SDL_JoystickID jid)
{
    for (int i = 0; i < s_padCount; i++) {
        if (s_pads[i].id != jid) {
            continue;
        }
        SDL_Log("Gamepad slot %d disconnected (ID %d)", i, (int)jid);
        gamepad_close(i);
        /* Clear pressed state for removed slot */
        memset(&g_keyPressState[i * JOY_BUTTONS_PER_SLOT], 0, JOY_BUTTONS_PER_SLOT);
        /* Shift remaining slots down */
        for (int j = i; j < s_padCount - 1; j++) {
            s_pads[j] = s_pads[j + 1];
        }
        s_pads[s_padCount - 1].ctrl     = NULL;
        s_pads[s_padCount - 1].joy      = NULL;
        s_pads[s_padCount - 1].id       = -1;
        s_pads[s_padCount - 1].nButtons = 0;
        s_padCount--;
        g_initFeatureC = s_padCount;
        SyncJoystickSlots();
        break;
    }
}

static int gamepad_button_held(const GamepadSlot *pad, int b)
{
    if (pad->ctrl) {
        if (b == GCBTN_TRIGGER_LEFT) {
            return SDL_GameControllerGetAxis(pad->ctrl,
                       SDL_CONTROLLER_AXIS_TRIGGERLEFT) > TRIGGER_THRESHOLD_I;
        }
        if (b == GCBTN_TRIGGER_RIGHT) {
            return SDL_GameControllerGetAxis(pad->ctrl,
                       SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_THRESHOLD_I;
        }
        return SDL_GameControllerGetButton(pad->ctrl,
                   (SDL_GameControllerButton)b) != 0;
    }
    return SDL_JoystickGetButton(pad->joy, b) != 0;
}

static unsigned char SDLScancodeToDIK(SDL_Scancode sc)
{
    switch (sc) {
        case SDL_SCANCODE_A: return 0x1E; /* DIK_A */
        case SDL_SCANCODE_B: return 0x30; /* DIK_B */
        case SDL_SCANCODE_C: return 0x2E; /* DIK_C */
        case SDL_SCANCODE_D: return 0x20; /* DIK_D */
        case SDL_SCANCODE_E: return 0x12; /* DIK_E */
        case SDL_SCANCODE_F: return 0x21; /* DIK_F */
        case SDL_SCANCODE_G: return 0x22; /* DIK_G */
        case SDL_SCANCODE_H: return 0x23; /* DIK_H */
        case SDL_SCANCODE_I: return 0x17; /* DIK_I */
        case SDL_SCANCODE_J: return 0x24; /* DIK_J */
        case SDL_SCANCODE_K: return 0x25; /* DIK_K */
        case SDL_SCANCODE_L: return 0x26; /* DIK_L */
        case SDL_SCANCODE_M: return 0x32; /* DIK_M */
        case SDL_SCANCODE_N: return 0x31; /* DIK_N */
        case SDL_SCANCODE_O: return 0x18; /* DIK_O */
        case SDL_SCANCODE_P: return 0x19; /* DIK_P */
        case SDL_SCANCODE_Q: return 0x10; /* DIK_Q */
        case SDL_SCANCODE_R: return 0x13; /* DIK_R */
        case SDL_SCANCODE_S: return 0x1F; /* DIK_S */
        case SDL_SCANCODE_T: return 0x14; /* DIK_T */
        case SDL_SCANCODE_U: return 0x16; /* DIK_U */
        case SDL_SCANCODE_V: return 0x2F; /* DIK_V */
        case SDL_SCANCODE_W: return 0x11; /* DIK_W */
        case SDL_SCANCODE_X: return 0x2D; /* DIK_X */
        case SDL_SCANCODE_Y: return 0x15; /* DIK_Y */
        case SDL_SCANCODE_Z: return 0x2C; /* DIK_Z */
        case SDL_SCANCODE_1: return 0x02; /* DIK_1 */
        case SDL_SCANCODE_2: return 0x03; /* DIK_2 */
        case SDL_SCANCODE_3: return 0x04; /* DIK_3 */
        case SDL_SCANCODE_4: return 0x05; /* DIK_4 */
        case SDL_SCANCODE_5: return 0x06; /* DIK_5 */
        case SDL_SCANCODE_6: return 0x07; /* DIK_6 */
        case SDL_SCANCODE_7: return 0x08; /* DIK_7 */
        case SDL_SCANCODE_8: return 0x09; /* DIK_8 */
        case SDL_SCANCODE_9: return 0x0A; /* DIK_9 */
        case SDL_SCANCODE_0: return 0x0B; /* DIK_0 */
        case SDL_SCANCODE_RETURN: return 0x1C; /* DIK_RETURN */
        case SDL_SCANCODE_ESCAPE: return 0x01; /* DIK_ESCAPE */
        case SDL_SCANCODE_AC_BACK: return 0x01; /* DIK_ESCAPE */
        case SDL_SCANCODE_BACKSPACE: return 0x0E; /* DIK_BACK */
        case SDL_SCANCODE_TAB: return 0x0F; /* DIK_TAB */
        case SDL_SCANCODE_SPACE: return 0x39; /* DIK_SPACE */
        case SDL_SCANCODE_LEFT: return 0xCB; /* DIK_LEFT */
        case SDL_SCANCODE_RIGHT: return 0xCD; /* DIK_RIGHT */
        case SDL_SCANCODE_UP: return 0xC8; /* DIK_UP */
        case SDL_SCANCODE_DOWN: return 0xD0; /* DIK_DOWN */
        case SDL_SCANCODE_LSHIFT: return 0x2A; /* DIK_LSHIFT */
        case SDL_SCANCODE_RSHIFT: return 0x36; /* DIK_RSHIFT */
        case SDL_SCANCODE_LCTRL: return 0x1D; /* DIK_LCONTROL */
        case SDL_SCANCODE_RCTRL: return 0x9D; /* DIK_RCONTROL */
        case SDL_SCANCODE_LALT: return 0x38; /* DIK_LALT */
        case SDL_SCANCODE_RALT: return 0xB8; /* DIK_RMENU */
        default: return 0;
    }
}

/* =====================================================================
 * Display
 * ===================================================================== */

int platform_init(int width, int height, int fullscreen, const char *title)
{
    (void)fullscreen;
    memset(s_keystate, 0, sizeof(s_keystate));
    memset(s_physicalKeystate, 0, sizeof(s_physicalKeystate));
    s_quitRequested = 0;

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return -1;
    }

    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    }
    LoadGameControllerMappings();

    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        SDL_Log("Mix_OpenAudio failed: %s", Mix_GetError());
    } else {
        int freq = 0;
        Uint16 fmt = 0;
        int ch = 0;
        if (Mix_QuerySpec(&freq, &fmt, &ch)) {
            SDL_Log("SDL_mixer audio opened: %d Hz, format 0x%x, %d channels", freq, fmt, ch);
        } else {
            SDL_Log("SDL_mixer audio opened (44100 Hz, stereo)");
        }
        Music_SetMasterVolume(Music_GetMasterVolume());
        SFX_SetMasterVolume(SFX_GetMasterVolume());
    }

    TouchOverlay_Init();

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    Uint32 winFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    g_sdlWindow = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        width, height,
        winFlags
    );
    if (!g_sdlWindow) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return -1;
    }

    g_sdlGLContext = SDL_GL_CreateContext(g_sdlWindow);
    if (!g_sdlGLContext) {
        SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return -1;
    }

    SDL_GL_MakeCurrent(g_sdlWindow, g_sdlGLContext);
    SDL_GL_SetSwapInterval(1);

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_sdlWindow, &dw, &dh);
    SDL_Log("Sonic R Android platform initialized successfully (drawable: %dx%d)", dw, dh);
    TouchOverlay_TestMultiTouch();

    return 0;
}

void platform_shutdown(void)
{
    Mix_CloseAudio();
    Mix_Quit();
    TouchOverlay_Reset();
    for (int i = 0; i < s_padCount; i++) {
        gamepad_close(i);
    }
    s_padCount = 0;
    g_initFeatureC = 0;
    if (g_sdlGLContext) {
        SDL_GL_DeleteContext(g_sdlGLContext);
        g_sdlGLContext = NULL;
    }
    if (g_sdlWindow) {
        SDL_DestroyWindow(g_sdlWindow);
        g_sdlWindow = NULL;
    }
    SDL_Quit();
}

const char *platform_base_path(void)
{
    static char s_base[512];
    const char *internalPath = SDL_AndroidGetInternalStoragePath();
    if (internalPath) {
        strncpy(s_base, internalPath, sizeof(s_base) - 1);
        s_base[sizeof(s_base) - 1] = '\0';
        return s_base;
    }
    return NULL;
}

/* =====================================================================
 * Input
 * ===================================================================== */

static void HandleSDLEvent(SDL_Event *event)
{
    TouchOverlay_HandleEvent(event);

    switch (event->type) {
        case SDL_QUIT:
        case SDL_APP_TERMINATING:
            s_quitRequested = 1;
            break;

        case SDL_APP_WILLENTERBACKGROUND:
            TouchOverlay_Reset();
            memset(s_physicalKeystate, 0, sizeof(s_physicalKeystate));
            break;

        case SDL_WINDOWEVENT:
            if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                TouchOverlay_Reset();
                memset(s_physicalKeystate, 0, sizeof(s_physicalKeystate));
            }
            break;

        case SDL_KEYDOWN: {
            unsigned char dik = SDLScancodeToDIK(event->key.keysym.scancode);
            if (dik) {
                s_physicalKeystate[dik] = 0x80;
            }
            break;
        }

        case SDL_KEYUP: {
            unsigned char dik = SDLScancodeToDIK(event->key.keysym.scancode);
            if (dik) {
                s_physicalKeystate[dik] = 0x00;
            }
            break;
        }

        case SDL_CONTROLLERDEVICEADDED: {
            if (gamepad_open(event->cdevice.which)) {
                SyncJoystickSlots();
            }
            break;
        }

        case SDL_CONTROLLERDEVICEREMOVED: {
            gamepad_remove_by_id(event->cdevice.which);
            break;
        }

        case SDL_JOYDEVICEADDED: {
            if (gamepad_open(event->jdevice.which)) {
                SyncJoystickSlots();
            }
            break;
        }

        case SDL_JOYDEVICEREMOVED: {
            gamepad_remove_by_id(event->jdevice.which);
            break;
        }

        default:
            break;
    }
}

int platform_poll_events(unsigned char *keystateOut, int keystateSize)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        HandleSDLEvent(&event);
    }

    TouchOverlay_Update(s_keystate);

    if (keystateOut && keystateSize > 0) {
        int n = keystateSize < (int)sizeof(s_keystate) ? keystateSize : (int)sizeof(s_keystate);
        memcpy(keystateOut, s_keystate, n);
    }
    return s_quitRequested ? 1 : 0;
}

void platform_pump_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        HandleSDLEvent(&event);
    }
    TouchOverlay_Update(s_keystate);
}

int platform_init_gamepads(void)
{
    LoadGameControllerMappings();
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n && s_padCount < MAX_GAMEPADS; i++) {
        gamepad_open(i);
    }
    SyncJoystickSlots();
    return s_padCount;
}

int platform_poll_gamepads(unsigned short *joySlotState, int maxSlots)
{
    int count = s_padCount;
    if (count > maxSlots) {
        count = maxSlots;
    }

    for (int i = 0; i < count; i++) {
        const GamepadSlot *pad = &s_pads[i];
        unsigned char *pressBase = &g_keyPressState[i * JOY_BUTTONS_PER_SLOT];

        if (!pad->ctrl && !pad->joy) {
            joySlotState[i] = 0;
            memset(pressBase, 0, JOY_BUTTONS_PER_SLOT);
            continue;
        }

        unsigned short bits = 0;

        /* Hat (D-pad) — raw path only, first hat only */
        if (pad->joy && SDL_JoystickNumHats(pad->joy) > 0) {
            Uint8 hat = SDL_JoystickGetHat(pad->joy, 0);
            if (hat & SDL_HAT_LEFT) {
                bits |= PAD_LEFT;
            }
            if (hat & SDL_HAT_RIGHT) {
                bits |= PAD_RIGHT;
            }
            if (hat & SDL_HAT_UP) {
                bits |= PAD_UP;
            }
            if (hat & SDL_HAT_DOWN) {
                bits |= PAD_DOWN;
            }
        }

        /* Left analog stick — digital threshold. Negative = left/up, positive = right/down. */
        int haveStick = 1;
        Sint16 lx = 0, ly = 0;
        if (pad->ctrl) {
            lx = SDL_GameControllerGetAxis(pad->ctrl, SDL_CONTROLLER_AXIS_LEFTX);
            ly = SDL_GameControllerGetAxis(pad->ctrl, SDL_CONTROLLER_AXIS_LEFTY);
        } else if (SDL_JoystickNumAxes(pad->joy) >= 2) {
            lx = SDL_JoystickGetAxis(pad->joy, 0);
            ly = SDL_JoystickGetAxis(pad->joy, 1);
        } else {
            haveStick = 0;
        }
        if (haveStick) {
            if (lx < -STICK_DEADZONE_I) {
                bits |= PAD_LEFT;
            } else if (lx > STICK_DEADZONE_I) {
                bits |= PAD_RIGHT;
            }
            if (ly < -STICK_DEADZONE_I) {
                bits |= PAD_UP;
            } else if (ly > STICK_DEADZONE_I) {
                bits |= PAD_DOWN;
            }
        }

        /* Buttons — populate g_keyPressState (so ScanKeyRemap can scan during
         * the remap UI), then OR in the bit pattern for each held button. */
        const short *slotCfg = (const short *)&g_joystickSlots[i][0x104];
        for (int b = 0; b < pad->nButtons; b++) {
            int held = gamepad_button_held(pad, b);
            pressBase[b] = held ? 0x80 : 0x00;
            if (!held) {
                continue;
            }
            short cfg = 0;
            if (b < JOY_SLOT_CFG_WORDS) {
                cfg = slotCfg[b];
            }
            if (cfg == 0 && b < JOY_CFG_MAX) {
                cfg = g_joystickConfigWords[b];
            }
            if (pad->joy) {
                /* Raw device: directions came from the hat/stick above, and this
                 * table is not laid out for this device's indices. */
                cfg &= (short)~PAD_DIRECTIONS;
            }
            bits |= (unsigned short)cfg;
        }
        /* Zero any trailing slots that this device doesn't have. */
        for (int b = pad->nButtons; b < JOY_BUTTONS_PER_SLOT; b++) {
            pressBase[b] = 0x00;
        }

        joySlotState[i] = bits;
    }

    /* Clear unused slot state and output. */
    for (int i = count; i < maxSlots; i++) {
        joySlotState[i] = 0;
    }
    for (int i = count; i < MAX_GAMEPADS; i++) {
        memset(&g_keyPressState[i * JOY_BUTTONS_PER_SLOT], 0,
               JOY_BUTTONS_PER_SLOT);
    }

    return count;
}

/* =====================================================================
 * Timing
 * ===================================================================== */

uint32_t platform_get_time_ms(void)
{
    return (uint32_t)SDL_GetTicks();
}

void platform_sleep_ms(int ms)
{
    if (ms > 0) {
        SDL_Delay((Uint32)ms);
    }
}

/* =====================================================================
 * Audio
 * ===================================================================== */

int platform_audio_init(void)
{
    return 0;
}

void platform_audio_shutdown(void)
{
}

/* =====================================================================
 * GL helpers
 * ===================================================================== */

void platform_gl_swap(void)
{
    if (g_sdlWindow) {
        SDL_GL_SwapWindow(g_sdlWindow);
    }
}

void platform_get_drawable_size(int *w, int *h)
{
    if (g_sdlWindow) {
        SDL_GL_GetDrawableSize(g_sdlWindow, w, h);
    } else {
        if (w) *w = 640;
        if (h) *h = 480;
    }
}

/* =====================================================================
 * Networking
 * ===================================================================== */

int platform_net_init(void)
{
    return 0;
}

void platform_net_shutdown(void)
{
}

int platform_net_is_modem(void)
{
    return 0;
}

int platform_get_region(void)
{
    return 0;
}

/* =====================================================================
 * JNI Volume Controls (called from GameActivity.kt)
 * ===================================================================== */

JNIEXPORT void JNICALL
Java_org_sonicr_android_GameActivity_nativeSetMusicVolume(JNIEnv *env, jclass clazz, jfloat volume)
{
    (void)env;
    (void)clazz;
    Music_SetMasterVolume((float)volume);
}

JNIEXPORT void JNICALL
Java_org_sonicr_android_GameActivity_nativeSetSfxVolume(JNIEnv *env, jclass clazz, jfloat volume)
{
    (void)env;
    (void)clazz;
    SFX_SetMasterVolume((float)volume);
}

JNIEXPORT void JNICALL
Java_org_sonicr_android_GameActivity_nativeSetControlLayout(
    JNIEnv *env, jclass clazz,
    jboolean customEnabled,
    jfloat dpadX, jfloat dpadY, jfloat dpadScale,
    jfloat driftLX, jfloat driftLY, jfloat driftLScale,
    jfloat accelX, jfloat accelY, jfloat accelScale,
    jfloat jumpX, jfloat jumpY, jfloat jumpScale,
    jfloat driftRX, jfloat driftRY, jfloat driftRScale,
    jfloat lookX, jfloat lookY, jfloat lookScale,
    jfloat startX, jfloat startY, jfloat startScale)
{
    (void)env;
    (void)clazz;
    if (customEnabled) {
        TouchOverlay_SetCustomLayout(
            1,
            (float)dpadX, (float)dpadY, (float)dpadScale,
            (float)driftLX, (float)driftLY, (float)driftLScale,
            (float)accelX, (float)accelY, (float)accelScale,
            (float)jumpX, (float)jumpY, (float)jumpScale,
            (float)driftRX, (float)driftRY, (float)driftRScale,
            (float)lookX, (float)lookY, (float)lookScale,
            (float)startX, (float)startY, (float)startScale
        );
    } else {
        TouchOverlay_ResetCustomLayout();
    }
}

/* =====================================================================
 * Menu / Lobby Controller Buttons (F-key synthesis)
 * ===================================================================== */

unsigned int platform_menu_buttons(void)
{
    unsigned int m = 0;
    if (s_keystate[0x39] || s_keystate[0x3B]) m |= MENUBTN_A;     /* Button A or F1 -> F1 */
    if (s_keystate[0x1E])                     m |= MENUBTN_B;     /* Button B (Back / Accel) */
    if (s_keystate[0x1C])                     m |= MENUBTN_START; /* Start / Return -> F1 */
    if (s_keystate[0xC8] || s_keystate[0x42]) m |= MENUBTN_UP;    /* Up or F8 -> F8 */
    if (s_keystate[0xD0])                     m |= MENUBTN_DOWN;  /* Down -> F8 */
    if (s_keystate[0xCB] || s_keystate[0x40]) m |= MENUBTN_LEFT;  /* Left or F6 -> F6 */
    if (s_keystate[0xCD])                     m |= MENUBTN_RIGHT; /* Right -> F6 */
    if (s_keystate[0x2C] || s_keystate[0x41]) m |= MENUBTN_L;     /* Drift L or F7 -> F7 */
    if (s_keystate[0x2D] || s_keystate[0x3C]) m |= MENUBTN_R;     /* Drift R or F2 -> F2 */
    return m;
}

/* =====================================================================
 * JNI Netplay Mode Config (called from GameActivity.kt)
 * ===================================================================== */

extern void Engine_SetNetplayAutoMode(int isHost, const char *hostIp, int port);

JNIEXPORT void JNICALL
Java_org_sonicr_android_GameActivity_nativeSetNetplayMode(
    JNIEnv *env, jclass clazz,
    jboolean isNetplay, jboolean isHost, jstring hostIp, jint port)
{
    (void)clazz;
    if (isNetplay) {
        const char *ipStr = hostIp ? (*env)->GetStringUTFChars(env, hostIp, NULL) : NULL;
        Engine_SetNetplayAutoMode(isHost ? 1 : 0, ipStr ? ipStr : "127.0.0.1", (int)port);
        if (ipStr && hostIp) {
            (*env)->ReleaseStringUTFChars(env, hostIp, ipStr);
        }
    }
}

/* =====================================================================
 * JNI Settings Persistence (called on Android lifecycle pause/destroy)
 * ===================================================================== */

extern void SaveGameSettings(void);
extern void SavePadTypesImpl(void);
extern void SaveKeyMappings(void);

JNIEXPORT void JNICALL
Java_org_sonicr_android_GameActivity_nativeSaveSettings(JNIEnv *env, jclass clazz)
{
    (void)env;
    (void)clazz;
    SaveGameSettings();
    SavePadTypesImpl();
    SaveKeyMappings();
    SDL_Log("nativeSaveSettings: saved SONICR.INF, JOYSTICK.INF, KEYS.BIN");
}



