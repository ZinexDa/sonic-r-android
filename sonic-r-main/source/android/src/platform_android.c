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

extern void Music_SetMasterVolume(float vol);
extern float Music_GetMasterVolume(void);
extern void SFX_SetMasterVolume(float vol);
extern float SFX_GetMasterVolume(void);

SDL_Window *g_sdlWindow = NULL;
SDL_GLContext g_sdlGLContext = NULL;

unsigned char s_keystate[256];
unsigned char s_physicalKeystate[256];
static int s_quitRequested = 0;

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
    return 0;
}

int platform_poll_gamepads(unsigned short *joySlotState, int maxSlots)
{
    if (joySlotState && maxSlots > 0) {
        memset(joySlotState, 0, maxSlots * sizeof(unsigned short));
    }
    return 0;
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
    if (s_keystate[0x39]) m |= MENUBTN_A;     /* Button A (Confirm / Jump) -> F1 */
    if (s_keystate[0x1E]) m |= MENUBTN_B;     /* Button B (Back / Accel) */
    if (s_keystate[0x1C]) m |= MENUBTN_START; /* Start / Return -> F1 */
    if (s_keystate[0xC8]) m |= MENUBTN_UP;    /* Up -> F8 */
    if (s_keystate[0xD0]) m |= MENUBTN_DOWN;  /* Down -> F8 */
    if (s_keystate[0xCB]) m |= MENUBTN_LEFT;  /* Left -> F6 */
    if (s_keystate[0xCD]) m |= MENUBTN_RIGHT; /* Right -> F6 */
    if (s_keystate[0x2C]) m |= MENUBTN_L;     /* Drift L -> F7 */
    if (s_keystate[0x2D]) m |= MENUBTN_R;     /* Drift R -> F2 */
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


