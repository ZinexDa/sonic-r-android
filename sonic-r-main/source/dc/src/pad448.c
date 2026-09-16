/**
 * pad448.c — Letterbox filler for the 448-line split-screen modes.
 *
 * PORT ADDITION, DC only. No binary equivalent.
 *
 * PVR clips to 32x32 tile boundaries for free, but 480/2 = 240 is 7.5 tiles,
 * so a horizontal split line cannot be expressed to the hardware at full
 * height. RenderHeightForMode() rounds the half down to 224 and renders 448
 * lines total in the modes that divide the height (2P horizontal, 3P, 4P).
 * The display stays DM_640x480, so scanlines 448..479 are real scanout that
 * nothing paints — a black bar along the bottom.
 *
 * This fills that bar with a 32x32 tile repeated across, drawn as one quad
 * with U running 0..20. PVR wraps by default: pvr_poly_cxt_txr leaves
 * txr.uv_clamp at PVR_UVCLAMP_NONE and compile_tpage_header never overrides
 * it, so the repeat costs nothing and needs no backend change.
 *
 * The texture is loaded once at boot by PVR_InitPad448Tpage (render_pvr.c,
 * next to the drain tpage) and frozen resident, so there is no per-frame or
 * per-track load path here.
 */

#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "net_transport.h"
#include "r_types.h"
#include "r_state.h"
#include "r_draw.h"

#define PAD448_TILE   32.0f

/* Height the framebuffer is actually scanned out at (480), as opposed to the
 * per-mode height we render into. Both owned by init.c. */
extern int g_screenHeightBase;
extern int RenderHeightForMode(void);

/* Depth handed to R_DrawQuad2D — matches the other 2D overlay callers. */
#define PAD448_Z      3.0f

/**
 * Pad448_Draw — fill the unrendered bottom scanlines.
 *
 * Call with the full-screen viewport and tile clip already selected: the
 * per-viewport clip left over from the last split pass would reject this,
 * and R_SetTileClipFullScreen deliberately sizes against g_screenHeightBase
 * rather than g_screenHeight, so the bottom tile row is inside the region.
 *
 * No-op unless the current mode actually renders short, and a no-op if the
 * texture failed to load (state never reaches 4).
 */
void Pad448_Draw(void)
{
#if !PAD448_ENABLE
    /* Superseded by the display origin-Y shift — see PAD448_ENABLE. */
    return;
#else
    int rendered = RenderHeightForMode();
    if (rendered >= g_screenHeightBase) {
        return;                                  /* full height, no bar */
    }
    if (g_tpageStateArray[TPAGE_PAD448] != 4) {
        return;                                  /* PAD448.TEX absent */
    }

    float top    = (float)rendered;
    float bottom = (float)g_screenHeightBase;
    float width  = (float)g_screenWidth;

    R_PushState();
    R_SetTexture(TPAGE_PAD448);
    R_SetTexEnv(R_TEXENV_MODULATE);
    R_SetFilter(R_FILTER_NEAREST);
    R_SetBlendMode(R_BLEND_NONE);
    R_SetDepthWrite(0);

    R_DrawQuad2D(0.0f, top, width, bottom,
                 0.0f, 0.0f,
                 width / PAD448_TILE,             /* 640/32 = 20 repeats */
                 (bottom - top) / PAD448_TILE,
                 PAD448_Z, 0xFFFFFFFFu);

    R_PopState();
#endif /* PAD448_ENABLE */
}
