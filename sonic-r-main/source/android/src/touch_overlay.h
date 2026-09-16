/**
 * touch_overlay.h — On-screen touch controls overlay for Sonic R Android
 *
 * Implements virtual D-pad and action buttons (Accel, Jump, Drift, Look, Start)
 * mapped to DirectInput keyboard scancodes for seamless integration with
 * Sonic R's input engine.
 */

#ifndef TOUCH_OVERLAY_H
#define TOUCH_OVERLAY_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

void TouchOverlay_Init(void);
void TouchOverlay_HandleEvent(const SDL_Event *event);
void TouchOverlay_Update(unsigned char *keystate);
void TouchOverlay_Render(int screenWidth, int screenHeight);
void TouchOverlay_Reset(void);
void TouchOverlay_TestMultiTouch(void);
void TouchOverlay_SetCustomLayout(int enabled,
                                  float dpadX, float dpadY, float dpadScale,
                                  float driftLX, float driftLY, float driftLScale,
                                  float accelX, float accelY, float accelScale,
                                  float jumpX, float jumpY, float jumpScale,
                                  float driftRX, float driftRY, float driftRScale,
                                  float lookX, float lookY, float lookScale,
                                  float startX, float startY, float startScale);
void TouchOverlay_ResetCustomLayout(void);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_OVERLAY_H */
