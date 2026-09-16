/**
 * touch_overlay.c — On-screen touch controls overlay for Sonic R Android
 *
 * Implements virtual 8-way D-pad and action buttons (Accel, Jump, Drift, Look, Start)
 * mapped to DirectInput keyboard scancodes. Renders via GLES2 directly into the window.
 */

#include "touch_overlay.h"
#include "platform.h"
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

/* External physical keystate tracked by platform_android.c */
extern unsigned char s_physicalKeystate[256];

/* =====================================================================
 * Multi-Touch State Tracking
 * ===================================================================== */

#define MAX_TOUCHES 10

typedef struct {
    SDL_FingerID id;
    float x; /* normalized [0.0, 1.0] */
    float y; /* normalized [0.0, 1.0] */
    int active;
    int pendingRelease;
} ActiveTouch;

static ActiveTouch s_touches[MAX_TOUCHES];
static unsigned char s_touchKeystate[256];

/* Current button pressed state for visual rendering */
typedef struct {
    int up;
    int down;
    int left;
    int right;
    int accel;
    int jump;
    int driftL;
    int driftR;
    int look;
    int start;
} TouchButtonState;

static TouchButtonState s_pressed;

/* Layout configuration computed from screen dimensions */
typedef struct {
    float dpad_cx, dpad_cy, dpad_radius, dpad_deadzone;
    float accel_cx, accel_cy, accel_radius;
    float jump_cx, jump_cy, jump_radius;
    float driftL_cx, driftL_cy, driftL_radius;
    float driftR_cx, driftR_cy, driftR_radius;
    float look_cx, look_cy, look_radius;
    float start_cx, start_cy, start_w, start_h;
} TouchLayout;

/* Custom layout configuration persisted from Android settings (7 independent controls) */
typedef struct {
    int enabled;
    float dpad_x, dpad_y, dpad_scale;
    float driftL_x, driftL_y, driftL_scale;
    float accel_x, accel_y, accel_scale;
    float jump_x, jump_y, jump_scale;
    float driftR_x, driftR_y, driftR_scale;
    float look_x, look_y, look_scale;
    float start_x, start_y, start_scale;
} CustomControlLayout;

static CustomControlLayout s_customLayout = {0};

static float ClampScale(float s) {
    if (s < 0.70f) return 0.70f;
    if (s > 1.40f) return 1.40f;
    return s;
}

void TouchOverlay_SetCustomLayout(int enabled,
                                  float dpadX, float dpadY, float dpadScale,
                                  float driftLX, float driftLY, float driftLScale,
                                  float accelX, float accelY, float accelScale,
                                  float jumpX, float jumpY, float jumpScale,
                                  float driftRX, float driftRY, float driftRScale,
                                  float lookX, float lookY, float lookScale,
                                  float startX, float startY, float startScale)
{
    s_customLayout.enabled = enabled ? 1 : 0;
    s_customLayout.dpad_x = dpadX;
    s_customLayout.dpad_y = dpadY;
    s_customLayout.dpad_scale = ClampScale(dpadScale);

    s_customLayout.driftL_x = driftLX;
    s_customLayout.driftL_y = driftLY;
    s_customLayout.driftL_scale = ClampScale(driftLScale);

    s_customLayout.accel_x = accelX;
    s_customLayout.accel_y = accelY;
    s_customLayout.accel_scale = ClampScale(accelScale);

    s_customLayout.jump_x = jumpX;
    s_customLayout.jump_y = jumpY;
    s_customLayout.jump_scale = ClampScale(jumpScale);

    s_customLayout.driftR_x = driftRX;
    s_customLayout.driftR_y = driftRY;
    s_customLayout.driftR_scale = ClampScale(driftRScale);

    s_customLayout.look_x = lookX;
    s_customLayout.look_y = lookY;
    s_customLayout.look_scale = ClampScale(lookScale);

    s_customLayout.start_x = startX;
    s_customLayout.start_y = startY;
    s_customLayout.start_scale = ClampScale(startScale);

    SDL_Log("TouchOverlay: Custom 7-control layout applied (enabled=%d): "
            "DPad(%.3f,%.3f,s=%.2f) DL(%.3f,%.3f,s=%.2f) A(%.3f,%.3f,s=%.2f) "
            "B(%.3f,%.3f,s=%.2f) DR(%.3f,%.3f,s=%.2f) Eye(%.3f,%.3f,s=%.2f) "
            "Start(%.3f,%.3f,s=%.2f)",
            s_customLayout.enabled,
            s_customLayout.dpad_x, s_customLayout.dpad_y, s_customLayout.dpad_scale,
            s_customLayout.driftL_x, s_customLayout.driftL_y, s_customLayout.driftL_scale,
            s_customLayout.accel_x, s_customLayout.accel_y, s_customLayout.accel_scale,
            s_customLayout.jump_x, s_customLayout.jump_y, s_customLayout.jump_scale,
            s_customLayout.driftR_x, s_customLayout.driftR_y, s_customLayout.driftR_scale,
            s_customLayout.look_x, s_customLayout.look_y, s_customLayout.look_scale,
            s_customLayout.start_x, s_customLayout.start_y, s_customLayout.start_scale);
}

void TouchOverlay_ResetCustomLayout(void)
{
    memset(&s_customLayout, 0, sizeof(s_customLayout));
    SDL_Log("TouchOverlay: Custom layout reset to default automatic layout");
}

static void ComputeLayout(int screenW, int screenH, TouchLayout *out)
{
    float W = (float)screenW;
    float H = (float)screenH;
    float pad = H * 0.02f;

    if (s_customLayout.enabled) {
        /* -------------------------------------------------------------
         * Custom Layout Mode: 7 Fully Independent Controls
         * ------------------------------------------------------------- */
        /* 1. D-Pad */
        float dpad_r = H * 0.19f * s_customLayout.dpad_scale;
        float deadzone = H * 0.045f * s_customLayout.dpad_scale;
        float dpad_cx = s_customLayout.dpad_x * W;
        float dpad_cy = s_customLayout.dpad_y * H;
        if (dpad_cx < dpad_r + pad) dpad_cx = dpad_r + pad;
        if (dpad_cx > W - dpad_r - pad) dpad_cx = W - dpad_r - pad;
        if (dpad_cy < dpad_r + pad) dpad_cy = dpad_r + pad;
        if (dpad_cy > H - dpad_r - pad) dpad_cy = H - dpad_r - pad;
        out->dpad_cx = dpad_cx;
        out->dpad_cy = dpad_cy;
        out->dpad_radius = dpad_r;
        out->dpad_deadzone = deadzone;

        /* 2. Drift Left */
        float driftL_r = H * 0.068f * s_customLayout.driftL_scale;
        float driftL_cx = s_customLayout.driftL_x * W;
        float driftL_cy = s_customLayout.driftL_y * H;
        if (driftL_cx < driftL_r + pad) driftL_cx = driftL_r + pad;
        if (driftL_cx > W - driftL_r - pad) driftL_cx = W - driftL_r - pad;
        if (driftL_cy < driftL_r + pad) driftL_cy = driftL_r + pad;
        if (driftL_cy > H - driftL_r - pad) driftL_cy = H - driftL_r - pad;
        out->driftL_cx = driftL_cx;
        out->driftL_cy = driftL_cy;
        out->driftL_radius = driftL_r;

        /* 3. Accel (A) */
        float accel_r = H * 0.095f * s_customLayout.accel_scale;
        float accel_cx = s_customLayout.accel_x * W;
        float accel_cy = s_customLayout.accel_y * H;
        if (accel_cx < accel_r + pad) accel_cx = accel_r + pad;
        if (accel_cx > W - accel_r - pad) accel_cx = W - accel_r - pad;
        if (accel_cy < accel_r + pad) accel_cy = accel_r + pad;
        if (accel_cy > H - accel_r - pad) accel_cy = H - accel_r - pad;
        out->accel_cx = accel_cx;
        out->accel_cy = accel_cy;
        out->accel_radius = accel_r;

        /* 4. Jump (B) */
        float jump_r = H * 0.088f * s_customLayout.jump_scale;
        float jump_cx = s_customLayout.jump_x * W;
        float jump_cy = s_customLayout.jump_y * H;
        if (jump_cx < jump_r + pad) jump_cx = jump_r + pad;
        if (jump_cx > W - jump_r - pad) jump_cx = W - jump_r - pad;
        if (jump_cy < jump_r + pad) jump_cy = jump_r + pad;
        if (jump_cy > H - jump_r - pad) jump_cy = H - jump_r - pad;
        out->jump_cx = jump_cx;
        out->jump_cy = jump_cy;
        out->jump_radius = jump_r;

        /* 5. Drift Right (R) */
        float driftR_r = H * 0.068f * s_customLayout.driftR_scale;
        float driftR_cx = s_customLayout.driftR_x * W;
        float driftR_cy = s_customLayout.driftR_y * H;
        if (driftR_cx < driftR_r + pad) driftR_cx = driftR_r + pad;
        if (driftR_cx > W - driftR_r - pad) driftR_cx = W - driftR_r - pad;
        if (driftR_cy < driftR_r + pad) driftR_cy = driftR_r + pad;
        if (driftR_cy > H - driftR_r - pad) driftR_cy = H - driftR_r - pad;
        out->driftR_cx = driftR_cx;
        out->driftR_cy = driftR_cy;
        out->driftR_radius = driftR_r;

        /* 6. Look Back (Eye) */
        float look_r = H * 0.068f * s_customLayout.look_scale;
        float look_cx = s_customLayout.look_x * W;
        float look_cy = s_customLayout.look_y * H;
        if (look_cx < look_r + pad) look_cx = look_r + pad;
        if (look_cx > W - look_r - pad) look_cx = W - look_r - pad;
        if (look_cy < look_r + pad) look_cy = look_r + pad;
        if (look_cy > H - look_r - pad) look_cy = H - look_r - pad;
        out->look_cx = look_cx;
        out->look_cy = look_cy;
        out->look_radius = look_r;

        /* 7. Start / Pause */
        float start_w = H * 0.18f * s_customLayout.start_scale;
        float start_h = H * 0.075f * s_customLayout.start_scale;
        float start_cx = s_customLayout.start_x * W;
        float start_cy = s_customLayout.start_y * H;
        float halfW = (start_w + start_h) * 0.5f;
        float halfH = start_h * 0.5f;
        if (start_cx < halfW + pad) start_cx = halfW + pad;
        if (start_cx > W - halfW - pad) start_cx = W - halfW - pad;
        if (start_cy < halfH + pad) start_cy = halfH + pad;
        if (start_cy > H - halfH - pad) start_cy = H - halfH - pad;
        out->start_cx = start_cx;
        out->start_cy = start_cy;
        out->start_w = start_w;
        out->start_h = start_h;
    } else {
        /* -------------------------------------------------------------
         * Default Automatic Pillarbox Layout
         * ------------------------------------------------------------- */
        /* Compute 4:3 pillarbox margins */
        float gameW = H * (4.0f / 3.0f);
        float margin = (W > gameW) ? ((W - gameW) * 0.5f) : 0.0f;

        /* Left anchor: prefer the pillarbox margin if wide enough, otherwise left edge */
        float left_anchor_x;
        if (margin >= H * 0.25f) {
            left_anchor_x = margin * 0.52f;
        } else {
            left_anchor_x = H * 0.22f;
        }
        if (left_anchor_x < H * 0.18f) {
            left_anchor_x = H * 0.18f;
        }

        float right_anchor_x = W - left_anchor_x;

        /* D-Pad */
        out->dpad_cx = left_anchor_x;
        out->dpad_cy = H * 0.73f;
        out->dpad_radius = H * 0.19f;
        out->dpad_deadzone = H * 0.045f;

        /* Drift Left (above D-pad) */
        out->driftL_cx = left_anchor_x;
        out->driftL_cy = out->dpad_cy - out->dpad_radius - H * 0.075f;
        out->driftL_radius = H * 0.068f;

        /* Action Buttons (Right Thumb Cluster) */
        out->accel_cx = right_anchor_x + H * 0.035f;
        out->accel_cy = H * 0.71f;
        out->accel_radius = H * 0.095f;

        out->jump_cx = right_anchor_x - H * 0.145f;
        out->jump_cy = H * 0.81f;
        out->jump_radius = H * 0.088f;

        out->driftR_cx = out->accel_cx;
        out->driftR_cy = out->accel_cy - H * 0.18f;
        out->driftR_radius = H * 0.068f;

        out->look_cx = out->jump_cx;
        out->look_cy = out->jump_cy - H * 0.18f;
        out->look_radius = H * 0.068f;

        /* Start / Pause Button (Top-Right) */
        out->start_cx = W - H * 0.16f;
        out->start_cy = H * 0.09f;
        out->start_w  = H * 0.18f;
        out->start_h  = H * 0.075f;
    }
}

/* =====================================================================
 * Hit Testing
 * ===================================================================== */

static int HitTestCircle(float px, float py, float cx, float cy, float r)
{
    float dx = px - cx;
    float dy = py - cy;
    float rHit = r * 1.30f; /* 30% generous touch padding */
    return (dx * dx + dy * dy) <= (rHit * rHit);
}

static int HitTestPill(float px, float py, float cx, float cy, float halfW, float halfH)
{
    float pad = halfH * 0.6f;
    return (px >= (cx - halfW - pad) && px <= (cx + halfW + pad) &&
            py >= (cy - halfH - pad) && py <= (cy + halfH + pad));
}

static void HitTestDpad(float px, float py, float cx, float cy, float r, float deadzone,
                        int *outUp, int *outDown, int *outLeft, int *outRight)
{
    float dx = px - cx;
    float dy = py - cy;
    float distSq = dx * dx + dy * dy;
    float rHit = r * 1.35f;

    if (distSq < (deadzone * deadzone) || distSq > (rHit * rHit)) {
        return;
    }

    float angle = atan2f(dy, dx);
    float deg = angle * (180.0f / (float)M_PI); /* -180 to +180 */

    if (deg >= -22.5f && deg <= 22.5f) {
        *outRight = 1;
    } else if (deg > 22.5f && deg < 67.5f) {
        *outDown = 1;
        *outRight = 1;
    } else if (deg >= 67.5f && deg <= 112.5f) {
        *outDown = 1;
    } else if (deg > 112.5f && deg < 157.5f) {
        *outDown = 1;
        *outLeft = 1;
    } else if (deg >= 157.5f || deg <= -157.5f) {
        *outLeft = 1;
    } else if (deg > -157.5f && deg < -112.5f) {
        *outUp = 1;
        *outLeft = 1;
    } else if (deg >= -112.5f && deg <= -67.5f) {
        *outUp = 1;
    } else if (deg > -67.5f && deg < -22.5f) {
        *outUp = 1;
        *outRight = 1;
    }
}

/* =====================================================================
 * Public Event & State Management
 * ===================================================================== */

void TouchOverlay_Init(void)
{
    memset(s_touches, 0, sizeof(s_touches));
    memset(s_touchKeystate, 0, sizeof(s_touchKeystate));
    memset(&s_pressed, 0, sizeof(s_pressed));
}

void TouchOverlay_Reset(void)
{
    memset(s_touches, 0, sizeof(s_touches));
    memset(s_touchKeystate, 0, sizeof(s_touchKeystate));
    memset(&s_pressed, 0, sizeof(s_pressed));
}

void TouchOverlay_TestMultiTouch(void)
{
    int screenW = 0, screenH = 0;
    platform_get_drawable_size(&screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) {
        screenW = 2400; screenH = 1080;
    }

    TouchLayout layout;
    ComputeLayout(screenW, screenH, &layout);

    SDL_Log("TouchOverlay: Running Multi-Touch Unit Verification (%dx%d)...", screenW, screenH);

    /* 1. Finger 1 on D-pad Right */
    SDL_Event e1;
    memset(&e1, 0, sizeof(e1));
    e1.type = SDL_FINGERDOWN;
    e1.tfinger.fingerId = 101;
    e1.tfinger.x = (layout.dpad_cx + layout.dpad_radius * 0.65f) / (float)screenW;
    e1.tfinger.y = layout.dpad_cy / (float)screenH;
    TouchOverlay_HandleEvent(&e1);

    /* 2. Finger 2 on Accel A */
    SDL_Event e2;
    memset(&e2, 0, sizeof(e2));
    e2.type = SDL_FINGERDOWN;
    e2.tfinger.fingerId = 102;
    e2.tfinger.x = layout.accel_cx / (float)screenW;
    e2.tfinger.y = layout.accel_cy / (float)screenH;
    TouchOverlay_HandleEvent(&e2);

    /* 3. Finger 3 on Jump B */
    SDL_Event e3;
    memset(&e3, 0, sizeof(e3));
    e3.type = SDL_FINGERDOWN;
    e3.tfinger.fingerId = 103;
    e3.tfinger.x = layout.jump_cx / (float)screenW;
    e3.tfinger.y = layout.jump_cy / (float)screenH;
    TouchOverlay_HandleEvent(&e3);

    /* Run update with mock keystate */
    unsigned char testKeystate[256];
    memset(testKeystate, 0, sizeof(testKeystate));
    TouchOverlay_Update(testKeystate);

    /* Verify all 3 keys are simultaneously active:
     * Right=DIK_RIGHT, Button A=DIK_SPACE (Confirm/Jump), Button B=DIK_A (Accel) */
    int passRight = (testKeystate[0xCD] == 0x80); /* DIK_RIGHT */
    int passA_Jump = (testKeystate[0x39] == 0x80); /* DIK_SPACE (Button A) */
    int passB_Accel = (testKeystate[0x1E] == 0x80); /* DIK_A (Button B) */

    if (passRight && passA_Jump && passB_Accel) {
        SDL_Log("TouchOverlay: [PASS] Multi-Touch simultaneous (Steer-Right + ButtonA[Jump] + ButtonB[Accel]) confirmed active!");
    } else {
        SDL_Log("TouchOverlay: [FAIL] Multi-Touch test failed: Right=%d, ButtonA=%d, ButtonB=%d",
                passRight, passA_Jump, passB_Accel);
    }

    /* Release finger 2 (Button A: Jump), keeping Steer and Button B (Accel) held */
    SDL_Event eUp2;
    memset(&eUp2, 0, sizeof(eUp2));
    eUp2.type = SDL_FINGERUP;
    eUp2.tfinger.fingerId = 102;
    TouchOverlay_HandleEvent(&eUp2);

    TouchOverlay_Update(testKeystate);
    TouchOverlay_Update(testKeystate);

    int passReleaseA = (testKeystate[0x39] == 0x00);
    int passStillRight = (testKeystate[0xCD] == 0x80);
    int passStillB = (testKeystate[0x1E] == 0x80);

    if (passReleaseA && passStillRight && passStillB) {
        SDL_Log("TouchOverlay: [PASS] Multi-Touch partial release (Button A released, Steer+ButtonB held) confirmed!");
    } else {
        SDL_Log("TouchOverlay: [FAIL] Multi-Touch release test failed: AReleased=%d, RightHeld=%d, BHeld=%d",
                passReleaseA, passStillRight, passStillB);
    }

    /* Reset all touches back to clean state */
    TouchOverlay_Reset();
    TouchOverlay_Update(testKeystate);
    int allCleared = 1;
    for (int i = 0; i < 256; i++) {
        if (testKeystate[i] != 0) { allCleared = 0; break; }
    }
    if (allCleared) {
        SDL_Log("TouchOverlay: [PASS] TouchOverlay_Reset cleanly cleared all keys (zero stuck keys)!");
    } else {
        SDL_Log("TouchOverlay: [FAIL] Stuck keys detected after reset!");
    }
}

void TouchOverlay_HandleEvent(const SDL_Event *event)
{
    switch (event->type) {
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION: {
            SDL_FingerID fid = event->tfinger.fingerId;
            int slot = -1;
            for (int i = 0; i < MAX_TOUCHES - 1; i++) {
                if ((s_touches[i].active || s_touches[i].pendingRelease) && s_touches[i].id == fid) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) {
                for (int i = 0; i < MAX_TOUCHES - 1; i++) {
                    if (!s_touches[i].active && !s_touches[i].pendingRelease) {
                        slot = i;
                        break;
                    }
                }
            }
            if (slot >= 0) {
                s_touches[slot].id = fid;
                s_touches[slot].x = event->tfinger.x;
                s_touches[slot].y = event->tfinger.y;
                s_touches[slot].active = 1;
                s_touches[slot].pendingRelease = 0;
            }
            break;
        }

        case SDL_FINGERUP: {
            SDL_FingerID fid = event->tfinger.fingerId;
            for (int i = 0; i < MAX_TOUCHES - 1; i++) {
                if (s_touches[i].active && s_touches[i].id == fid) {
                    s_touches[i].pendingRelease = 1;
                    break;
                }
            }
            break;
        }

        /* Mouse events for adb input tap / emulator pointer */
        case SDL_MOUSEBUTTONDOWN: {
            int w = 0, h = 0;
            platform_get_drawable_size(&w, &h);
            if (w > 0 && h > 0) {
                int slot = MAX_TOUCHES - 1;
                s_touches[slot].id = (SDL_FingerID)-100;
                s_touches[slot].x = (float)event->button.x / (float)w;
                s_touches[slot].y = (float)event->button.y / (float)h;
                s_touches[slot].active = 1;
                s_touches[slot].pendingRelease = 0;
            }
            break;
        }

        case SDL_MOUSEMOTION: {
            int slot = MAX_TOUCHES - 1;
            if (s_touches[slot].active && s_touches[slot].id == (SDL_FingerID)-100) {
                int w = 0, h = 0;
                platform_get_drawable_size(&w, &h);
                if (w > 0 && h > 0) {
                    s_touches[slot].x = (float)event->motion.x / (float)w;
                    s_touches[slot].y = (float)event->motion.y / (float)h;
                }
            }
            break;
        }

        case SDL_MOUSEBUTTONUP: {
            int slot = MAX_TOUCHES - 1;
            if (s_touches[slot].id == (SDL_FingerID)-100) {
                s_touches[slot].pendingRelease = 1;
            }
            break;
        }

        default:
            break;
    }
}

void TouchOverlay_Update(unsigned char *keystate)
{
    int screenW = 0, screenH = 0;
    platform_get_drawable_size(&screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) {
        return;
    }

    TouchLayout layout;
    ComputeLayout(screenW, screenH, &layout);

    TouchButtonState prev = s_pressed;
    memset(&s_pressed, 0, sizeof(s_pressed));

    /* Re-evaluate all active/pending touches against current layout */
    for (int i = 0; i < MAX_TOUCHES; i++) {
        if (!s_touches[i].active && !s_touches[i].pendingRelease) continue;

        float px = s_touches[i].x * (float)screenW;
        float py = s_touches[i].y * (float)screenH;

        HitTestDpad(px, py, layout.dpad_cx, layout.dpad_cy, layout.dpad_radius, layout.dpad_deadzone,
                    &s_pressed.up, &s_pressed.down, &s_pressed.left, &s_pressed.right);

        if (HitTestCircle(px, py, layout.accel_cx, layout.accel_cy, layout.accel_radius)) {
            s_pressed.accel = 1;
        }
        if (HitTestCircle(px, py, layout.jump_cx, layout.jump_cy, layout.jump_radius)) {
            s_pressed.jump = 1;
        }
        if (HitTestCircle(px, py, layout.driftL_cx, layout.driftL_cy, layout.driftL_radius)) {
            s_pressed.driftL = 1;
        }
        if (HitTestCircle(px, py, layout.driftR_cx, layout.driftR_cy, layout.driftR_radius)) {
            s_pressed.driftR = 1;
        }
        if (HitTestCircle(px, py, layout.look_cx, layout.look_cy, layout.look_radius)) {
            s_pressed.look = 1;
        }
        if (HitTestPill(px, py, layout.start_cx, layout.start_cy, layout.start_w * 0.5f, layout.start_h * 0.5f)) {
            s_pressed.start = 1;
        }

        /* Retire touches that were released */
        if (s_touches[i].pendingRelease) {
            s_touches[i].active = 0;
            s_touches[i].pendingRelease = 0;
        }
    }

    /* Log changes for debugging */
    if (memcmp(&prev, &s_pressed, sizeof(s_pressed)) != 0) {
        SDL_Log("TouchOverlay: pressed [U:%d D:%d L:%d R:%d A:%d J:%d DL:%d DR:%d Eye:%d Start:%d]",
                s_pressed.up, s_pressed.down, s_pressed.left, s_pressed.right,
                s_pressed.accel, s_pressed.jump, s_pressed.driftL, s_pressed.driftR,
                s_pressed.look, s_pressed.start);
    }

    /* Build touch scancode buffer */
    memset(s_touchKeystate, 0, sizeof(s_touchKeystate));

    if (s_pressed.up)     s_touchKeystate[0xC8] = 0x80; /* DIK_UP */
    if (s_pressed.down)   s_touchKeystate[0xD0] = 0x80; /* DIK_DOWN */
    if (s_pressed.left)   s_touchKeystate[0xCB] = 0x80; /* DIK_LEFT */
    if (s_pressed.right)  s_touchKeystate[0xCD] = 0x80; /* DIK_RIGHT */
    if (s_pressed.start)  s_touchKeystate[0x1C] = 0x80; /* DIK_RETURN */
    if (s_pressed.jump)   s_touchKeystate[0x1E] = 0x80; /* Button B -> DIK_A (Accel) */
    if (s_pressed.accel)  s_touchKeystate[0x39] = 0x80; /* Button A -> DIK_SPACE (Confirm / Jump) */
    if (s_pressed.driftL) s_touchKeystate[0x2C] = 0x80; /* DIK_Z */
    if (s_pressed.driftR) s_touchKeystate[0x2D] = 0x80; /* DIK_X */
    if (s_pressed.look)   s_touchKeystate[0x02] = 0x80; /* DIK_1 */

    /* Union with physical keystate into target keystate buffer */
    if (keystate) {
        for (int k = 0; k < 256; k++) {
            keystate[k] = s_physicalKeystate[k] | s_touchKeystate[k];
        }
    }
}

/* =====================================================================
 * GLES2 Vector Renderer
 * ===================================================================== */

typedef struct {
    float x, y;
    float r, g, b, a;
} OverlayVertex;

#define MAX_OVERLAY_VERTICES 4096
static OverlayVertex s_vertices[MAX_OVERLAY_VERTICES];
static int s_numVertices = 0;

static GLuint s_program = 0;
static GLint s_uScreenSize = -1;
static GLint s_aPos = -1;
static GLint s_aColor = -1;
static GLuint s_vbo = 0;

typedef struct {
    float x, y;
    float u, v;
    float r, g, b, a;
} OverlayTexVertex;

static GLuint s_texProgram = 0;
static GLint s_uTexScreenSize = -1;
static GLint s_uTexSampler = -1;
static GLint s_aTexPos = -1;
static GLint s_aTexUv = -1;
static GLint s_aTexColor = -1;
static GLuint s_texVbo = 0;

static GLuint s_texButtonA = 0;
static GLuint s_texButtonB = 0;
static int s_texturesLoaded = 0;

static const char *s_vertShaderSource =
    "attribute vec2 a_pos;\n"
    "attribute vec4 a_color;\n"
    "varying vec4 v_color;\n"
    "uniform vec2 u_screenSize;\n"
    "void main() {\n"
    "    float ndcX = (a_pos.x / u_screenSize.x) * 2.0 - 1.0;\n"
    "    float ndcY = 1.0 - (a_pos.y / u_screenSize.y) * 2.0;\n"
    "    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

static const char *s_fragShaderSource =
    "precision mediump float;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    gl_FragColor = v_color;\n"
    "}\n";

static const char *s_texVertShaderSource =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_color;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "uniform vec2 u_screenSize;\n"
    "void main() {\n"
    "    float ndcX = (a_pos.x / u_screenSize.x) * 2.0 - 1.0;\n"
    "    float ndcY = 1.0 - (a_pos.y / u_screenSize.y) * 2.0;\n"
    "    gl_Position = vec4(ndcX, ndcY, 0.0, 1.0);\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
    "}\n";

static const char *s_texFragShaderSource =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_texture, v_uv) * v_color;\n"
    "}\n";

static GLuint CompileShader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        SDL_Log("TouchOverlay: shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint LoadTextureFromAsset(const char *assetPath)
{
    SDL_RWops *rw = SDL_RWFromFile(assetPath, "rb");
    if (!rw) {
        SDL_Log("TouchOverlay: Asset not found: '%s'", assetPath);
        return 0;
    }

    Sint64 fileSize = SDL_RWsize(rw);
    if (fileSize <= 0 || fileSize > 10 * 1024 * 1024) {
        SDL_RWclose(rw);
        return 0;
    }

    void *buffer = malloc((size_t)fileSize);
    if (!buffer) {
        SDL_RWclose(rw);
        return 0;
    }

    if (SDL_RWread(rw, buffer, (size_t)fileSize, 1) != 1) {
        free(buffer);
        SDL_RWclose(rw);
        return 0;
    }
    SDL_RWclose(rw);

    int w = 0, h = 0, channels = 0;
    unsigned char *data = stbi_load_from_memory((const stbi_uc *)buffer, (int)fileSize, &w, &h, &channels, 4);
    free(buffer);

    if (!data) {
        SDL_Log("TouchOverlay: stbi failed to decode '%s': %s", assetPath, stbi_failure_reason());
        return 0;
    }

    GLuint texId = 0;
    glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glBindTexture(GL_TEXTURE_2D, 0);

    stbi_image_free(data);
    SDL_Log("TouchOverlay: Loaded button icon '%s' (%dx%d, texId=%u)", assetPath, w, h, texId);
    return texId;
}

static int InitGLPipeline(void)
{
    /* Check for GL context recreation */
    if (s_program != 0 && !glIsProgram(s_program)) {
        s_program = 0;
        s_texProgram = 0;
        s_texButtonA = 0;
        s_texButtonB = 0;
        s_texturesLoaded = 0;
        s_vbo = 0;
        s_texVbo = 0;
    }

    if (s_program != 0) return 1;

    /* 1. Vector color shader */
    GLuint vs = CompileShader(GL_VERTEX_SHADER, s_vertShaderSource);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, s_fragShaderSource);
    if (!vs || !fs) return 0;

    s_program = glCreateProgram();
    glAttachShader(s_program, vs);
    glAttachShader(s_program, fs);
    glBindAttribLocation(s_program, 0, "a_pos");
    glBindAttribLocation(s_program, 1, "a_color");
    glLinkProgram(s_program);

    GLint ok = 0;
    glGetProgramiv(s_program, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!ok) {
        char log[512];
        glGetProgramInfoLog(s_program, sizeof(log), NULL, log);
        SDL_Log("TouchOverlay: program link error: %s", log);
        glDeleteProgram(s_program);
        s_program = 0;
        return 0;
    }

    s_uScreenSize = glGetUniformLocation(s_program, "u_screenSize");
    s_aPos = glGetAttribLocation(s_program, "a_pos");
    s_aColor = glGetAttribLocation(s_program, "a_color");

    glGenBuffers(1, &s_vbo);

    /* 2. Textured shader */
    GLuint texVs = CompileShader(GL_VERTEX_SHADER, s_texVertShaderSource);
    GLuint texFs = CompileShader(GL_FRAGMENT_SHADER, s_texFragShaderSource);
    if (texVs && texFs) {
        s_texProgram = glCreateProgram();
        glAttachShader(s_texProgram, texVs);
        glAttachShader(s_texProgram, texFs);
        glBindAttribLocation(s_texProgram, 0, "a_pos");
        glBindAttribLocation(s_texProgram, 1, "a_uv");
        glBindAttribLocation(s_texProgram, 2, "a_color");
        glLinkProgram(s_texProgram);

        GLint texOk = 0;
        glGetProgramiv(s_texProgram, GL_LINK_STATUS, &texOk);
        glDeleteShader(texVs);
        glDeleteShader(texFs);

        if (texOk) {
            s_uTexScreenSize = glGetUniformLocation(s_texProgram, "u_screenSize");
            s_uTexSampler = glGetUniformLocation(s_texProgram, "u_texture");
            s_aTexPos = glGetAttribLocation(s_texProgram, "a_pos");
            s_aTexUv = glGetAttribLocation(s_texProgram, "a_uv");
            s_aTexColor = glGetAttribLocation(s_texProgram, "a_color");
            glGenBuffers(1, &s_texVbo);
        } else {
            char log[512];
            glGetProgramInfoLog(s_texProgram, sizeof(log), NULL, log);
            SDL_Log("TouchOverlay: tex program link error: %s", log);
            glDeleteProgram(s_texProgram);
            s_texProgram = 0;
        }
    }

    /* 3. Load button icon textures from APK assets */
    if (!s_texturesLoaded) {
        s_texButtonA = LoadTextureFromAsset("textures/Abutton.png");
        if (!s_texButtonA) s_texButtonA = LoadTextureFromAsset("Abutton.png");

        s_texButtonB = LoadTextureFromAsset("textures/Bbutton.png");
        if (!s_texButtonB) s_texButtonB = LoadTextureFromAsset("Bbutton.png");

        s_texturesLoaded = 1;
        SDL_Log("TouchOverlay: Icons initialized: ButtonA=%u, ButtonB=%u", s_texButtonA, s_texButtonB);
    }

    return 1;
}

static void PushVertex(float x, float y, float r, float g, float b, float a)
{
    if (s_numVertices < MAX_OVERLAY_VERTICES) {
        s_vertices[s_numVertices].x = x;
        s_vertices[s_numVertices].y = y;
        s_vertices[s_numVertices].r = r;
        s_vertices[s_numVertices].g = g;
        s_vertices[s_numVertices].b = b;
        s_vertices[s_numVertices].a = a;
        s_numVertices++;
    }
}

static void AddTriangle(float x1, float y1, float x2, float y2, float x3, float y3,
                        float r, float g, float b, float a)
{
    PushVertex(x1, y1, r, g, b, a);
    PushVertex(x2, y2, r, g, b, a);
    PushVertex(x3, y3, r, g, b, a);
}

static void AddQuad(float x1, float y1, float x2, float y2,
                    float x3, float y3, float x4, float y4,
                    float r, float g, float b, float a)
{
    AddTriangle(x1, y1, x2, y2, x3, y3, r, g, b, a);
    AddTriangle(x1, y1, x3, y3, x4, y4, r, g, b, a);
}

static void AddRect(float x, float y, float w, float h,
                    float r, float g, float b, float a)
{
    AddQuad(x, y, x + w, y, x + w, y + h, x, y + h, r, g, b, a);
}

static void AddCircle(float cx, float cy, float radius, int segments,
                      float r, float g, float b, float a)
{
    float step = 2.0f * (float)M_PI / (float)segments;
    for (int i = 0; i < segments; i++) {
        float a1 = (float)i * step;
        float a2 = (float)(i + 1) * step;
        AddTriangle(cx, cy,
                    cx + cosf(a1) * radius, cy + sinf(a1) * radius,
                    cx + cosf(a2) * radius, cy + sinf(a2) * radius,
                    r, g, b, a);
    }
}

static void AddRing(float cx, float cy, float innerR, float outerR, int segments,
                    float r, float g, float b, float a)
{
    float step = 2.0f * (float)M_PI / (float)segments;
    for (int i = 0; i < segments; i++) {
        float a1 = (float)i * step;
        float a2 = (float)(i + 1) * step;
        float c1 = cosf(a1), s1 = sinf(a1);
        float c2 = cosf(a2), s2 = sinf(a2);
        AddQuad(cx + c1 * innerR, cy + s1 * innerR,
                cx + c1 * outerR, cy + s1 * outerR,
                cx + c2 * outerR, cy + s2 * outerR,
                cx + c2 * innerR, cy + s2 * innerR,
                r, g, b, a);
    }
}

static void AddWedge(float cx, float cy, float innerR, float outerR, float startAngle, float endAngle, int segments,
                     float r, float g, float b, float a)
{
    float step = (endAngle - startAngle) / (float)segments;
    for (int i = 0; i < segments; i++) {
        float a1 = startAngle + (float)i * step;
        float a2 = startAngle + (float)(i + 1) * step;
        float c1 = cosf(a1), s1 = sinf(a1);
        float c2 = cosf(a2), s2 = sinf(a2);
        AddQuad(cx + c1 * innerR, cy + s1 * innerR,
                cx + c1 * outerR, cy + s1 * outerR,
                cx + c2 * outerR, cy + s2 * outerR,
                cx + c2 * innerR, cy + s2 * innerR,
                r, g, b, a);
    }
}

static void AddLine(float x1, float y1, float x2, float y2, float thickness,
                    float r, float g, float b, float a)
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * (thickness * 0.5f);
    float ny =  dx / len * (thickness * 0.5f);
    AddQuad(x1 + nx, y1 + ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, x1 - nx, y1 - ny, r, g, b, a);
}

/* =====================================================================
 * Glyph Drawings
 * ===================================================================== */

static void DrawGlyphA(float cx, float cy, float s, float thick, float r, float g, float b, float a)
{
    AddLine(cx - s * 0.45f, cy + s * 0.65f, cx, cy - s * 0.65f, thick, r, g, b, a);
    AddLine(cx, cy - s * 0.65f, cx + s * 0.45f, cy + s * 0.65f, thick, r, g, b, a);
    AddLine(cx - s * 0.26f, cy + s * 0.15f, cx + s * 0.26f, cy + s * 0.15f, thick, r, g, b, a);
}

static void DrawGlyphB(float cx, float cy, float s, float thick, float r, float g, float b, float a)
{
    float left = cx - s * 0.38f;
    float right = cx + s * 0.32f;
    float top = cy - s * 0.65f;
    float mid = cy;
    float bot = cy + s * 0.65f;

    AddLine(left, top, left, bot, thick, r, g, b, a);
    AddLine(left, top, right - s * 0.12f, top, thick, r, g, b, a);
    AddLine(right - s * 0.12f, top, right, top + (mid - top) * 0.5f, thick, r, g, b, a);
    AddLine(right, top + (mid - top) * 0.5f, right - s * 0.12f, mid, thick, r, g, b, a);
    AddLine(right - s * 0.12f, mid, left, mid, thick, r, g, b, a);

    AddLine(right - s * 0.12f, mid, right + s * 0.05f, mid + (bot - mid) * 0.5f, thick, r, g, b, a);
    AddLine(right + s * 0.05f, mid + (bot - mid) * 0.5f, right - s * 0.12f, bot, thick, r, g, b, a);
    AddLine(right - s * 0.12f, bot, left, bot, thick, r, g, b, a);
}

static void DrawGlyphL(float cx, float cy, float s, float thick, float r, float g, float b, float a)
{
    float left = cx - s * 0.30f;
    float right = cx + s * 0.38f;
    float top = cy - s * 0.60f;
    float bot = cy + s * 0.60f;
    AddLine(left, top, left, bot, thick, r, g, b, a);
    AddLine(left, bot, right, bot, thick, r, g, b, a);
}

static void DrawGlyphR(float cx, float cy, float s, float thick, float r, float g, float b, float a)
{
    float left = cx - s * 0.38f;
    float right = cx + s * 0.35f;
    float top = cy - s * 0.60f;
    float mid = cy - s * 0.05f;
    float bot = cy + s * 0.60f;

    AddLine(left, top, left, bot, thick, r, g, b, a);
    AddLine(left, top, right - s * 0.10f, top, thick, r, g, b, a);
    AddLine(right - s * 0.10f, top, right, top + (mid - top) * 0.5f, thick, r, g, b, a);
    AddLine(right, top + (mid - top) * 0.5f, right - s * 0.10f, mid, thick, r, g, b, a);
    AddLine(right - s * 0.10f, mid, left, mid, thick, r, g, b, a);
    AddLine(left + s * 0.08f, mid, right, bot, thick, r, g, b, a);
}

static void DrawGlyphEye(float cx, float cy, float s, float thick, float r, float g, float b, float a)
{
    AddLine(cx - s * 0.65f, cy, cx, cy - s * 0.38f, thick, r, g, b, a);
    AddLine(cx, cy - s * 0.38f, cx + s * 0.65f, cy, thick, r, g, b, a);
    AddLine(cx + s * 0.65f, cy, cx, cy + s * 0.38f, thick, r, g, b, a);
    AddLine(cx, cy + s * 0.38f, cx - s * 0.65f, cy, thick, r, g, b, a);
    AddCircle(cx, cy, s * 0.20f, 12, r, g, b, a);
}

static void DrawGlyphStart(float cx, float cy, float s, float thick, float r, float g, float b, float a)
{
    /* Play triangle */
    float triX = cx - s * 0.32f;
    AddTriangle(triX - s * 0.32f, cy - s * 0.40f,
                triX - s * 0.32f, cy + s * 0.40f,
                triX + s * 0.32f, cy,
                r, g, b, a);

    /* Two pause bars */
    float barX = cx + s * 0.22f;
    AddRect(barX, cy - s * 0.40f, thick * 1.4f, s * 0.80f, r, g, b, a);
    AddRect(barX + thick * 2.4f, cy - s * 0.40f, thick * 1.4f, s * 0.80f, r, g, b, a);
}

static void DrawTexturedButton(GLuint texId, float cx, float cy, float radius, int isPressed)
{
    if (!texId || s_texProgram == 0) return;

    /* Press visual feedback:
     * - Subtle tactile press shrink (0.92x radius) for realistic physical feedback
     * - Full opacity when pressed (1.0f vs 0.85f unpressed)
     * - Brightness modulation boost when pressed */
    float r = isPressed ? (radius * 0.92f) : radius;
    float alpha = isPressed ? 1.0f : 0.85f;
    float tint = isPressed ? 1.20f : 1.0f;

    OverlayTexVertex verts[6];
    float x1 = cx - r, y1 = cy - r;
    float x2 = cx + r, y2 = cy + r;

    verts[0] = (OverlayTexVertex){ x1, y1, 0.0f, 0.0f, tint, tint, tint, alpha };
    verts[1] = (OverlayTexVertex){ x2, y1, 1.0f, 0.0f, tint, tint, tint, alpha };
    verts[2] = (OverlayTexVertex){ x2, y2, 1.0f, 1.0f, tint, tint, tint, alpha };

    verts[3] = (OverlayTexVertex){ x1, y1, 0.0f, 0.0f, tint, tint, tint, alpha };
    verts[4] = (OverlayTexVertex){ x2, y2, 1.0f, 1.0f, tint, tint, tint, alpha };
    verts[5] = (OverlayTexVertex){ x1, y2, 0.0f, 1.0f, tint, tint, tint, alpha };

    glBindBuffer(GL_ARRAY_BUFFER, s_texVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texId);
    glUniform1i(s_uTexSampler, 0);

    glEnableVertexAttribArray(s_aTexPos);
    glVertexAttribPointer(s_aTexPos, 2, GL_FLOAT, GL_FALSE, sizeof(OverlayTexVertex), (const void *)0);

    glEnableVertexAttribArray(s_aTexUv);
    glVertexAttribPointer(s_aTexUv, 2, GL_FLOAT, GL_FALSE, sizeof(OverlayTexVertex), (const void *)(2 * sizeof(float)));

    glEnableVertexAttribArray(s_aTexColor);
    glVertexAttribPointer(s_aTexColor, 4, GL_FLOAT, GL_FALSE, sizeof(OverlayTexVertex), (const void *)(4 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, 6);

    glDisableVertexAttribArray(s_aTexPos);
    glDisableVertexAttribArray(s_aTexUv);
    glDisableVertexAttribArray(s_aTexColor);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* =====================================================================
 * Render Overlay Entry Point
 * ===================================================================== */

void TouchOverlay_Render(int screenWidth, int screenHeight)
{
    if (screenWidth <= 0 || screenHeight <= 0) return;
    if (!InitGLPipeline()) return;

    TouchLayout layout;
    ComputeLayout(screenWidth, screenHeight, &layout);

    s_numVertices = 0;

    /* -------------------------------------------------------------
     * 1. Render D-Pad
     * ------------------------------------------------------------- */
    float dpad_r = layout.dpad_radius;
    float dpad_cx = layout.dpad_cx;
    float dpad_cy = layout.dpad_cy;

    /* Base background circle */
    AddCircle(dpad_cx, dpad_cy, dpad_r, 32, 0.06f, 0.08f, 0.14f, 0.45f);
    AddRing(dpad_cx, dpad_cy, dpad_r - 3.0f, dpad_r, 32, 0.5f, 0.7f, 0.9f, 0.40f);

    /* Direction active wedges */
    float wedgeInner = layout.dpad_deadzone * 1.1f;
    float wedgeOuter = dpad_r - 4.0f;
    if (s_pressed.up) {
        AddWedge(dpad_cx, dpad_cy, wedgeInner, wedgeOuter, -112.5f * (float)M_PI / 180.0f, -67.5f * (float)M_PI / 180.0f, 8,
                 0.15f, 0.75f, 1.0f, 0.55f);
    }
    if (s_pressed.down) {
        AddWedge(dpad_cx, dpad_cy, wedgeInner, wedgeOuter, 67.5f * (float)M_PI / 180.0f, 112.5f * (float)M_PI / 180.0f, 8,
                 0.15f, 0.75f, 1.0f, 0.55f);
    }
    if (s_pressed.left) {
        AddWedge(dpad_cx, dpad_cy, wedgeInner, wedgeOuter, 157.5f * (float)M_PI / 180.0f, 202.5f * (float)M_PI / 180.0f, 8,
                 0.15f, 0.75f, 1.0f, 0.55f);
    }
    if (s_pressed.right) {
        AddWedge(dpad_cx, dpad_cy, wedgeInner, wedgeOuter, -22.5f * (float)M_PI / 180.0f, 22.5f * (float)M_PI / 180.0f, 8,
                 0.15f, 0.75f, 1.0f, 0.55f);
    }

    /* Direction arrows */
    float arrowDist = dpad_r * 0.60f;
    float arrowS = dpad_r * 0.18f;

    /* Up Arrow */
    float upR = s_pressed.up ? 0.2f : 0.85f;
    float upG = s_pressed.up ? 0.9f : 0.90f;
    float upB = s_pressed.up ? 1.0f : 0.95f;
    float upA = s_pressed.up ? 0.95f : 0.65f;
    AddTriangle(dpad_cx, dpad_cy - arrowDist - arrowS,
                dpad_cx - arrowS, dpad_cy - arrowDist + arrowS * 0.6f,
                dpad_cx + arrowS, dpad_cy - arrowDist + arrowS * 0.6f,
                upR, upG, upB, upA);

    /* Down Arrow */
    float dnR = s_pressed.down ? 0.2f : 0.85f;
    float dnG = s_pressed.down ? 0.9f : 0.90f;
    float dnB = s_pressed.down ? 1.0f : 0.95f;
    float dnA = s_pressed.down ? 0.95f : 0.65f;
    AddTriangle(dpad_cx, dpad_cy + arrowDist + arrowS,
                dpad_cx + arrowS, dpad_cy + arrowDist - arrowS * 0.6f,
                dpad_cx - arrowS, dpad_cy + arrowDist - arrowS * 0.6f,
                dnR, dnG, dnB, dnA);

    /* Left Arrow */
    float ltR = s_pressed.left ? 0.2f : 0.85f;
    float ltG = s_pressed.left ? 0.9f : 0.90f;
    float ltB = s_pressed.left ? 1.0f : 0.95f;
    float ltA = s_pressed.left ? 0.95f : 0.65f;
    AddTriangle(dpad_cx - arrowDist - arrowS, dpad_cy,
                dpad_cx - arrowDist + arrowS * 0.6f, dpad_cy - arrowS,
                dpad_cx - arrowDist + arrowS * 0.6f, dpad_cy + arrowS,
                ltR, ltG, ltB, ltA);

    /* Right Arrow */
    float rtR = s_pressed.right ? 0.2f : 0.85f;
    float rtG = s_pressed.right ? 0.9f : 0.90f;
    float rtB = s_pressed.right ? 1.0f : 0.95f;
    float rtA = s_pressed.right ? 0.95f : 0.65f;
    AddTriangle(dpad_cx + arrowDist + arrowS, dpad_cy,
                dpad_cx + arrowDist - arrowS * 0.6f, dpad_cy + arrowS,
                dpad_cx + arrowDist - arrowS * 0.6f, dpad_cy - arrowS,
                rtR, rtG, rtB, rtA);

    /* Center deadzone disc */
    AddCircle(dpad_cx, dpad_cy, layout.dpad_deadzone, 20, 0.12f, 0.15f, 0.22f, 0.65f);

    /* -------------------------------------------------------------
     * 2. Action Buttons
     * ------------------------------------------------------------- */
    float glyphThick = (float)screenHeight * 0.007f;
    if (glyphThick < 3.0f) glyphThick = 3.0f;

    /* ACCEL (A) — Custom Texture or Vector Fallback */
    if (!s_texButtonA) {
        float cx = layout.accel_cx, cy = layout.accel_cy, r = layout.accel_radius;
        float baseR = s_pressed.accel ? 0.15f : 0.10f;
        float baseG = s_pressed.accel ? 0.55f : 0.35f;
        float baseB = s_pressed.accel ? 1.00f : 0.85f;
        float baseA = s_pressed.accel ? 0.90f : 0.50f;
        AddCircle(cx, cy, r, 28, baseR, baseG, baseB, baseA);
        AddRing(cx, cy, r - 3.5f, r, 28, 0.4f, 0.75f, 1.0f, 0.8f);
        DrawGlyphA(cx, cy, r * 0.55f, glyphThick, 1.0f, 1.0f, 1.0f, 0.95f);
    } else if (s_pressed.accel) {
        /* Vibrant glowing ring on press */
        AddRing(layout.accel_cx, layout.accel_cy, layout.accel_radius - 2.0f, layout.accel_radius + 5.0f, 28, 0.4f, 1.0f, 0.6f, 0.8f);
    }

    /* JUMP (B) — Custom Texture or Vector Fallback */
    if (!s_texButtonB) {
        float cx = layout.jump_cx, cy = layout.jump_cy, r = layout.jump_radius;
        float baseR = s_pressed.jump ? 0.10f : 0.08f;
        float baseG = s_pressed.jump ? 0.90f : 0.65f;
        float baseB = s_pressed.jump ? 0.30f : 0.22f;
        float baseA = s_pressed.jump ? 0.90f : 0.50f;
        AddCircle(cx, cy, r, 28, baseR, baseG, baseB, baseA);
        AddRing(cx, cy, r - 3.5f, r, 28, 0.3f, 0.95f, 0.5f, 0.8f);
        DrawGlyphB(cx, cy, r * 0.55f, glyphThick, 1.0f, 1.0f, 1.0f, 0.95f);
    } else if (s_pressed.jump) {
        /* Vibrant glowing ring on press */
        AddRing(layout.jump_cx, layout.jump_cy, layout.jump_radius - 2.0f, layout.jump_radius + 5.0f, 28, 1.0f, 0.4f, 0.4f, 0.8f);
    }

    /* DRIFT L — Orange */
    {
        float cx = layout.driftL_cx, cy = layout.driftL_cy, r = layout.driftL_radius;
        float baseR = s_pressed.driftL ? 1.00f : 0.85f;
        float baseG = s_pressed.driftL ? 0.65f : 0.45f;
        float baseB = s_pressed.driftL ? 0.15f : 0.10f;
        float baseA = s_pressed.driftL ? 0.90f : 0.50f;
        AddCircle(cx, cy, r, 24, baseR, baseG, baseB, baseA);
        AddRing(cx, cy, r - 3.0f, r, 24, 1.0f, 0.75f, 0.3f, 0.8f);
        DrawGlyphL(cx, cy, r * 0.55f, glyphThick, 1.0f, 1.0f, 1.0f, 0.95f);
    }

    /* DRIFT R — Orange */
    {
        float cx = layout.driftR_cx, cy = layout.driftR_cy, r = layout.driftR_radius;
        float baseR = s_pressed.driftR ? 1.00f : 0.85f;
        float baseG = s_pressed.driftR ? 0.65f : 0.45f;
        float baseB = s_pressed.driftR ? 0.15f : 0.10f;
        float baseA = s_pressed.driftR ? 0.90f : 0.50f;
        AddCircle(cx, cy, r, 24, baseR, baseG, baseB, baseA);
        AddRing(cx, cy, r - 3.0f, r, 24, 1.0f, 0.75f, 0.3f, 0.8f);
        DrawGlyphR(cx, cy, r * 0.55f, glyphThick, 1.0f, 1.0f, 1.0f, 0.95f);
    }

    /* LOOK BACK — Purple */
    {
        float cx = layout.look_cx, cy = layout.look_cy, r = layout.look_radius;
        float baseR = s_pressed.look ? 0.80f : 0.60f;
        float baseG = s_pressed.look ? 0.30f : 0.20f;
        float baseB = s_pressed.look ? 0.95f : 0.80f;
        float baseA = s_pressed.look ? 0.90f : 0.50f;
        AddCircle(cx, cy, r, 24, baseR, baseG, baseB, baseA);
        AddRing(cx, cy, r - 3.0f, r, 24, 0.85f, 0.55f, 1.0f, 0.8f);
        DrawGlyphEye(cx, cy, r * 0.55f, glyphThick, 1.0f, 1.0f, 1.0f, 0.95f);
    }

    /* START / PAUSE — Red Pill */
    {
        float cx = layout.start_cx, cy = layout.start_cy;
        float halfW = layout.start_w * 0.5f;
        float halfH = layout.start_h * 0.5f;
        float baseR = s_pressed.start ? 0.95f : 0.75f;
        float baseG = s_pressed.start ? 0.25f : 0.15f;
        float baseB = s_pressed.start ? 0.25f : 0.15f;
        float baseA = s_pressed.start ? 0.90f : 0.50f;

        /* Body rect */
        AddRect(cx - halfW, cy - halfH, layout.start_w, layout.start_h, baseR, baseG, baseB, baseA);
        /* Endcaps */
        AddCircle(cx - halfW, cy, halfH, 16, baseR, baseG, baseB, baseA);
        AddCircle(cx + halfW, cy, halfH, 16, baseR, baseG, baseB, baseA);

        /* Border */
        float bThick = 2.5f;
        AddLine(cx - halfW, cy - halfH, cx + halfW, cy - halfH, bThick, 1.0f, 0.4f, 0.4f, 0.8f);
        AddLine(cx - halfW, cy + halfH, cx + halfW, cy + halfH, bThick, 1.0f, 0.4f, 0.4f, 0.8f);
        AddWedge(cx - halfW, cy, halfH - bThick, halfH, 90.0f * (float)M_PI / 180.0f, 270.0f * (float)M_PI / 180.0f, 12,
                 1.0f, 0.4f, 0.4f, 0.8f);
        AddWedge(cx + halfW, cy, halfH - bThick, halfH, -90.0f * (float)M_PI / 180.0f, 90.0f * (float)M_PI / 180.0f, 12,
                 1.0f, 0.4f, 0.4f, 0.8f);

        DrawGlyphStart(cx, cy, halfH * 0.85f, glyphThick, 1.0f, 1.0f, 1.0f, 0.95f);
    }

    /* Set 2D overlay render states */
    glViewport(0, 0, screenWidth, screenHeight);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* -------------------------------------------------------------
     * 3. Draw Batched Vector Geometry
     * ------------------------------------------------------------- */
    if (s_numVertices > 0) {
        glUseProgram(s_program);
        glUniform2f(s_uScreenSize, (float)screenWidth, (float)screenHeight);

        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glBufferData(GL_ARRAY_BUFFER, s_numVertices * sizeof(OverlayVertex), s_vertices, GL_STREAM_DRAW);

        glEnableVertexAttribArray(s_aPos);
        glVertexAttribPointer(s_aPos, 2, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex), (const void *)0);

        glEnableVertexAttribArray(s_aColor);
        glVertexAttribPointer(s_aColor, 4, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex), (const void *)(2 * sizeof(float)));

        glDrawArrays(GL_TRIANGLES, 0, s_numVertices);

        glDisableVertexAttribArray(s_aPos);
        glDisableVertexAttribArray(s_aColor);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    /* -------------------------------------------------------------
     * 4. Draw Custom Textured Buttons (A and B)
     * ------------------------------------------------------------- */
    if (s_texProgram != 0 && (s_texButtonA || s_texButtonB)) {
        glUseProgram(s_texProgram);
        glUniform2f(s_uTexScreenSize, (float)screenWidth, (float)screenHeight);

        if (s_texButtonA) {
            DrawTexturedButton(s_texButtonA, layout.accel_cx, layout.accel_cy, layout.accel_radius, s_pressed.accel);
        }
        if (s_texButtonB) {
            DrawTexturedButton(s_texButtonB, layout.jump_cx, layout.jump_cy, layout.jump_radius, s_pressed.jump);
        }
    }

    glUseProgram(0);
    glDepthMask(GL_TRUE);
}
