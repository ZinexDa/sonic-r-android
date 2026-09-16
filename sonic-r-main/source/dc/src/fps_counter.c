/* fps_counter.c - DC on-screen FPS readout for perf testing.
 *
 * FPS_Tick() runs once per flipped frame, accumulating count over a
 * 1-second wall-clock window. FPS_Render() draws the latest count as
 * three small digits at the top-left, reusing the HUD digit atlas
 * (g_tpageParallax1 row 0x80, 6×8 source, scaled to 12×16 on screen).
 *
 * Hooked from PVR_FlipFrame in render_pvr.c - Render before scene_finish
 * so the digits are part of the active scene; Tick after so the next
 * frame's window-end is measured against this frame's flip time.
 */

#include <kos.h>
#include <stdint.h>

extern void DrawTexturedQuad(int xPos, int yPos, int depth, int width, int height,
                             int tpage, int uvX, int uvY, int uvW, int uvH,
                             unsigned int color);
extern int g_tpageParallax1;

static uint64_t s_fpsLastSec = 0;
static int s_fpsFrameCount = 0;
static int s_fpsCurrent = 0;

#define HIDE_FPS 1

void FPS_Tick(void)
{
    uint64_t now = timer_ms_gettime64();
    s_fpsFrameCount++;
    if (s_fpsLastSec == 0) {
        s_fpsLastSec = now;
        return;
    }
    if (now - s_fpsLastSec >= 1000) {
        s_fpsCurrent = s_fpsFrameCount;
        s_fpsFrameCount = 0;
        s_fpsLastSec = now;
    }
}

void FPS_Render(void)
{
    /* g_tpageParallax1 may not be loaded on title/menu screens -
     * DrawTexturedQuad self-gates on tpage state, so this is a no-op
     * before the HUD atlas is up. */
    if (g_tpageParallax1 < 0) {
        return;
    }

    int fps = s_fpsCurrent;
    if (fps < 0) {
        fps = 0;
    }
    if (fps > 999) {
        fps = 999;
    }

    int hundreds = (fps / 100) % 10;
    int tens = (fps / 10) % 10;
    int ones = fps % 10;

    const int x0 = 8;
    const int y0 = 8;
    const int depth = 0x41200000;     /* HUD_DEPTH_LARGE — float 10.0 */
    const unsigned int color = 0xFFFFFFFFu;

    /* Lap-timer atlas: digits at row 0x80, baseX=0, 6px wide × 8px tall. */
#if HIDE_FPS
    int fps_tpage = 49;
#else
    int fps_tpage = g_tpageParallax1;
#endif

    DrawTexturedQuad(x0, y0, depth, 0xC, 0x10, fps_tpage,
                     hundreds * 6, 0x80, 6, 8, color);
    DrawTexturedQuad(x0 + 0xC, y0, depth, 0xC, 0x10, fps_tpage,
                     tens * 6, 0x80, 6, 8, color);
    DrawTexturedQuad(x0 + 0x18, y0, depth, 0xC, 0x10, fps_tpage,
                     ones * 6, 0x80, 6, 8, color);
}
