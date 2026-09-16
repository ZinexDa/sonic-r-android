/**
 * render_pvr.c - Texture upload + frame lifecycle for the PVR backend.
 *
 * DC counterpart to sdl/src/render_gl.c. This file owns the PVR texture
 * objects (per-tpage VRAM pointers + dirty bits), uploads pixel data,
 * and runs the per-frame begin/end + scene begin/finish.
 *
 * MVP scope: 16bpp tpage upload as PVR_TXRFMT_ARGB1555. Color-key handling
 * mirrors the GL path. Sky panorama upload is supported but minimal.
 */

#include <kos.h>
#include <dc/pvr.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>

#include "r_state.h"
#include "r_texture.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "net_transport.h"   /* TPAGE_DRAIN, TPAGE_PAD448 */
#include "sonicr_paths.h"    /* PATH_PAD448 */

#define MAX_TPAGES 52

extern void R_CompileTpageHeader(int);
extern void compile_untex_header(void);

/* Per-tpage VRAM allocations and metadata. */
pvr_ptr_t s_pvrTextures[MAX_TPAGES];
int s_pvrTextureDirty[MAX_TPAGES];
int s_pvrTextureW[MAX_TPAGES];
int s_pvrTextureH[MAX_TPAGES];
static int s_pvrTextureFrozen[MAX_TPAGES];
int s_pvrNoColorKey[MAX_TPAGES]; /* extern'd by r_pvr_backend for fmt choice */
int s_pvrGreen6[MAX_TPAGES];     /* per-tpage: source packs true 6-bit green at bits 10:5
                                  * (full-screen wallpapers + folded sky) - read 6-bit at
                                  * upload instead of 5-bit-then-expand. 0 = legacy 5-bit. */
int s_pvrSatBoost[MAX_TPAGES];   /* per-tpage chroma gain applied at upload, 8.8 fixed
                                  * (256 = 1.0x). 0 = off. Compensates the saturation
                                  * loss from the additive offset-colour lift used to
                                  * stand in for Add Signed. */
static unsigned char *s_pvrPendingRGBA[MAX_TPAGES];
static int s_pvrPendingRGBAw[MAX_TPAGES];
static int s_pvrPendingRGBAh[MAX_TPAGES];

/* Tpages pointed at TPAGE_DRAIN because their own pvr_mem_malloc failed,
 * and the colour-key setting each one had before it was borrowed. */
static int s_pvrTextureBorrowed[MAX_TPAGES];
static int s_pvrBorrowSavedNoKey[MAX_TPAGES];

/* Game-side pixel buffers (texture.c) */
extern void *g_tpagePixelBuf[];
extern int Random(void); /* 0x004E1342 - returns 0..0x7FFF */

/* Allow renderer code to know whether textures are initialized. */
static int s_pvrTexturesInited = 0;

void PVR_InitTextures(void)
{
    if (s_pvrTexturesInited) {
        return;
    }
    for (int i = 0; i < MAX_TPAGES; i++) {
        s_pvrTextures[i] = NULL;
        s_pvrTextureDirty[i] = 1;
        s_pvrTextureW[i] = 0;
        s_pvrTextureH[i] = 0;
        s_pvrTextureFrozen[i] = 0;
        s_pvrNoColorKey[i] = 0;
        s_pvrGreen6[i] = 0;
        s_pvrPendingRGBA[i] = NULL;
        s_pvrTextureBorrowed[i] = 0;
        s_pvrBorrowSavedNoKey[i] = 0;
    }
    s_pvrTexturesInited = 1;
}

/* =====================================================================
 * Out-of-VRAM fallback.
 *
 * pvr_mem_malloc can fail — texture VRAM is whatever is left after the
 * framebuffers, the PVR vertex buffer and the OPBs. Storing a NULL into
 * s_pvrTextures and carrying on would leave the tpage pointing nowhere
 * while its compiled header still says "sample from here", so a borrowed
 * page is used instead: the tpage is aimed at the always-resident
 * TPAGE_DRAIN texture, whose every texel is the colour key. Quads drawn
 * from it are punched through by the PT alpha test, so the page renders
 * as nothing at all rather than as stale VRAM.
 *
 * A borrowed page does NOT own its allocation. It is never uploaded to,
 * never written through, and never freed — doing any of those would
 * damage the drain texture that the fade-iris drain and every other
 * borrower share. The upload paths retry the allocation on the next
 * attempt (the borrowed flag forces the size check to miss), so a page
 * recovers on its own once VRAM frees up.
 * ===================================================================== */
extern void R_CompileTpageHeader(int tpage);

static int borrow_drain_tpage(int tpage)
{
    if (tpage == TPAGE_DRAIN || !s_pvrTextures[TPAGE_DRAIN]) {
        return 0;
    }

    /* The drain texture is keyed, so the header has to be compiled keyed
     * whatever this tpage normally wants. Remember the real setting: the
     * page keeps its own format the moment it gets real VRAM back. */
    s_pvrBorrowSavedNoKey[tpage] = s_pvrNoColorKey[tpage];
    s_pvrNoColorKey[tpage] = 0;

    s_pvrTextures[tpage] = s_pvrTextures[TPAGE_DRAIN];
    s_pvrTextureW[tpage] = s_pvrTextureW[TPAGE_DRAIN];
    s_pvrTextureH[tpage] = s_pvrTextureH[TPAGE_DRAIN];
    s_pvrTextureBorrowed[tpage] = 1;
    R_CompileTpageHeader(tpage);
    return 1;
}

/* Drop a tpage's VRAM ahead of a fresh allocation. Borrowed pages are
 * released by forgetting them, never by freeing. */
static void release_tpage_vram(int tpage)
{
    if (s_pvrTextureBorrowed[tpage]) {
        s_pvrTextureBorrowed[tpage] = 0;
        s_pvrNoColorKey[tpage] = s_pvrBorrowSavedNoKey[tpage];
        s_pvrTextures[tpage] = NULL;
        return;
    }
    if (s_pvrTextures[tpage]) {
        pvr_mem_free(s_pvrTextures[tpage]);
        s_pvrTextures[tpage] = NULL;
    }
}

/* Take VRAM for a tpage, falling back to the drain page. Returns 0 when
 * the caller must abandon the upload — either because the page is now
 * borrowed, or because even the drain page was unavailable and nothing
 * may draw this tpage at all. */
static int acquire_tpage_vram(int tpage, int w, int h)
{
    release_tpage_vram(tpage);

    s_pvrTextures[tpage] = pvr_mem_malloc(w * h * 2);
    if (!s_pvrTextures[tpage]) {
        if (!borrow_drain_tpage(tpage)) {
            /* No drain page either. Park the tpage so DrawTexturedQuad's
             * state gate stops it being drawn from a null pointer. */
            s_pvrTextureW[tpage] = 0;
            s_pvrTextureH[tpage] = 0;
            g_tpageStateArray[tpage] = 0;
        }
        return 0;
    }

    s_pvrTextureW[tpage] = w;
    s_pvrTextureH[tpage] = h;
    R_CompileTpageHeader(tpage);
    return 1;
}

/* Source pixels live in g_tpagePixelBuf[] in the binary's native 16bpp
 * packing: R5 at bits 15-11, G5 at bits 10-6, B5 at bits 4-0 (bit 5 unused).
 * Same bit positions as ARGB1555 minus the alpha bit, so conversion is
 * essentially "set alpha=1 unless the pixel is the color key" - magenta
 * (r5≈31, g5≈0, b5≈31) → A=0. The SDL renderer uses a tolerant threshold
 * to catch near-magenta from filtering artifacts; we match it. */
extern int g_tpageWidth[];
extern int g_tpageHeight[];

#define MAX_TPAGE_PIXELS (1024 * 512)
static uint16_t s_pvrConvertBuf[MAX_TPAGE_PIXELS];

static void upload_tpage_16bpp(int tpage)
{
    void *src = g_tpagePixelBuf[tpage];
    if (!src) {
        return;
    }

    int srcW = g_tpageWidth[tpage];
    int srcH = g_tpageHeight[tpage];
    if (srcW == 0) {
        srcW = 256;
    }
    if (srcH == 0) {
        srcH = 256;
    }

    /* Wallpaper case: 640x480 source resampled into 512x512 POT VRAM.
     * Twiddled requires POT and PVR has no native non-POT path here.
     * Nearest-neighbor downsample is fine for backdrops. UV math is
     * unaffected - the engine emits normalized fractions, and the GPU
     * samples the same fraction of the resampled image. */
    int physW, physH;
    int wallpaper = (srcW > 256 || srcH > 256);
    if (wallpaper) {
        physW = 512;
        physH = 512;
    }
    else {
        physW = srcW;
        physH = srcH;
    }
    if (physW * physH > MAX_TPAGE_PIXELS) {
        return;
    }

    if (!s_pvrTextures[tpage] || s_pvrTextureBorrowed[tpage] ||
        s_pvrTextureW[tpage] != physW || s_pvrTextureH[tpage] != physH)
    {
        if (!acquire_tpage_vram(tpage, physW, physH)) {
            return;   /* borrowed the drain page, or parked — nothing to upload */
        }
        //printf("[PVR] %d bytes free\n", pvr_mem_available());
    }

    uint16_t *src16 = (uint16_t *)src;
    int noKey = s_pvrNoColorKey[tpage];

    /* Two output formats:
     *   noKey  → RGB565   (no alpha bit, full 6-bit green) - better color
     *                     fidelity for env maps / wallpapers / sky.
     *   keyed  → ARGB1555 (1-bit alpha) - color-key transparency.
     * The PVR header for this tpage must declare the matching format. */
    if (wallpaper) {
        for (int dy = 0; dy < physH; dy++) {
            int sy = dy * srcH / physH;
            const uint16_t *srcRow = src16 + sy * srcW;
            uint16_t *dstRow = s_pvrConvertBuf + dy * physW;
            if (noKey) {
                int green6 = s_pvrGreen6[tpage];
                for (int dx = 0; dx < physW; dx++) {
                    int sx = dx * srcW / physW;
                    uint16_t p  = srcRow[sx];
                    uint16_t r5 = (p >> 11) & 0x1F;
                    uint16_t b5 = p & 0x1F;
                    uint16_t g6;
                    if (green6) {
                        g6 = (p >> 5) & 0x3F;        /* true 6-bit green at bits 10:5 */
                    }
                    else {
                        uint16_t g5 = (p >> 6) & 0x1F;
                        g6 = (g5 << 1) | (g5 >> 4);  /* legacy 5-bit -> 6-bit */
                    }
                    dstRow[dx] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                }
            }
            else {
                for (int dx = 0; dx < physW; dx++) {
                    int sx = dx * srcW / physW;
                    uint16_t p  = srcRow[sx];
                    uint16_t r5 = (p >> 11) & 0x1F;
                    uint16_t g5 = (p >>  6) & 0x1F;
                    uint16_t b5 = p & 0x1F;
                    uint16_t a1 = IS_COLOR_KEY_RGB5(r5, g5, b5) ? 0 : 1;
                    dstRow[dx] = (uint16_t)((a1 << 15) | (r5 << 10) | (g5 << 5) | b5);
                }
            }
        }
    } else {
        if (noKey) {
            int green6 = s_pvrGreen6[tpage];
            for (int i = 0; i < physW * physH; i++) {
                uint16_t p  = src16[i];
                uint16_t r5 = (p >> 11) & 0x1F;
                uint16_t b5 = p & 0x1F;
                uint16_t g6;
                if (green6) {
                    g6 = (p >> 5) & 0x3F;        /* true 6-bit green at bits 10:5 */
                }
                else {
                    uint16_t g5 = (p >> 6) & 0x1F;
                    g6 = (g5 << 1) | (g5 >> 4);  /* legacy 5-bit -> 6-bit */
                }
                s_pvrConvertBuf[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            }
        }
        else {
            /* Chroma gain runs here, after the key test reads the source
             * texel, so key texels pass through bit-exact. Reads the CPU
             * buffer and writes the convert buffer, so re-uploads are not
             * cumulative. */
            int satK = s_pvrSatBoost[tpage];
            for (int i = 0; i < physW * physH; i++) {
                uint16_t p  = src16[i];
                int r5 = (p >> 11) & 0x1F;
                int g5 = (p >>  6) & 0x1F;
                int b5 = p & 0x1F;
                uint16_t a1 = IS_COLOR_KEY_RGB5(r5, g5, b5) ? 0 : 1;
                if (satK != 0 && a1 != 0) {
                    int lum = ((r5 * 77) + (g5 * 151) + (b5 * 28)) >> 8;
                    r5 = lum + (((r5 - lum) * satK) / 256);
                    g5 = lum + (((g5 - lum) * satK) / 256);
                    b5 = lum + (((b5 - lum) * satK) / 256);
                    if (r5 < 0) {
                        r5 = 0;
                    }
                    else if (r5 > 31) {
                        r5 = 31;
                    }
                    if (g5 < 0) {
                        g5 = 0;
                    }
                    else if (g5 > 31) {
                        g5 = 31;
                    }
                    if (b5 < 0) {
                        b5 = 0;
                    }
                    else if (b5 > 31) {
                        b5 = 31;
                    }
                }
                s_pvrConvertBuf[i] = (uint16_t)((a1 << 15) | (r5 << 10) | (g5 << 5) | b5);
            }
        }
    }

    pvr_txr_load_ex(s_pvrConvertBuf, s_pvrTextures[tpage], physW, physH, PVR_TXRLOAD_16BPP);
    s_pvrTextureDirty[tpage] = 0;
}

void PVR_UploadTpageRGBA(int tpage, unsigned char *rgba, int w, int h);

extern void R_InvalidateTpageHeader(int tpage);
extern int  s_pvrKeepPixels[];   /* render_pvr_glue.c */

/* After a successful upload the tpage's source pixels live in VRAM, so
 * we can drop the system-RAM shadow unless something downstream still
 * reads it (parallax sub-rects via PVR_UploadTpageSubRect, sky gradient
 * regenerator). The keep flag is set early enough by LoadTextureSubRect
 * → GL_KeepPixels so this fires safely for the bulk of tpages. */
static void release_system_pixels_if_unused(int tpage)
{
    if (s_pvrKeepPixels[tpage]) {
        return;
    }
    if (g_tpagePixelBuf[tpage] != NULL) {
        free(g_tpagePixelBuf[tpage]);
        g_tpagePixelBuf[tpage] = NULL;
    }
}

void PVR_UploadTpage(int tpage)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    if (s_pvrTextureFrozen[tpage]) {
        return;
    }

    if (s_pvrPendingRGBA[tpage]) {
        unsigned char *rgba = s_pvrPendingRGBA[tpage];
        PVR_UploadTpageRGBA(tpage, rgba,
                            s_pvrPendingRGBAw[tpage],
                            s_pvrPendingRGBAh[tpage]);
        free(rgba);
        s_pvrPendingRGBA[tpage] = NULL;
        release_system_pixels_if_unused(tpage);
        return;
    }

    upload_tpage_16bpp(tpage);
    release_system_pixels_if_unused(tpage);
}

void PVR_UploadTpageRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }

    if (!rgba) {
        return;
    }

    if (!s_pvrTextures[tpage] || s_pvrTextureBorrowed[tpage] ||
        s_pvrTextureW[tpage] != w || s_pvrTextureH[tpage] != h)
    {
        if (!acquire_tpage_vram(tpage, w, h)) {
            return;   /* borrowed the drain page, or parked — nothing to upload */
        }
        //printf("[PVR] %d bytes free\n", pvr_mem_available());
    }

    int noKey = s_pvrNoColorKey[tpage];
    uint16_t *tmp = (uint16_t *)malloc(w * h * 2);
    if (!tmp) {
        return;
    }
    for (int i = 0; i < w * h; i++) {
        unsigned char r = rgba[i*4 + 0];
        unsigned char g = rgba[i*4 + 1];
        unsigned char b = rgba[i*4 + 2];
        unsigned char a = rgba[i*4 + 3];
        uint16_t out;
        if (noKey) {
            int r5 = r >> 3;
            int g5 = g >> 3;
            int b5 = b >> 3;
            int g6 = (g5 << 1) | (g5 >> 4);
            out = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
        else {
            out = ((a >= 128) ? 0x8000 : 0)
                | ((r >> 3) << 10)
                | ((g >> 3) << 5)
                |  (b >> 3);
        }
        tmp[i] = out;
    }
    pvr_txr_load_ex(tmp, s_pvrTextures[tpage], w, h, PVR_TXRLOAD_16BPP);
    free(tmp);
    s_pvrTextureDirty[tpage] = 0;
}

/* Bit-interleave twiddle for a square POT texture. PVR stores twiddled
 * texels in a Morton-order curve: bit 0 of y, bit 0 of x, bit 1 of y,
 * bit 1 of x, ... - so pixel (x,y) lives at the interleaved index. */
static unsigned int pvr_twiddle_sq(unsigned int x, unsigned int y,
                                   unsigned int dim_log2)
{
    unsigned int z = 0;
    for (unsigned int i = 0; i < dim_log2; i++) {
        z |= ((y >> i) & 1u) << (2 * i);
        z |= ((x >> i) & 1u) << (2 * i + 1);
    }
    return z;
}

/* Partial sub-rect upload: writes ONLY the requested pixels to VRAM,
 * leaving the rest of the texture untouched (matches GL's glTexSubImage2D
 * semantic). Engine pattern is freeze-then-patch: freeze locks the texture,
 * then patches land in the CPU buffer, then this commits the explicit
 * sub-rect. Other CPU-side writes intentionally don't reach VRAM. */
void PVR_UploadTpageSubRect(int tpage, int destX, int destY, int width, int height)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
         return;
    }
    if (!s_pvrTextures[tpage] || !g_tpagePixelBuf[tpage]) {
        return;
    }
    if (s_pvrTextureBorrowed[tpage]) {
        return;   /* drain page is shared — never write through a borrower */
    }

    int srcW = g_tpageWidth[tpage];
    int srcH = g_tpageHeight[tpage];
    if (srcW == 0) {
        srcW = 256;
    }
    if (srcH == 0) {
        srcH = 256;
    }

    int physW = s_pvrTextureW[tpage];
    int physH = s_pvrTextureH[tpage];

    /* Square POT verification - required for the simple twiddle math.
     * Wallpapers (resampled to 512x512) and other oddities fall back to
     * a full re-upload, which is wrong-vs-PC for those tpages but they
     * don't actually go through this code path in practice. */
    int isSquarePot = (physW == physH && physW == srcW && physH == srcH);
    unsigned int dim_log2 = 0;
    if (isSquarePot) {
        unsigned int d = (unsigned int)physW;
        while (d > 1) {
            d >>= 1;
            dim_log2++;
        }
        if ((1u << dim_log2) != (unsigned int)physW) {
            isSquarePot = 0;
        }
    }

    if (!isSquarePot) {
        int wasFrozen = s_pvrTextureFrozen[tpage];
        s_pvrTextureFrozen[tpage] = 0;
        upload_tpage_16bpp(tpage);
        s_pvrTextureFrozen[tpage] = wasFrozen;
        return;
    }

    uint16_t *src = (uint16_t *)g_tpagePixelBuf[tpage];
    uint16_t *vram = (uint16_t *)s_pvrTextures[tpage];
    int noKey = s_pvrNoColorKey[tpage];

    for (int row = 0; row < height; row++) {
        int dy = destY + row;
        if (dy < 0 || dy >= physH) {
            continue;
        }
        for (int col = 0; col < width; col++) {
            int dx = destX + col;
            if (dx < 0 || dx >= physW) {
                continue;
            }
            uint16_t p  = src[dy * srcW + dx];
            uint16_t r5 = (p >> 11) & 0x1F;
            uint16_t g5 = (p >>  6) & 0x1F;
            uint16_t b5 =  p        & 0x1F;
            uint16_t out;
            if (noKey) {
                uint16_t g6 = (g5 << 1) | (g5 >> 4);
                out = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            }
            else {
                uint16_t a1 = IS_COLOR_KEY_RGB5(r5, g5, b5) ? 0 : 1;
                out = (uint16_t)((a1 << 15) | (r5 << 10) | (g5 << 5) | b5);
            }
            unsigned int idx = pvr_twiddle_sq((unsigned)dx, (unsigned)dy, dim_log2);
            vram[idx] = out;
        }
    }
}

void PVR_MarkTpageDirty(int tpage)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    if (s_pvrTextureFrozen[tpage]) {
        return;
    }
    s_pvrTextureDirty[tpage] = 1;
    /* Eager upload: do the VRAM transfer at the call site rather than
     * deferring to first-draw. Lazy uploads hide ordering bugs by
     * surfacing failures far from the offending mark-dirty call. */
    PVR_UploadTpage(tpage);
}

void PVR_ClearTpageDirty(int tpage)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    s_pvrTextureDirty[tpage] = 0;
}

void PVR_FreezeTpage(int tpage)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    if (s_pvrTextureDirty[tpage]) {
        PVR_UploadTpage(tpage);
    }
    s_pvrTextureFrozen[tpage] = 1;
}

/* Clear the freeze flag so subsequent MarkDirty + UploadTpage work normally.
 * Called by LoadTPageRGB because a full re-upload of the tpage
 * means whatever was previously frozen is no longer relevant - the engine
 * is replacing the entire content. Without this, the freeze sticks across
 * race transitions and blocks new track-specific uploads on slots that
 * were frozen for a previous track's parallax. */
void PVR_ThawTpage(int tpage)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    s_pvrTextureFrozen[tpage] = 0;
}

void PVR_SetPendingRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    s_pvrPendingRGBA[tpage] = rgba;
    s_pvrPendingRGBAw[tpage] = w;
    s_pvrPendingRGBAh[tpage] = h;
    s_pvrTextureDirty[tpage] = 1;
    PVR_UploadTpage(tpage);
}

extern void R_InvalidateTpageHeader(int);
extern void R_CompileTpageHeader(int);

void PVR_SetNoColorKey(int tpage)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    if (s_pvrNoColorKey[tpage] == 1) {
        return;
    }
    s_pvrNoColorKey[tpage] = 1;
    s_pvrTextureDirty[tpage] = 1;
    R_InvalidateTpageHeader(tpage);
    PVR_UploadTpage(tpage);
    R_CompileTpageHeader(tpage);
}

/* Mark a tpage's source as carrying true 6-bit green (bits 10:5). Set before
 * the tpage is uploaded; the noKey upload then reads green at full precision
 * instead of truncating to 5-bit. Used for full-screen wallpapers + the folded
 * sky/parallax with 6-bit green. No re-upload here - callers upload afterwards. */
void PVR_SetTpageGreen6(int tpage, int on)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    s_pvrGreen6[tpage] = on ? 1 : 0;
}

/* Per-tpage chroma gain in 8.8 fixed point (256 = 1.0x, 0 = off), applied by
 * the keyed upload path. Set before the tpage is uploaded; no re-upload here -
 * callers upload afterwards. */
void PVR_SetSatBoost(int tpage, int k256)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    s_pvrSatBoost[tpage] = k256;
}

void PVR_ClearNoColorKey(int tpage)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    if (s_pvrNoColorKey[tpage] == 0) {
        return;
    }
    s_pvrNoColorKey[tpage] = 0;
    s_pvrTextureDirty[tpage] = 1;
    R_InvalidateTpageHeader(tpage);
    PVR_UploadTpage(tpage);
    R_CompileTpageHeader(tpage);
}

/* =====================================================================
 * GenerateSkyGradientTpage - DC override of sdl/src/sky_gradient.c.
 *
 * Writes 8 randomized gradient strips directly into the VRAM-resident
 * twiddled tpage at g_tpageCharBase (no system-RAM intermediate). Mirrors
 * the binary's two-rgb-tuple-per-strip interpolation: each strip has top
 * and bottom RGB endpoints, with horizontal interpolation toward the
 * NEXT strip's endpoints (entry[8] wraps to entry[0] for seamless looping).
 *
 * Region within the 256x256 tpage: 8 strips x 16 columns = 128 cols wide,
 * 15 rows tall, anchored at row 240 (matching binary offset +0x1E000 in
 * the linear 16bpp buffer used by the SDL build).
 *
 * Format: ARGB1555 with alpha=1 for keyed tpages, RGB565 for noKey.
 * ===================================================================== */
void GenerateSkyGradientTpage(void)
{
    int surfIdx = g_tpageCharBase;
    if (surfIdx < 0 || surfIdx >= MAX_TPAGES) {
        return;
    }
    if (!s_pvrTextures[surfIdx]) {
        return;
    }
    if (s_pvrTextureBorrowed[surfIdx]) {
        return;   /* drain page is shared — never write through a borrower */
    }

    /* Tpage must be square POT for the twiddle math we use. All character
     * tpages are 256x256, so dim_log2 = 8. Verify defensively. */
    int physW = s_pvrTextureW[surfIdx];
    int physH = s_pvrTextureH[surfIdx];
    if (physW != 256 || physH != 256) {
        return;
    }
    const unsigned int dim_log2 = 8;

    int noKey = s_pvrNoColorKey[surfIdx];
    uint16_t *vram = (uint16_t *)s_pvrTextures[surfIdx];

    /* 8 (+ 1) random R/G/B endpoints per channel, top and bottom.
     * Each value is either ~0 or ~31 in 5-bit (16.16 fixed). [8] wraps to [0]. */
    int topR[9];
    int topG[9];
    int topB[9];
    int botR[9];
    int botG[9];
    int botB[9];
    for (int i = 0; i < 8; i++)
    {
        topR[i] = (Random() > 0x4000) ? 0x8000 : 0x1F8000;
        topG[i] = (Random() > 0x4000) ? 0x8000 : 0x1F8000;
        topB[i] = (Random() > 0x4000) ? 0x8000 : 0x1F8000;
        botR[i] = (Random() > 0x4000) ? 0x8000 : 0x1F8000;
        botG[i] = (Random() > 0x4000) ? 0x8000 : 0x1F8000;
        botB[i] = (Random() > 0x4000) ? 0x8000 : 0x1F8000;
    }
    topR[8] = topR[0];
    topG[8] = topG[0];
    topB[8] = topB[0];
    botR[8] = botR[0];
    botG[8] = botG[0];
    botB[8] = botB[0];

    /* per-strip 16-col x 15-row write. */
    const int rowBase = 240;
    for (int strip = 0; strip < 8; strip++) {
        int curR = topR[strip];
        int curG = topG[strip];
        int curB = topB[strip];
        int curR2 = botR[strip];
        int curG2 = botG[strip];
        int curB2 = botB[strip];
        int stepR = (topR[strip + 1] - curR) / 16;
        int stepG = (topG[strip + 1] - curG) / 16;
        int stepB = (topB[strip + 1] - curB) / 16;
        int stepR2 = (botR[strip + 1] - curR2) / 16;
        int stepG2 = (botG[strip + 1] - curG2) / 16;
        int stepB2 = (botB[strip + 1] - curB2) / 16;

        int colBase = strip * 16;
        for (int x = 0; x < 16; x++) {
            int colStepR = (curR2 - curR) / 15;
            int colStepG = (curG2 - curG) / 15;
            int colStepB = (curB2 - curB) / 15;

            int pixR = curR, pixG = curG, pixB = curB;
            for (int y = 0; y < 15; y++) {
                int r5 = pixR >> 16;
                int g5 = pixG >> 16;
                int b5 = pixB >> 16;
                if (r5 < 0) {
                    r5 = 0;
                }
                if (r5 > 31) {
                    r5 = 31;
                }
                if (g5 < 0) {
                    g5 = 0;
                }
                if (g5 > 31) {
                    g5 = 31;
                }
                if (b5 < 0) {
                    b5 = 0;
                }
                if (b5 > 31) {
                    b5 = 31;
                }

                uint16_t pixel;
                if (noKey) {
                    int g6 = (g5 << 1) | (g5 >> 4);
                    pixel = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                }
                else {
                    pixel = (uint16_t)(0x8000u | (r5 << 10) | (g5 << 5) | b5);
                }

                unsigned int absX = (unsigned int)(colBase + x);
                unsigned int absY = (unsigned int)(rowBase + y);
                unsigned int idx = pvr_twiddle_sq(absX, absY, dim_log2);
                vram[idx] = pixel;

                pixR += colStepR;
                pixG += colStepG;
                pixB += colStepB;
            }

            curR  += stepR;  curG  += stepG;  curB  += stepB;
            curR2 += stepR2; curG2 += stepG2; curB2 += stepB2;
        }
    }
}

/* =====================================================================
 * PVR_ColorizeTpageVRAM - DC implementation of the menu-tint recolor.
 *
 * Mirrors ColorizeTpageHiColor's algorithm but operates directly on the
 * twiddled VRAM-resident tpage. No system-RAM dependency: the caller
 * pattern is always LoadTPageRGB(g_uiTexPage, ...) immediately followed
 * by ColorizeTpageHiColor(...), so VRAM holds fresh source pixels at
 * recolor time. Reading and writing in place keeps the g_tpagePixelBuf
 * shadow free for release_system_pixels_if_unused to reclaim.
 *
 * Format: handles RGB565 (noKey) and ARGB1555 (keyed). The wallpaper
 * tpages we tint are uploaded as RGB565, but we cover both for safety.
 *
 * Index extraction: source RAW files are grayscale (R=G=B), so after
 * upload the low 5 bits of the VRAM pixel equal the original intensity
 * regardless of which 16-bit format the upload chose.
 * ===================================================================== */
void PVR_ColorizeTpageVRAM(int tpage, int targetR, int targetG, int targetB)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    if (!s_pvrTextures[tpage]) {
        return;
    }
    if (s_pvrTextureBorrowed[tpage]) {
        return;   /* drain page is shared — never write through a borrower */
    }
    int physW = s_pvrTextureW[tpage];
    int physH = s_pvrTextureH[tpage];
    if (physW <= 0 || physH <= 0) {
        return;
    }

    /* Twiddle math requires square POT. Wallpapers go to 512x512, normal
     * tpages to 256x256 - both qualify. */
    if (physW != physH) {
        return;
    }
    unsigned int dim_log2 = 0;
    unsigned int d = (unsigned int)physW;
    while ((1u << dim_log2) < d) {
        dim_log2++;
    }
    if ((1u << dim_log2) != (unsigned int)physW) {
        return;
    }

    int stepR = (targetR << 16) / 31;
    int stepG = (targetG << 16) / 31;
    int stepB = (targetB << 16) / 31;
    int accR = 0x8000;
    int accG = 0x8000;
    int accB = 0x8000;

    int noKey = s_pvrNoColorKey[tpage];
    uint16_t gradient[32];
    for (int i = 0; i < 32; i++) {
        int r5 = accR >> 19;
        int g5 = accG >> 19;
        int b5 = accB >> 19;
        if (r5 < 0) {
            r5 = 0;
        } if (r5 > 31) {
            r5 = 31;
        }
        if (g5 < 0) {
            g5 = 0;
        }
        if (g5 > 31) {
            g5 = 31;
        }
        if (b5 < 0) {
            b5 = 0;
        }
        if (b5 > 31) {
            b5 = 31;
        }
        if (noKey) {
            int g6 = (g5 << 1) | (g5 >> 4);
            gradient[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
        else {
            gradient[i] = (uint16_t)(0x8000u | (r5 << 10) | (g5 << 5) | b5);
        }
        accR += stepR; accG += stepG; accB += stepB;
    }

    uint16_t *vram = (uint16_t *)s_pvrTextures[tpage];
    for (int y = 0; y < physH; y++) {
        for (int x = 0; x < physW; x++) {
            unsigned int idx = pvr_twiddle_sq((unsigned)x, (unsigned)y, dim_log2);
            uint16_t p = vram[idx];
            vram[idx] = gradient[p & 0x1F];
        }
    }
}

/* =====================================================================
 * Frame lifecycle
 * ===================================================================== */

static int s_pvrFrameInited = 0;

extern uint8_t  g_trDmaBuffer[];
#define __TR_VERTBUF_SIZE (512 * 1024)
int TR_VERTBUF_SIZE = 512 * 1024;

uint8_t __attribute__((aligned(32))) g_trDmaBuffer[__TR_VERTBUF_SIZE];

/* Build the reserved TPAGE_DRAIN store-queue drain texture: an 8x8 tpage whose
 * every texel is the color-key green, uploaded once and frozen so it stays
 * resident for the life of the program. Because all texels are the key colour,
 * any quad drawn from it is fully punched through (invisible) no matter where
 * it lands or what UVs it samples - even if its own final vertex block is the
 * one dropped. Emitting one such quad as the last PT primitive of a frame lets
 * its store-queue writes drain the preceding fade-iris tail transfer that would
 * otherwise be lost (the bug the FPS overlay used to mask). Exempt from both
 * tpage-reset loops (SetupD3DTexturesBegin / SetupMenuTexturesD3D).
 *
 * It has a second job: any tpage whose own pvr_mem_malloc fails borrows this
 * texture so it draws as nothing instead of sampling a null pointer. See
 * borrow_drain_tpage. Borrowers share this one allocation, which is why every
 * path that writes into tpage VRAM refuses to run on a borrowed page. */
void PVR_InitDrainTpage(void)
{
    const int t   = TPAGE_DRAIN;
    const int dim = 8;

    if (g_tpagePixelBuf[t] == NULL) {
        g_tpagePixelBuf[t] = malloc(dim * dim * 2);
        if (g_tpagePixelBuf[t] == NULL) {
            return;
        }
    }

    /* Native 16bpp packing is R5<<11 | G5<<6 | B5. Pure green (R0 G31 B0)
     * satisfies IS_COLOR_KEY_RGB5, so every texel becomes ARGB1555 alpha 0
     * and the PT alpha test discards it. */
    uint16_t *buf = (uint16_t *)g_tpagePixelBuf[t];
    for (int i = 0; i < dim * dim; i++) {
        buf[i] = (uint16_t)(0x1F << 6);
    }

    g_tpageWidth[t]      = dim;
    g_tpageHeight[t]     = dim;
    s_pvrNoColorKey[t]   = 0;   /* keyed: green must punch through */
    s_pvrKeepPixels[t]   = 0;   /* one-shot upload; system buffer may be freed */
    g_tpageStateArray[t] = 4;   /* loaded - DrawTexturedQuad self-gate passes */

    PVR_UploadTpage(t);         /* allocates VRAM, compiles keyed PT header, uploads */
    PVR_FreezeTpage(t);         /* never re-uploaded or invalidated */
}

/**
 * PVR_InitPad448Tpage — load PAD448.TEX into TPAGE_PAD448 at boot.
 *
 * PORT ADDITION. Letterbox filler for the 448-line split-screen modes; the
 * draw is in pad448.c, and net_transport.h records why the slot is exempt
 * from the tpage reset loops.
 *
 * The file is DTEX (the pvrtex output format), NOT raw pixels:
 *
 *   +0x00  char  magic[4]   "DTEX"
 *   +0x04  u16   width
 *   +0x06  u16   height
 *   +0x08  u32   type       PVR texture-control bits; 1<<27 = RGB565,
 *                           1<<26 would mean NONTWIDDLED and is clear here
 *   +0x0C  u32   size       payload bytes following the header
 *
 * So the payload is ALREADY TWIDDLED, and this cannot go through
 * g_tpagePixelBuf / PVR_UploadTpage the way the drain tpage above does —
 * that path twiddles on upload and would twiddle it a second time. Load the
 * payload verbatim with pvr_txr_load and register the slot by hand instead.
 */
void PVR_InitPad448Tpage(void)
{
#if !PAD448_ENABLE
    /* Superseded by the display origin-Y shift — see PAD448_ENABLE. */
    return;
#else
    const int t = TPAGE_PAD448;

    FILE *fp = fopen(PATH_PAD448, "rb");
    if (fp == NULL) {
        dbglog(DBG_WARNING, "Pad448: %s missing, bottom row stays black\n",
               PATH_PAD448);
        return;
    }

    unsigned char hdr[16];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
        hdr[0] != 'D' || hdr[1] != 'T' || hdr[2] != 'E' || hdr[3] != 'X')
    {
        dbglog(DBG_WARNING, "Pad448: %s is not a DTEX file\n", PATH_PAD448);
        fclose(fp);
        return;
    }

    int w        = (int)(hdr[4] | (hdr[5] << 8));
    int h        = (int)(hdr[6] | (hdr[7] << 8));
    uint32_t typ = (uint32_t)hdr[8]  | ((uint32_t)hdr[9]  << 8) |
                   ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    uint32_t sz  = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) |
                   ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);

    /* Only the one shape the draw assumes. The format field is bits 29:27,
     * where 1 is RGB565 (PVR_TXRFMT_RGB565 is 1<<27), and bit 26 set would
     * mean NONTWIDDLED. Anything else needs either an untwiddle or a
     * different header format, and the slot is registered as RGB565
     * unconditionally by compile_tpage_header — so an ARGB1555 or ARGB4444
     * payload would be silently reinterpreted. Refuse loudly instead. */
    if (w <= 0 || h <= 0 ||
        ((typ >> 27) & 7u) != 1u ||         /* not RGB565 */
        (typ & (1u << 26)) != 0 ||          /* not twiddled */
        sz != (uint32_t)(w * h * 2))
    {
        dbglog(DBG_WARNING,
               "Pad448: unsupported DTEX %dx%d type=%08lx size=%lu\n",
               w, h, (unsigned long)typ, (unsigned long)sz);
        fclose(fp);
        return;
    }

    void *staging = malloc(sz);
    if (staging == NULL) {
        fclose(fp);
        return;
    }
    size_t got = fread(staging, 1, sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        dbglog(DBG_WARNING, "Pad448: short read, %u of %lu bytes\n",
               (unsigned)got, (unsigned long)sz);
        free(staging);
        return;
    }

    pvr_ptr_t vram = pvr_mem_malloc(sz);
    if (vram == NULL) {
        dbglog(DBG_WARNING, "Pad448: no VRAM for %lu bytes\n",
               (unsigned long)sz);
        free(staging);
        return;
    }
    pvr_txr_load(staging, vram, sz);
    free(staging);

    s_pvrTextures[t]     = vram;
    s_pvrTextureW[t]     = w;
    s_pvrTextureH[t]     = h;
    s_pvrTextureDirty[t] = 0;
    g_tpageWidth[t]      = w;
    g_tpageHeight[t]     = h;

    /* Opaque, so compile_tpage_header picks RGB565 rather than ARGB1555 —
     * which is what the payload actually is. Set directly rather than via
     * PVR_SetNoColorKey: that calls PVR_UploadTpage, which would overwrite
     * the VRAM just filled with whatever is (not) in g_tpagePixelBuf. */
    s_pvrNoColorKey[t]   = 1;
    s_pvrKeepPixels[t]   = 0;
    g_tpageStateArray[t] = 4;   /* loaded */

    R_InvalidateTpageHeader(t);
    R_CompileTpageHeader(t);
    PVR_FreezeTpage(t);         /* resident for the life of the process */
#endif /* PAD448_ENABLE */
}

/* PVR-side vertex buffer allocation, in bytes. Named so pvr_init and the
 * budget probe cannot drift apart. Double-buffering halves it per frame. */
#ifdef SONICR_DC_240P
#define PVR_VERTBUF_BYTES ((2048+128) * 1024)
#else
#define PVR_VERTBUF_BYTES ((1536+256) * 1024)
#endif

/* Set to 1 to print VRAM headroom and per-frame submission budgets. */
#define PVR_BUDGET_PROBE 0

void PVR_Init(void)
{
    if (s_pvrFrameInited) {
        return;
    }
    pvr_init_params_t params = {
        /* OP, OP_MOD, TR, TR_MOD, PT */
        { 0, 0, PVR_BINSIZE_16, 0, PVR_BINSIZE_16 },
        PVR_VERTBUF_BYTES,
                      /* Vertex buffer size. DO NOT SHRINK.
                       *
                       * Double-buffering below is ON, so KOS halves this: the
                       * per-frame budget is 896K, not 1792K. That halving is
                       * why safe_pvr_vertbuf_tail checks TR_VERTBUF_SIZE / 2.
                       *
                       * If a Dreamcast crash ever looks like memory
                       * corruption, check this value first. */
        1,            /* 1 = DMA enabled (required for TR DMA submission) */
        0,            /* 0 = FSAA off */
        0,            /* 1 = autosort disabled */
        2,            /* Extra OPBs. */
        0,            /* 0 = vertex buffer double-buffer NOT disabled */
    };
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    /* PT alpha-test threshold: pixels with alpha < ref get discarded. KOS
     * leaves this at 0 by default, so ARGB1555 color-keyed texels (alpha=0)
     * still pass and render their RGB. Set to 0x80 so 1-bit alpha cleanly
     * splits - green keyed pixels (alpha 0x00) drop, opaque (0xFF) keep. */
    #define PT_ALPHA_REG  ((volatile uint32_t *)(0xa05f811c))
    *PT_ALPHA_REG = 0xFE;
    #undef PT_ALPHA_REG

    /* Wire the TR list to the system-RAM staging buffer. KOS DMAs this
     * into the TR list at pvr_scene_finish each frame. One-time call. */
    pvr_set_vertbuf(PVR_LIST_TR_POLY, g_trDmaBuffer, TR_VERTBUF_SIZE);

    compile_untex_header();

    /* NOTE: the reserved always-resident tpages — PVR_InitDrainTpage and
     * PVR_InitPad448Tpage — are deliberately NOT initialised here. Both have
     * to run after PVR_InitTextures, which zeroes s_pvrTextures, the frozen
     * flag and the no-color-key flag for every slot. See platform_dc.c. */

#if PVR_BUDGET_PROBE
    /* Free texture VRAM once pvr_init has taken the framebuffers, the OPBs
     * and the vertex buffer. Everything the game uploads has to fit in what
     * this reports; if it is small, texture allocations are already failing
     * and the drain-page fallback is quietly hiding it. */
    printf("[PVRCAP] init: %d KB texture VRAM free\n",
           (int)(pvr_mem_available() / 1024));
#endif

    s_pvrFrameInited = 1;
}

/* PVR autoflips at scene_finish, so we can only close one scene per visible
 * frame. The game engine, modeled on D3D/GL, calls BeginFrame/EndFrame
 * multiple times per visible frame (background pass, UI pass, etc.) and
 * uses FlipD3D as the actual present.  Coalesce all the engine's passes
 * into one PVR scene by deferring scene_finish to FlipFrame. */
static int s_pvrSceneOpen = 0;

extern pvr_dr_state_t g_drState;
extern void R_PvrFrameReset(void);
extern void R_SetTileClipFullScreen(void);

void PVR_BeginFrame(void)
{
    pvr_set_zclip(0);
    if (!s_pvrSceneOpen) {
        pvr_scene_begin();
        /* PT carries the bulk of the scene direct via pvr_dr_*; TR
         * appends to the system-RAM vertbuf and gets DMA'd at
         * scene_finish. Open the PT list explicitly so direct-render
         * SQ writes land in it. */
        pvr_list_begin(PVR_LIST_PT_POLY);
#if KOS_VERSION_BELOW(2, 3, 0)
        pvr_dr_init(&g_drState);
#endif
        R_PvrFrameReset();
        /* Start every scene from a known region — the lists are fresh, so
         * nothing carries over from last frame's final viewport. */
        R_SetTileClipFullScreen();
        s_pvrSceneOpen = 1;
    }
}

void PVR_EndFrame(void)
{
    /* Don't close the scene yet - FlipFrame does that.  Engine passes
     * within one game tick share a single PVR scene. */
}

#define DC_SHOW_FPS 1   /* dev FPS overlay - set to 1 to re-enable */

#if DC_SHOW_FPS
extern void FPS_Tick(void);
extern void FPS_Render(void);
#endif

#if PVR_BUDGET_PROBE
/* Vertex-buffer budget probe. Measures only — nothing is refused.
 *
 * pt/tr are the high-water submission counts from r_pvr_backend.c, in bytes
 * we pushed. vtx is what pvr_get_stats reports the TA actually occupied,
 * which is the number that has to stay under the allocation. The two are
 * not equal, and their ratio is the thing worth reading off this: it is
 * what any future PT cap has to be derived from.
 *
 * budget is the per-frame ceiling — the pvr_init allocation halved by
 * double-buffering. Printed every 60 flips, starting with the first, so a
 * hang leaves the last line before it on the terminal. */
extern void R_PvrBudgetPeaks(size_t *ptPeak, size_t *trPeak);

static void PVR_ReportBudget(void)
{
    static int frames = 0;

    if ((frames++ % 60) != 0) {
        return;
    }

    size_t ptPeak = 0, trPeak = 0;
    R_PvrBudgetPeaks(&ptPeak, &trPeak);

    pvr_stats_t st;
    pvr_get_stats(&st);

    printf("[PVRCAP] pt=%dK tr=%dK vtx last=%dK peak=%dK of %dK free=%dK vp=%d\n",
           (int)(ptPeak / 1024),
           (int)(trPeak / 1024),
           (int)(st.vtx_buffer_used / 1024),
           (int)(st.vtx_buffer_used_max / 1024),
           (int)(PVR_VERTBUF_BYTES / 2 / 1024),
           (int)(pvr_mem_available() / 1024),
           g_numViewports);
}
#endif

void PVR_FlipFrame(void)
{
    if (s_pvrSceneOpen) {
#if DC_SHOW_FPS
        /* The clip region is still the last viewport's — the FPS overlay is
         * full-screen and would be clipped into that quadrant. */
        R_SetTileClipFullScreen();
        FPS_Render();              /* draw FPS digits before closing scene */
#endif
#if KOS_VERSION_BELOW(2, 3, 0)
        pvr_dr_finish();
#endif
        pvr_list_finish();
        pvr_scene_finish();
        s_pvrSceneOpen = 0;
#if DC_SHOW_FPS
        FPS_Tick();                /* one tick per flipped frame */
#endif
#if PVR_BUDGET_PROBE
        PVR_ReportBudget();
#endif
    }
}

void PVR_ClearAndReset(void)
{
    /* Mirror SDL's state-6 cleanup at render_gl.c:740-758: when a tpage
     * is in state 6 (pending reload), free its VRAM allocation, drop any
     * pending RGBA staging buffer, invalidate the compiled header, and
     * transition to state 0 (idle). Without this, fresh content uploaded
     * over a long sequence (menu → race → results → credits → ...) keeps
     * accumulating PVR VRAM via pvr_mem_malloc with no path to release
     * the prior allocations. */
#if PVR_BUDGET_PROBE
    int freed = 0;
#endif
    for (int i = 0; i < MAX_TPAGES; i++) {
        if (g_tpageStateArray[i] != 6) {
            continue;
        }

        if (s_pvrTextures[i]) {
            release_tpage_vram(i);   /* frees, or un-borrows without freeing */
            s_pvrTextureW[i] = 0;
            s_pvrTextureH[i] = 0;
#if PVR_BUDGET_PROBE
            freed++;
#endif
        }
        if (s_pvrPendingRGBA[i]) {
            free(s_pvrPendingRGBA[i]);
            s_pvrPendingRGBA[i] = NULL;
        }
        s_pvrTextureDirty[i] = 1;
        s_pvrNoColorKey[i] = 0;
        s_pvrGreen6[i] = 0;
        s_pvrTextureFrozen[i] = 0;
        R_InvalidateTpageHeader(i);

        g_tpageStateArray[i] = 0;
    }

#if PVR_BUDGET_PROBE
    /* Fragmentation watch. Only report real teardowns — a track load frees
     * most of the tpage set, a menu transition frees a handful.
     *
     * pvr_mem_available() is the TOTAL free, which is exactly the number
     * that hides fragmentation: it stays healthy while the largest
     * contiguous block shrinks. pvr_mem_stats() prints dlmalloc-style
     * stats including the max free block, and that is the one to watch
     * across a SEQUENCE of different tracks. Reloading the same track
     * frees and re-takes identical sizes and will never show the problem.
     *
     * malloc_stats() covers main RAM, where g_tpagePixelBuf[] does the
     * same per-track malloc/free dance and can fragment the same way. */
    if (freed >= 8) {
        static int teardown = 0;
        printf("\n[VRAM] teardown #%d: freed %d tpages, %d KB total free\n",
               ++teardown, freed, (int)(pvr_mem_available() / 1024));
        pvr_mem_stats();
        malloc_stats();
    }
#endif
}

/* render_gl.c expects BeginFrame/EndFrame/FlipD3D/ProcessTpageStates symbols.
 * The DC build uses different names internally to avoid clashing with the
 * SDL build's render_gl.c symbols, but provide aliases for game code that
 * may still call the old names (some places bypass the R_* layer). */
void BeginFrame(void)
{
    PVR_BeginFrame();
}

void EndFrame(void)
{
    PVR_EndFrame();
}

void FlipD3D(void)
{
    PVR_FlipFrame();
}

void ProcessTpageStates(void) {
    PVR_ClearAndReset();
}
