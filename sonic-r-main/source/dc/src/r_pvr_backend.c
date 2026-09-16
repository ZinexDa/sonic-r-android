/**
 * r_pvr_backend.c — PowerVR (Dreamcast) implementation of the immediate-mode
 * render API defined in r_state.h, r_draw.h, r_texture.h.
 *
 * This is the DC counterpart to sdl/src/r_gl_backend.c. The R_* surface is
 * identical; only the underlying submission changes from glBegin/glVertex to
 * pvr_prim() with pvr_vertex_t.
 *
 * MVP scope: state tracking + queueing. Geometry is buffered into a per-frame
 * vertex pool and submitted at R_EndFrame. Lazy texture upload via render_pvr.c.
 *
 * NOTE: this is a scaffolding pass. PVR primitives are submitted but the
 * matrix/projection setup currently lives in render_pvr.c BeginFrame().
 */

#include <kos.h>
#include <dc/pvr.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "r_types.h"
#include "r_state.h"
#include "r_draw.h"
#include "r_texture.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"

/* =====================================================================
 * Internal state snapshot (mirrors r_gl_backend.c)
 * ===================================================================== */

typedef struct {
    int          textureId;
    R_BlendMode  blendMode;
    int          depthTest;
    R_DepthFunc  depthFunc;
    int          depthWrite;
    R_TexEnvMode texEnv;
    R_FilterMode filter;
    R_CullMode   cullMode;
    int          alphaTest;
    float        alphaRef;
    int          scissorEnabled;
    int          scissorX, scissorY, scissorW, scissorH;
} R_StateSnapshot;

static R_StateSnapshot s_desired;
static R_StateSnapshot s_current;

#define R_STATE_STACK_DEPTH 4
static R_StateSnapshot s_stateStack[R_STATE_STACK_DEPTH];
static int s_stackDepth = 0;

static const R_StateSnapshot s_defaults = {
    .textureId      = -1,
    .blendMode      = R_BLEND_ALPHA,
    .depthTest      = 1,
    .depthFunc      = R_DEPTH_LEQUAL,
    .depthWrite     = 1,
    .texEnv         = R_TEXENV_MODULATE,
    .filter         = R_FILTER_NEAREST,
    .cullMode       = R_CULL_NONE,
    .alphaTest      = 1,
    .alphaRef       = 0.01f,
    .scissorEnabled = 0,
    .scissorX       = 0, .scissorY = 0, .scissorW = 640, .scissorH = 480,
};

/* render_pvr.c-owned texture state */
extern pvr_ptr_t s_pvrTextures[]; /* per-tpage VRAM pointer */
extern int s_pvrTextureDirty[];   /* per-tpage dirty bit */
extern int s_pvrTextureW[];       /* texture width (pow2) */
extern int s_pvrTextureH[];       /* texture height (pow2) */
extern int s_pvrNoColorKey[];     /* per-tpage no-color-key flag */
extern void PVR_UploadTpage(int tpage);
extern void PVR_UploadTpageRGBA(int tpage, unsigned char *rgba, int w, int h);
extern void PVR_UploadTpageSubRect(int tpage, int x, int y, int w, int h);
extern void PVR_MarkTpageDirty(int tpage);
extern void PVR_ClearTpageDirty(int tpage);
extern void PVR_FreezeTpage(int tpage);
extern void PVR_ThawTpage(int tpage);
extern void PVR_SetPendingRGBA(int tpage, unsigned char *rgba, int w, int h);
extern void PVR_SetNoColorKey(int tpage);
extern void PVR_SetSatBoost(int tpage, int k256);
extern void PVR_SetTpageGreen6(int tpage, int on);
extern void PVR_ClearNoColorKey(int tpage);
extern void PVR_InitTextures(void);

/* =====================================================================
 * Polygon header cache.
 *
 * We rebuild the active poly header whenever state-affecting bits change
 * (texture, blend, depth, filter). The header is then submitted before the
 * next batch of vertices.
 * ===================================================================== */

/* Mirror render_pvr.c's count — kept in sync manually. */
#define MAX_TPAGES 52

/* Per-list header variants. Each tpage/sky/untex compiles two headers in
 * lockstep — one for the PT list, one for the TR list — so the hybrid
 * submitter can pick by list with no recompile in the hot path. */
#define R_HDR_PT 0
#define R_HDR_TR 1
#define R_HDR_COUNT 2

/* Hybrid submission state.
 *
 * PT-route prims go direct to the PT list via SQ writes (pvr_dr_target /
 * pvr_dr_commit). TR-route prims are appended to a system-RAM staging
 * buffer; KOS DMAs the buffer into the TR list at pvr_scene_finish.
 *
 * Each route maintains its own "header dirty" flag — switching routes
 * mid-frame doesn't force a header re-emit, only state changes do.
 * TR_WORSTCASE_SUBMISSION sizes the headroom check at one full prim.
 * The buffer half size is the usable portion per frame because KOS
 * double-buffers the vertex buffer.
 *
 * Worst case is 1 header + 6 verts — the most a near-clipped quad can produce
 * (in-out-in-out gives 2 kept + 4 intersections). There is no longer a
 * screen-space scissor pass to add more: the PVR user tile clip owns the
 * viewport edges. See PLAN_DC_HW_TILE_CLIP.md. */
extern int TR_VERTBUF_SIZE;
extern uint8_t __attribute__((aligned(32))) g_trDmaBuffer[];
#define TR_WORSTCASE_SUBMISSION (7 * 32)

extern int g_screenHeightBase;   /* init.c — framebuffer height, not render height */

pvr_dr_state_t g_drState = 0;
static size_t s_vbPos = 0;
static int s_trExhausted = 0;

static void *safe_pvr_vertbuf_tail(int list)
{
    if ((s_vbPos + TR_WORSTCASE_SUBMISSION) > (TR_VERTBUF_SIZE / 2)) {
        s_trExhausted = 1;
        return NULL;
    }

    return pvr_vertbuf_tail(list);
}

static void safe_pvr_vertbuf_written(int list, size_t amount)
{
    if (s_trExhausted) {
        return;
    }

    if ((s_vbPos + amount) > (TR_VERTBUF_SIZE / 2)) {
        s_trExhausted = 1;
        return;
    }

    s_vbPos += amount;
    pvr_vertbuf_written(list, amount);
}

/* =====================================================================
 * Submission accounting.
 *
 * Everything the PT route pushes goes through pt_submit, so s_ptBytes is
 * the exact byte count this frame handed to the TA. Nothing is refused
 * yet — this measures only. Enforcement, when it comes, is a single test
 * inside pt_submit rather than seven scattered ones.
 *
 * The budget it will eventually check against: the PVR-side vertex buffer
 * from pvr_init_params is halved by double-buffering, and TR can stage at
 * most TR_VERTBUF_SIZE / 2 per frame, so PT's share is
 * (vertex_buf_size / 2) - (TR_VERTBUF_SIZE / 2). Note that submitted bytes
 * are not the same as the bytes the TA then occupies — the ratio between
 * s_ptPeak and what pvr_get_stats reports is exactly what this is here to
 * establish, and the cap has to come from that measurement, not from the
 * submission count directly.
 * ===================================================================== */
static size_t s_ptBytes = 0;   /* PT bytes submitted this frame */
static size_t s_ptPeak  = 0;   /* high-water across all frames */
static size_t s_trPeak  = 0;   /* same for the TR staging buffer */

static inline void pt_submit(const void *src, int granules)
{
    s_ptBytes += (size_t)granules * 32u;
    sq_fast_cpy(SQ_MASK_DEST(PVR_TA_INPUT), src, granules);
}

void R_PvrBudgetPeaks(size_t *ptPeak, size_t *trPeak)
{
    *ptPeak = s_ptPeak;
    *trPeak = s_trPeak;
}

/* Per-route header dirty flags. State changes (R_SetTexture etc.) mark
 * BOTH dirty — each list keeps its own hardware state so each route's
 * header is re-emitted exactly once when it next draws after a state
 * change. Switching routes alone (no state change) doesn't force a
 * re-emit; the route's hardware state persists across the switch.
 *
 * Declared here rather than beside the header code below because
 * R_PvrFrameReset() has to invalidate them — see the comment there. */
static int s_ptHdrDirty = 1;
static int s_trHdrDirty = 1;

#define MARK_HDR_DIRTY() do { s_ptHdrDirty = 1; s_trHdrDirty = 1; } while (0)

void R_PvrFrameReset(void)
{
    /* Called at scene open, so the counters still hold last frame's totals. */
    if (s_ptBytes > s_ptPeak) {
        s_ptPeak = s_ptBytes;
    }
    if (s_vbPos > s_trPeak) {
        s_trPeak = s_vbPos;
    }

    s_ptBytes = 0;
    s_vbPos = 0;
    s_trExhausted = 0;

    /* Both routes start every scene with NO header in the stream, so the
     * "header is already in place" flags must be invalidated here — they are
     * a statement about the stream, and the stream just went away.
     *
     * TR is staged in g_trDmaBuffer and s_vbPos above rewinds it to offset 0;
     * PT goes straight to the TA and pvr_list_begin opens a fresh list. Either
     * way a stale clean flag lets emit_header_pt/emit_header_tr short-circuit
     * and submit vertices with no state in front of them — the exact failure
     * those functions guard against for the select_header_pair() == NULL case.
     *
     * Intermittent by nature: MARK_HDR_DIRTY() fires on almost any state
     * change, so churn usually re-dirties these between frames and hides it.
     * It bites when a route's first prim of the frame follows the previous
     * frame's last prim with no state change in between — e.g. when the only
     * translucent thing in the scene is the player's shield, which then goes
     * invisible until some other TR prim (ring-collect particles going
     * additive) dirties the flag and a header gets written again. */
    MARK_HDR_DIRTY();
}

/* Persistent per-texture headers — Wipeout DC pattern. Compile once when
 * the texture's VRAM ptr / dims / filter mode first becomes valid, patch
 * depth/blend bits in place per draw via PVR_TA_PM1/PM2 mask manipulation.
 * Way faster than pvr_poly_compile in the hot path. */
static pvr_poly_hdr_t s_tpageHdr[MAX_TPAGES][R_HDR_COUNT];
static int s_tpageHdrValid[MAX_TPAGES];
static pvr_ptr_t s_tpageHdrTxr[MAX_TPAGES];
static int s_tpageHdrW[MAX_TPAGES];
static int s_tpageHdrH[MAX_TPAGES];
static int s_tpageHdrFilter[MAX_TPAGES];

/* Per-tpage filter override. set[t]=0 means follow s_desired.filter;
 * set[t]=1 means use mode[t] regardless of global state. Used to keep
 * sprite/character atlases on point-sampling while the rest of the world
 * runs bilinear, avoiding atlas-edge bleed. */
static int s_pvrTpageFilterSet[MAX_TPAGES];
static R_FilterMode s_pvrTpageFilter[MAX_TPAGES];

static pvr_poly_hdr_t s_untexHdr[2];
static int s_untexHdrValid;

static void compile_tpage_header(int t, int filter);

void R_InvalidateTpageHeader(int tpage)
{
    if (tpage >= 0 && tpage < MAX_TPAGES) {
        s_tpageHdrValid[tpage] = 0;
        MARK_HDR_DIRTY();
    }
}

void R_CompileTpageHeader(int tpage)
{
    if (tpage < 0 || tpage >= MAX_TPAGES) {
        return;
    }
    if (!s_pvrTextures[tpage]) {
        return;
    }
    R_FilterMode mode = s_pvrTpageFilterSet[tpage]
                            ? s_pvrTpageFilter[tpage]
                            : s_desired.filter;
    int filter = (mode == R_FILTER_LINEAR)
                     ? PVR_FILTER_BILINEAR
                     : PVR_FILTER_NONE;
    compile_tpage_header(tpage, filter);
}

static int blendmode_to_pvr(R_BlendMode m, int *src, int *dst)
{
    /* Everything goes through TR for now. Opaque blend (ONE/ZERO) still
     * renders correctly inside the TR list — PVR just doesn't get to
     * optimize the way it would with the OP list. Once the rest of the
     * pipeline is verified, route R_BLEND_NONE back to PVR_LIST_OP_POLY
     * and open both lists in sequence per KOS's one-way constraint. */
    switch (m) {
        case R_BLEND_NONE:
            *src = PVR_BLEND_ONE;
            *dst = PVR_BLEND_ZERO;
            break;
        case R_BLEND_ALPHA:
            *src = PVR_BLEND_SRCALPHA;
            *dst = PVR_BLEND_INVSRCALPHA;
            break;
        case R_BLEND_ADDITIVE:
            *src = PVR_BLEND_SRCALPHA;
            *dst = PVR_BLEND_ONE;
            break;
        default:
            *src = PVR_BLEND_SRCALPHA;
            *dst = PVR_BLEND_INVSRCALPHA;
            break;
    }
    return PVR_LIST_TR_POLY;
}

/* One-time compile per (texture, filter) pair. Depth/blend left at defaults
 * here — they get patched per-draw in update_header. Format choice tracks
 * the per-tpage no-color-key flag: RGB565 for env-map / wallpaper / sky
 * (no alpha needed, full 6-bit green), ARGB1555 for color-keyed tpages.
 *
 * Compiles both PT and TR variants in lockstep so the hybrid submitter
 * can pick by list with no recompile in the hot path. */
static void compile_tpage_header(int t, int filter)
{
    pvr_poly_cxt_t cxt;
    int fmt = s_pvrNoColorKey[t]
                  ? (PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED)
                  : (PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_TWIDDLED);

    pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, fmt,
                     s_pvrTextureW[t], s_pvrTextureH[t],
                     s_pvrTextures[t], filter);

    /* KOS defaults PT txr.env to MODULATE (no alpha mod), which makes
     * PT's alpha test see only the vertex alpha — color-keyed texels
     * pass through. Force MODULATEALPHA so 1-bit texture alpha drives
     * the test and ARGB1555 keying actually punches green to clear. */
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.gen.culling = PVR_CULLING_SMALL;
    cxt.gen.specular = 1;
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;

    if (fmt == (PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_TWIDDLED)) {
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE; /* ENABLE is 0 in KOS */
        cxt.gen.alpha = PVR_ALPHA_ENABLE;    /* must be ENABLE so the
                                              * output stage honors the
                                              * 1-bit texture alpha for
                                              * color-key punch-through */
    }

    /* Every header honours the user tile clip; the CLIP REGION is what varies.
     * Set per viewport by R_SetTileClip(), and widened to the full framebuffer
     * for full-screen work. Doing it this way keeps the header cache free of a
     * per-viewport dimension. See PLAN_DC_HW_TILE_CLIP.md. */
    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    pvr_poly_compile(&s_tpageHdr[t][R_HDR_PT], &cxt);

    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, fmt,
                     s_pvrTextureW[t], s_pvrTextureH[t],
                     s_pvrTextures[t], filter);

    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.gen.culling = PVR_CULLING_SMALL;
    cxt.gen.specular = 1;
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    if (fmt == (PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_TWIDDLED)) {
        cxt.txr.alpha = PVR_TXRALPHA_ENABLE; /* ENABLE is 0 in KOS */
        cxt.gen.alpha = PVR_ALPHA_ENABLE;    /* must be ENABLE so the
                                              * output stage honors the
                                              * 1-bit texture alpha for
                                              * color-key punch-through */
    }

    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    pvr_poly_compile(&s_tpageHdr[t][R_HDR_TR], &cxt);

    s_tpageHdrValid[t] = 1;
    s_tpageHdrTxr[t] = s_pvrTextures[t];
    s_tpageHdrW[t] = s_pvrTextureW[t];
    s_tpageHdrH[t] = s_pvrTextureH[t];
    s_tpageHdrFilter[t] = filter;
}

void compile_untex_header(void)
{
    pvr_poly_cxt_t cxt;

    pvr_poly_cxt_col(&cxt, PVR_LIST_PT_POLY);
    cxt.gen.culling = PVR_CULLING_SMALL;
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    pvr_poly_compile(&s_untexHdr[R_HDR_PT], &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.culling = PVR_CULLING_SMALL;
    cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    cxt.gen.clip_mode = PVR_USERCLIP_INSIDE;
    pvr_poly_compile(&s_untexHdr[R_HDR_TR], &cxt);

    s_untexHdrValid = 1;
}

/* Pick the right cached header pair for current state. Returns the
 * 2-element array (PT at [R_HDR_PT], TR at [R_HDR_TR]). Caller selects
 * the variant by route. Recompiles if filter changed since last compile.
 *
 * Returns NULL if texture not uploaded (header compiled at upload time). */
static pvr_poly_hdr_t *select_header_pair(void)
{
    if (s_desired.textureId >= 0) {
        int t = s_desired.textureId;
        if (t >= MAX_TPAGES) {
            return NULL;
        }
        if (!s_pvrTextures[t]) {
            return NULL;
        }
        if (!s_tpageHdrValid[t]) {
            return NULL;
        }
        R_FilterMode mode = s_pvrTpageFilterSet[t]
                                ? s_pvrTpageFilter[t]
                                : s_desired.filter;
        int filter = (mode == R_FILTER_LINEAR)
                         ? PVR_FILTER_BILINEAR
                         : PVR_FILTER_NONE;
        if (s_tpageHdrFilter[t] != filter) {
            compile_tpage_header(t, filter);
        }
        return s_tpageHdr[t];
    }
    return s_untexHdr;
}

/* Patch one header in place with the current depth/blend bits. Used per
 * route — PT and TR variants share state mostly but live at different
 * memory addresses (one inside [R_HDR_PT], one inside [R_HDR_TR]). */
static inline void patch_header_bits(pvr_poly_hdr_t *hdr)
{
    int srcBlend, dstBlend;
    blendmode_to_pvr(s_desired.blendMode, &srcBlend, &dstBlend);

    uint32_t *hp = (uint32_t *)hdr;
    uint32_t h1 = hp[1];
    uint32_t h2 = hp[2];

    h1 = (h1 & ~PVR_TA_PM1_DEPTHCMP_MASK)
       | ((uint32_t)(s_desired.depthTest ? PVR_DEPTHCMP_GEQUAL
                                          : PVR_DEPTHCMP_ALWAYS)
          << PVR_TA_PM1_DEPTHCMP_SHIFT);

    h1 = (h1 & ~PVR_TA_PM1_DEPTHWRITE_MASK)
       | ((uint32_t)(s_desired.depthWrite ? PVR_DEPTHWRITE_ENABLE
                                           : PVR_DEPTHWRITE_DISABLE)
          << PVR_TA_PM1_DEPTHWRITE_SHIFT);

    h2 = (h2 & ~(PVR_TA_PM2_SRCBLEND_MASK | PVR_TA_PM2_DSTBLEND_MASK))
       | ((uint32_t)srcBlend << PVR_TA_PM2_SRCBLEND_SHIFT)
       | ((uint32_t)dstBlend << PVR_TA_PM2_DSTBLEND_SHIFT);

/*     h2 = (h2 & ~PVR_TA_PM2_TXRENV_MASK)
         | ((uint32_t)PVR_TXRENV_REPLACE << PVR_TA_PM2_TXRENV_SHIFT); */

    hp[1] = h1;
    hp[2] = h2;
}

/* =====================================================================
 * Frame lifecycle (PVR list management lives here; render_pvr.c handles
 * the projection/viewport setup that sits "before" the list).
 * ===================================================================== */

extern void PVR_BeginFrame(void);
extern void PVR_EndFrame(void);
extern void PVR_FlipFrame(void);
extern void PVR_ClearAndReset(void);

void R_EndFrame(void)
{
    PVR_EndFrame();
}

void R_Flip(void)
{
    PVR_FlipFrame();
}

void R_ClearAndReset(void)
{
    PVR_ClearAndReset();
}

void R_ClearDepth(void)
{
    /* PVR has no separate depth clear in TR list flow */
}

/* =====================================================================
 * State setters (no immediate effect; queued until R_FlushState/draw).
 * ===================================================================== */

void R_SetTexture(int t)
{
    if (s_desired.textureId != t) {
        s_desired.textureId = t;
        MARK_HDR_DIRTY();
    }
}

void R_SetBlendMode(R_BlendMode m)
{
    if (s_desired.blendMode != m) {
        s_desired.blendMode = m;
        MARK_HDR_DIRTY();
    }
}

void R_SetDepthTest(int e)
{
    if (s_desired.depthTest != e) {
        s_desired.depthTest = e;
        MARK_HDR_DIRTY();
    }
}

void R_SetDepthFunc(R_DepthFunc f)
{
    if (s_desired.depthFunc != f) {
        s_desired.depthFunc = f;
        MARK_HDR_DIRTY();
    }
}

void R_SetDepthWrite(int e)
{
    if (s_desired.depthWrite != e) {
        s_desired.depthWrite = e;
        MARK_HDR_DIRTY();
    }
}

void R_SetTexEnv(R_TexEnvMode m)
{
    s_desired.texEnv = m;
}

void R_SetFilter(R_FilterMode m)
{
    if (s_desired.filter != m) {
        s_desired.filter = m;
        MARK_HDR_DIRTY();
    }
}

void R_SetTpageFilter(int t, R_FilterMode m)
{
    if (t < 0 || t >= MAX_TPAGES) {
        return;
    }
    s_pvrTpageFilterSet[t] = 1;
    s_pvrTpageFilter[t] = m;
    MARK_HDR_DIRTY();
}

void R_SetTpageSatBoost(int t, int k256)
{
    PVR_SetSatBoost(t, k256);
}

void R_ClearTpageFilter(int t)
{
    if (t < 0 || t >= MAX_TPAGES) {
        return;
    }
    if (s_pvrTpageFilterSet[t]) {
        s_pvrTpageFilterSet[t] = 0;
        MARK_HDR_DIRTY();
    }
}

void R_SetCullMode(R_CullMode m)
{
    s_desired.cullMode = m; /* PVR culling isn't used */
}

void R_SetAlphaTest(int e)
{
    s_desired.alphaTest = e;
}

void R_SetAlphaRef(float r)
{ 
    s_desired.alphaRef = r;
}

void R_SetScissor(int x, int y, int w, int h)
{
    s_desired.scissorEnabled = 1;
    s_desired.scissorX = x;
    s_desired.scissorY = y;
    s_desired.scissorW = w;
    s_desired.scissorH = h;
}

void R_DisableScissor(void)
{
    s_desired.scissorEnabled = 0;
}

void R_PushState(void)
{
    if (s_stackDepth < R_STATE_STACK_DEPTH) {
        s_stateStack[s_stackDepth++] = s_desired;
    }
}

void R_PopState(void)
{
    if (s_stackDepth > 0) {
        const R_StateSnapshot *popped = &s_stateStack[--s_stackDepth];
        /* Only mark dirty if a header-affecting field actually changed.
         * Push/Pop pairs around code that didn't touch state (or pushed/
         * popped identical state) used to force a header re-emit on every
         * pop — that adds up fast in nested draw paths. */
        if (popped->textureId  != s_desired.textureId  ||
            popped->blendMode  != s_desired.blendMode  ||
            popped->depthTest  != s_desired.depthTest  ||
            popped->depthWrite != s_desired.depthWrite ||
            popped->filter     != s_desired.filter)
        {
            MARK_HDR_DIRTY();
        }
        s_desired = *popped;
    }
}

void R_ResetState(void)
{
    s_desired = s_defaults;
    memset(&s_current, 0xFF, sizeof(s_current));
    s_current.textureId = -99;
    s_current.alphaRef = -1.0f;
    s_stackDepth = 0;
    MARK_HDR_DIRTY();
}

void R_FlushState(void)
{
    /* On SDL/GL this committed GL state before non-draw ops (glClear etc.).
     * On PVR there are no such ops — state is meaningful only at draw time
     * and emit_header_pt/emit_header_tr commit it then. Doing it here
     * would clear the per-route dirty flags without a paired emit,
     * causing the next draw to skip its header submit and render under
     * the previous draw's state. So this is intentionally a no-op. */
}

/* =====================================================================
 * Vertex emission.
 *
 * Game-side RenderVertex stores screen-space (sx,sy) in pixel units,
 * sz already normalized to [0,1] for the depth buffer, and rhw =
 * 1/W (pre-divided coordinates). PVR wants the same pre-divided form
 * for pre-transformed verts: x,y in screen pixels, z = 1/w (since the
 * PVR uses 1/w as its depth metric, larger = closer with GEQUAL).
 *
 * We pass sx,sy as-is and use rhw for z. UVs are passed through.
 * ===================================================================== */

/* Field copy from RenderVertex into a pvr_vertex_t at dst. No submission
 * — caller is responsible for committing dst (PT: pvr_dr_commit; TR:
 * safe_pvr_vertbuf_written). */
static inline void fill_vertex(pvr_vertex_t *pv, const RenderVertex *v, uint32_t flags)
{
    pv->flags = flags;
    pv->x = v->sx;
    pv->y = v->sy;
    pv->z = (v->rhw > 0.0f) ? v->rhw : 0.00001f;
    pv->u = v->u;
    pv->v = v->v;
    uint32_t c = v->color;
    uint32_t cr = (c >> 16) & 0xff;
    uint32_t cg = (c >> 8) & 0xff;
    uint32_t cb = (c) & 0xff;

    cr = cr + (cr >> 2);
    cg = cg + (cg >> 2);
    cb = cb + (cb >> 2);

    if (cr > 255) {
        cr = 255;
    }
    if (cg > 255) {
        cg = 255;
    }
    if (cb > 255) {
        cb = 255;
    }

    pv->argb = (v->color & 0xff000000) | (cr << 16) | (cg << 8) | cb;
    /* Offset color (added by the TSP after texture-modulate; headers ship
     * specular=1). All shipping geometry leaves specular=0 -> oargb 0 (a
     * no-op, unchanged behavior); the water band uses it for its teal
     * depth ramp so the tint stays on the opaque PT prim. */
    pv->oargb = v->specular;
}

/* One-pass Add Signed (`texture + colour - 0.5`) for PVR, which has no such
 * combine mode. Split the vertex colour at the 0x7F midpoint across the two
 * colour slots — the remainder modulates, the surplus is added by the TSP:
 *
 *     excess = max(chan - 0x7F, 0)
 *     oargb  = excess - (excess >> 2)     surplus, scaled 3/4
 *     argb   = (chan - excess) + 0x80     remainder, biased into [0x80,0xFF]
 *     result = texture * argb + oargb     (headers ship specular = 1)
 *
 * VERTEX_WHITE 0xE0E0E0 lands on argb 0xFFFFFF / oargb 0x494949, i.e. modulate
 * becomes a no-op and the prim gets a flat +0x49 additive lift. Plain MODULATE
 * on the same vertex is texture * 0.88 instead — a big enough gap that any draw
 * which inherits the wrong tex-env is obvious on dark content.
 *
 * This is NOT modulate-2x. Nothing here doubles anything. See the memory note
 * project_dc_add_signed. */
static inline void split_add_signed_color(uint32_t c, uint32_t *bc, uint32_t *oc)
{
    uint32_t cr = (c >> 16) & 0xff;
    uint32_t cg = (c >> 8) & 0xff;
    uint32_t cb = (c) & 0xff;

    int32_t br, bg, bb, ocr, ocg, ocb;
#if 0
    br = cr > 0x7F ? 0x7F : cr;
    ocr = cr > 0x7F ? (cr - 0x7F) : 0;

    bg = cg > 0x7F ? 0x7F : cg;
    ocg = cg > 0x7F ? (cg - 0x7F) : 0;

    bb = cb > 0x7F ? 0x7F : cb;
    ocb = cb > 0x7F ? (cb - 0x7F) : 0;
#endif
    int dr = cr - 0x7F;
    int dg = cg - 0x7F;
    int db = cb - 0x7F;

    ocr = dr > 0 ? dr : 0;
    ocg = dg > 0 ? dg : 0;
    ocb = db > 0 ? db : 0;

    br = cr - ocr;
    bg = cg - ocg;
    bb = cb - ocb;

    br = br + 0x80;
    bg = bg + 0x80;
    bb = bb + 0x80;

//    br -= 0x30;
  //  bg -= 0x30;
    //bb -= 0x30;

//    if (br < 0) br = 0;
  //  if (bg < 0) bg = 0;
    //if (bb < 0) bb = 0;
    //br = (br << 1) + br;
    //bg = (bg << 1) + bg;
    //bb = (bb << 1) + bb;

//    br = 0xe0 - (0x7f - br);
  //  bg = 0xe0 - (0x7f - bg);
    //bb = 0xe0 - (0x7f - bb);

//    br += 0x3f;
  //  bg += 0x3f;
    //bb += 0x3f;
#if 0
    if (br < 0x40 && bg < 0x40 && bb < 0x40) {
            br <<= 1;
            bg <<= 1;
            bb <<= 1;
    }
#endif

    ocr -= (ocr >> 2);
    ocg -= (ocg >> 2);
    ocb -= (ocb >> 2);

    *bc  = (c & 0xff000000) | (br << 16) | (bg << 8) | (bb << 0);
    *oc = (ocr << 16) | (ocg << 8) | ocb;
}

static inline void fill_vertex_add_signed(pvr_vertex_t *pv, const RenderVertex *v, uint32_t flags)
{
    pv->flags = flags;
    pv->x = v->sx;
    pv->y = v->sy;
    pv->z = (v->rhw > 0.0f) ? v->rhw : 0.00001f;
    pv->u = v->u;
    pv->v = v->v;

    split_add_signed_color(v->color, &pv->argb, &pv->oargb);
}

static inline int texenv_is_add_signed(void)
{
    return s_desired.texEnv == R_TEXENV_ADD_SIGNED;
}

/* PT-route header emit: patch the PT variant and SQ-write it into the
 * active PT list via direct render.
 *
 * Returns 1 when it's safe for the caller to submit verts, 0 when the
 * caller MUST drop the prim. The two reasons for 0:
 *   - select_header_pair() returned NULL (texture not uploaded, or tpage
 *     out of range) AND the route is dirty — we have no header to emit
 *     and submitting verts now would land them under the previous prim's
 *     state (TA wedge / wrong-format reinterpret).
 *   - vertbuf exhaustion at header append time (TR variant only).
 * Returns 1 when the route was already clean (header still in place from
 * a prior prim, OK to submit). */
static inline int emit_header_pt(void)
{
    if (!s_ptHdrDirty) {
        return 1;
    }

    pvr_poly_hdr_t *pair = select_header_pair();
    if (!pair) {
        return 0;
    }

    pvr_poly_hdr_t *hdr = &pair[R_HDR_PT];
    patch_header_bits(hdr);

    pt_submit(hdr, 1);

    s_ptHdrDirty = 0;
    return 1;
}

/* TR-route header emit: patch the TR variant and append it to the
 * RAM-staged vertbuf. KOS DMAs the buffer into the TR list at
 * pvr_scene_finish. Same return semantics as emit_header_pt(). */
static inline int emit_header_tr(void)
{
    if (!s_trHdrDirty) {
        return 1;
    }

    pvr_poly_hdr_t *pair = select_header_pair();
    if (!pair) {
        return 0;
    }

    pvr_poly_hdr_t *hdr = &pair[R_HDR_TR];
    patch_header_bits(hdr);

    pvr_poly_hdr_t *dst = (pvr_poly_hdr_t *)safe_pvr_vertbuf_tail(PVR_LIST_TR_POLY);
    if (!dst) {
        return 0;
    }

    *dst = *hdr;
    safe_pvr_vertbuf_written(PVR_LIST_TR_POLY, sizeof(pvr_poly_hdr_t));

    s_trHdrDirty = 0;
    return 1;
}

/* Routing decision: any vertex with alpha < 255 forces the prim onto
 * the TR list (translucent blending requires sorting). All-opaque
 * verts route to PT, where they kill depth behind them and don't need
 * sorting. We scan all N verts because per-vertex fog can introduce
 * fade on individual verts that vertex 0 wouldn't show.
 *
 * Additive blend ALWAYS needs TR regardless of vertex alpha — PT is
 * alpha-test only, no blending stage, so an additive-on-opaque sparkle
 * routed to PT would draw as a flat-opaque sprite with no glow. */

#define DC_TR_ALPHA_THRESH 224u

 static inline int prim_needs_tr(const RenderVertex *vs, int n)
{
    if (s_desired.blendMode == R_BLEND_ADDITIVE) {
        return 1;
    }
    for (int i = 0; i < n; i++) {
        unsigned a = (vs[i].color >> 24) & 0xFFu;
        a = a + (a >> 4);
        if (a > 255) {
            a = 255;
        }
        if (a < 255) {
            return 1;
        }
    }
    return 0;
}

static inline int pvr_prim_needs_tr(const pvr_vertex_t *vs, int n)
{
    if (s_desired.blendMode == R_BLEND_ADDITIVE) {
        return 1;
    }
    for (int i = 0; i < n; i++) {
        unsigned a = (vs[i].argb >> 24) & 0xFFu;
        a = a + (a >> 4);
        if (a < 255) {
            return 1;
        }
    }
    return 0;
}

/* Per-call PT staging buffer — verts get built here then bulk-submitted
 * via pvr_prim (which sq_cpy's the whole region in one tight inner loop,
 * saving the per-vert pvr_dr_target/pvr_dr_commit macro overhead).
 * Sized for the worst-case clipped-quad output (6 verts) plus headroom.
 * Static + aligned because SH4 GCC's stack alignment isn't reliable past
 * the ABI default and SQ writes need 32-byte aligned destinations. */
/* static */ __attribute__((aligned(32))) pvr_vertex_t s_ptScratch[16];

void R_DrawTri(const RenderVertex v[3])
{
    if (texenv_is_add_signed()) {
        fill_vertex_add_signed(&s_ptScratch[0], &v[0], PVR_CMD_VERTEX);
        fill_vertex_add_signed(&s_ptScratch[1], &v[1], PVR_CMD_VERTEX);
        fill_vertex_add_signed(&s_ptScratch[2], &v[2], PVR_CMD_VERTEX_EOL);
    }
    else {
        fill_vertex(&s_ptScratch[0], &v[0], PVR_CMD_VERTEX);
        fill_vertex(&s_ptScratch[1], &v[1], PVR_CMD_VERTEX);
        fill_vertex(&s_ptScratch[2], &v[2], PVR_CMD_VERTEX_EOL);
    }

    if (prim_needs_tr(v, 3)) {
        if (!emit_header_tr()) {
            return;
        }
        pvr_vertex_t *dst = (pvr_vertex_t *)safe_pvr_vertbuf_tail(PVR_LIST_TR_POLY);
        if (!dst) {
            return;
        }
        memcpy(dst, s_ptScratch, 3 * sizeof(pvr_vertex_t));
#ifdef SONICR_DC_240P
        for (int i = 0; i < 3; i++) {
            dst[i].x *= 0.5f;
            dst[i].y *= 0.5f;
        }
#endif
        safe_pvr_vertbuf_written(PVR_LIST_TR_POLY, 3 * sizeof(pvr_vertex_t));
    }
    else {
        if (!emit_header_pt()) {
            return;
        }
#ifdef SONICR_DC_240P
        for (int i = 0; i < 3; i++) {
            s_ptScratch[i].x *= 0.5f;
            s_ptScratch[i].y *= 0.5f;
        }
#endif
        pt_submit(s_ptScratch, 3);
    }
}

/* Callers always pass s_ptScratch (or another writable per-call buffer)
 * after building verts in place. v is non-const so argb can be mutated
 * without an extra memcpy. */
void R_DrawPvrTri(pvr_vertex_t *v)
{
    if (pvr_prim_needs_tr(v, 3)) {
        pvr_vertex_t *dst;
        if (!emit_header_tr()) {
            return;
        }
        dst = (pvr_vertex_t *)safe_pvr_vertbuf_tail(PVR_LIST_TR_POLY);
        if (!dst) {
            return;
        }
        memcpy(dst, v, 3 * sizeof(pvr_vertex_t));
#ifdef SONICR_DC_240P
        for (int i = 0; i < 3; i++) {
            dst[i].x *= 0.5f;
            dst[i].y *= 0.5f;
        }
#endif
        safe_pvr_vertbuf_written(PVR_LIST_TR_POLY, 3 * sizeof(pvr_vertex_t));
    }
    else {
        if (!emit_header_pt()) {
            return;
        }
#ifdef SONICR_DC_240P
        for (int i = 0; i < 3; i++) {
            v[i].x *= 0.5f;
            v[i].y *= 0.5f;
        }
#endif
        pt_submit(v, 3);
    }
}

void R_DrawPvrQuad(pvr_vertex_t *v)
{
    if (pvr_prim_needs_tr(v, 4)) {
        pvr_vertex_t *dst;
        if (!emit_header_tr()) {
            return;
        }
        dst = (pvr_vertex_t *)safe_pvr_vertbuf_tail(PVR_LIST_TR_POLY);
        if (!dst) {
            return;
        }
        memcpy(dst, v, 4 * sizeof(pvr_vertex_t));
#ifdef SONICR_DC_240P
        for (int i = 0; i < 4; i++) {
            dst[i].x *= 0.5f;
            dst[i].y *= 0.5f;
        }
#endif
        safe_pvr_vertbuf_written(PVR_LIST_TR_POLY, 4 * sizeof(pvr_vertex_t));
    }
    else {
        if (!emit_header_pt()) {
            return;
        }
#ifdef SONICR_DC_240P
        for (int i = 0; i < 4; i++) {
            v[i].x *= 0.5f;
            v[i].y *= 0.5f;
        }
#endif
        pt_submit(v, 4);
    }
}

void R_DrawQuad(const RenderVertex v[4])
{
    /* PVR strip vertex order: 0,1,3,2 from a quad with verts ordered TL,TR,BR,BL */
    if (texenv_is_add_signed()) {
        fill_vertex_add_signed(&s_ptScratch[0], &v[0], PVR_CMD_VERTEX);
        fill_vertex_add_signed(&s_ptScratch[1], &v[1], PVR_CMD_VERTEX);
        fill_vertex_add_signed(&s_ptScratch[2], &v[3], PVR_CMD_VERTEX);
        fill_vertex_add_signed(&s_ptScratch[3], &v[2], PVR_CMD_VERTEX_EOL);
    }
    else {
        fill_vertex(&s_ptScratch[0], &v[0], PVR_CMD_VERTEX);
        fill_vertex(&s_ptScratch[1], &v[1], PVR_CMD_VERTEX);
        fill_vertex(&s_ptScratch[2], &v[3], PVR_CMD_VERTEX);
        fill_vertex(&s_ptScratch[3], &v[2], PVR_CMD_VERTEX_EOL);
    }

    if (prim_needs_tr(v, 4)) {
        if (!emit_header_tr()) {
            return;
        }
        pvr_vertex_t *dst = (pvr_vertex_t *)safe_pvr_vertbuf_tail(PVR_LIST_TR_POLY);
        if (!dst) {
            return;
        }
        memcpy(dst, s_ptScratch, 4 * sizeof(pvr_vertex_t));
#ifdef SONICR_DC_240P
        for (int i = 0; i < 4; i++) {
            dst[i].x *= 0.5f;
            dst[i].y *= 0.5f;
        }
#endif
        safe_pvr_vertbuf_written(PVR_LIST_TR_POLY, 4 * sizeof(pvr_vertex_t));
    }
    else {
        if (!emit_header_pt()) {
            return;
        }
#ifdef SONICR_DC_240P
        for (int i = 0; i < 4; i++) {
            s_ptScratch[i].x *= 0.5f;
            s_ptScratch[i].y *= 0.5f;
        }
#endif
        pt_submit(s_ptScratch, 4);
    }
}

void R_DrawTriFan(const RenderVertex *v, int count)
{
    if (count < 3) {
        return;
    }
    /* Decompose fan into individual triangles for PVR (no native fan list). */
    for (int i = 1; i < count - 1; i++) {
        RenderVertex tri[3] = { v[0], v[i], v[i+1] };
        R_DrawTri(tri);
    }
}

/* Submit a caller-prebuilt PVR strip of `count` verts (3..N). Caller is
 * responsible for setting flags (PVR_CMD_VERTEX on all but last,
 * PVR_CMD_VERTEX_EOL on last) and laying verts out in PVR strip order.
 * Generalizes R_DrawPvrTri / R_DrawPvrQuad for variable-count cases like
 * the track-quad near-plane clip output (3, 4, or 5 verts). */
void R_DrawPvrStrip(pvr_vertex_t *v, int count)
{
    if (count < 3) {
        return;
    }

    int needs_tr = (s_desired.blendMode == R_BLEND_ADDITIVE);
    if (!needs_tr) {
        for (int i = 0; i < count; i++) {
            if (((v[i].argb >> 24) & 0xFFu) < 255u) {
                needs_tr = 1;
                break;
            }
        }
    }

    size_t bytes = (size_t)count * sizeof(pvr_vertex_t);

    if (needs_tr) {
        if (!emit_header_tr()) {
            return;
        }
        pvr_vertex_t *dst = (pvr_vertex_t *)safe_pvr_vertbuf_tail(PVR_LIST_TR_POLY);
        if (!dst) {
            return;
        }
        memcpy(dst, v, bytes);
#ifdef SONICR_DC_240P
        for (int i = 0; i < count; i++) {
            dst[i].x *= 0.5f;
            dst[i].y *= 0.5f;
        }
#endif
        safe_pvr_vertbuf_written(PVR_LIST_TR_POLY, bytes);
    }
    else {
        if (!emit_header_pt()) {
            return;
        }
#ifdef SONICR_DC_240P
        for (int i = 0; i < count; i++) {
            v[i].x *= 0.5f;
            v[i].y *= 0.5f;
        }
#endif
        pt_submit(v, bytes >> 5);
    }
}

/* =====================================================================
 * Direct-write API for hot-path callers (TrackClipAndEmitQuad_DC etc.)
 * that know PT vs TR before building verts and want to skip the memcpy.
 * ===================================================================== */

int R_EmitHeader(int pvr_list)
{
    return (pvr_list == PVR_LIST_TR_POLY) ? emit_header_tr() : emit_header_pt();
}

pvr_vertex_t *R_TrVertbufTail(void)
{
    return (pvr_vertex_t *)safe_pvr_vertbuf_tail(PVR_LIST_TR_POLY);
}

void R_TrVertbufWritten(size_t bytes)
{
    safe_pvr_vertbuf_written(PVR_LIST_TR_POLY, bytes);
}

/* =====================================================================
 * PVR user tile clip — hardware split-screen viewport clipping.
 *
 * Every compiled header carries PVR_USERCLIP_INSIDE, so the region set
 * here is what varies: per viewport during the split-screen pass, and
 * widened to the whole framebuffer for anything full-screen.
 *
 * Because nothing opts out any more, the region is global state that must
 * be correct at ALL times — a missed reset leaves later drawing clipped
 * into the previous viewport. Reset points are in hud_full.c (before the
 * background, and per viewport) and PVR_FlipFrame (before the FPS overlay).
 *
 * Coordinates are INCLUSIVE TILE indices, 32 pixels per tile: 640x480 is
 * (0,0)-(19,14), a 640x448 top viewport is (0,0)-(19,6).
 *
 * See PLAN_DC_HW_TILE_CLIP.md.
 * ===================================================================== */

typedef struct {
    uint32_t flags;             /* PVR_CMD_USERCLIP */
    uint32_t d1, d2, d3;        /* ignored for this command type */
    uint32_t sx, sy, ex, ey;    /* inclusive tile indices */
} pvr_tile_clip_cmd_t;

/* Previous clip region, in INCLUSIVE TILE indices. The erratum dummy has to
 * land inside the region that is still in force, i.e. the one before this
 * call, so it is consumed under the old clip rather than the new one.
 * Initialised to the full framebuffer, which is what the first call sees. */
static int s_prevClip[4] = { 0, 0, 63, 63 };

/* ---------------------------------------------------------------------------
 * USERCLIP erratum workaround.
 *
 * On this hardware a user tile clip command does not take effect for the
 * primitive that immediately follows it — the next real geometry is still
 * rasterised under the PREVIOUS region. Submitting one throwaway polygon
 * between the clip command and the real geometry absorbs it.
 *
 * The bug this fixes: in split-screen the first primitive after each viewport's
 * clip change got the other viewport's region. It showed up as a player's own
 * shield vanishing from their view and appearing in the other player's half,
 * still sized and positioned as if attached to its owner — geometry from one
 * viewport rasterised under another viewport's clip. It was intermittent
 * because ANY other primitive landing in between (ring-collect particles, from
 * either player) absorbed the erratum instead, which is also why it never
 * happened in single player: the region is set once per scene and never
 * changes, so nothing ever follows a clip change.
 *
 * The dummy is a 2-pixel alpha-0 triangle inside the OLD region, carrying its
 * own untextured header. Emitting a header invalidates the cached header
 * state, so both routes are marked dirty afterwards — otherwise the next real
 * primitive would skip its own header and inherit this one.
 * ------------------------------------------------------------------------- */
static void emit_clip_dummy(void)
{
    static pvr_vertex_t __attribute__((aligned(32))) dummy[3];

    /* Top-left corner of the still-active region, in pixels. */
    float x0 = (float)(s_prevClip[0] * 32);
    float y0 = (float)(s_prevClip[1] * 32);

    for (int i = 0; i < 3; i++) {
        dummy[i].flags = (i == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        dummy[i].u = 0.0f;
        dummy[i].v = 0.0f;
        dummy[i].argb = 0x00000000;   /* alpha 0 — contributes nothing */
        dummy[i].oargb = 0;
        /* Near enough to pass a GEQUAL depth test against scene geometry
         * (whose rhw runs ~1e-3 and below) so it is genuinely rasterised —
         * a primitive rejected before rasterisation may not absorb the
         * erratum. Two pixels of alpha-0 cost nothing. */
        dummy[i].z = 1.0f;
    }
    dummy[0].x = x0;        dummy[0].y = y0;
    dummy[1].x = x0 + 2.0f; dummy[1].y = y0;
    dummy[2].x = x0;        dummy[2].y = y0 + 2.0f;

    /* PT route */
    pt_submit(&s_untexHdr[R_HDR_PT], 1);
    pt_submit(dummy, 3);

    /* TR route — same stream position, appended to the RAM staging buffer. */
    void *hdst = safe_pvr_vertbuf_tail(PVR_LIST_TR_POLY);
    if (hdst) {
        memcpy(hdst, &s_untexHdr[R_HDR_TR], sizeof(pvr_poly_hdr_t));
        safe_pvr_vertbuf_written(PVR_LIST_TR_POLY, sizeof(pvr_poly_hdr_t));

        void *vdst = safe_pvr_vertbuf_tail(PVR_LIST_TR_POLY);
        if (vdst) {
            memcpy(vdst, dummy, sizeof(dummy));
            safe_pvr_vertbuf_written(PVR_LIST_TR_POLY, sizeof(dummy));
        }
    }

    /* We just put OUR header in both streams; the cached state no longer
     * describes what the hardware will see. */
    MARK_HDR_DIRTY();
}

void R_SetTileClip(int sx, int sy, int ex, int ey)
{
    /* 32-byte aligned: sq_fast_cpy moves whole store-queue granules. */
    static pvr_tile_clip_cmd_t __attribute__((aligned(32))) cmd;

    cmd.flags = PVR_CMD_USERCLIP;
    cmd.d1 = cmd.d2 = cmd.d3 = 0;
    cmd.sx = (uint32_t)sx;
    cmd.sy = (uint32_t)sy;
    cmd.ex = (uint32_t)ex;
    cmd.ey = (uint32_t)ey;

    /* BOTH sides are required. Tested on hardware: a dummy only AFTER the
     * command does NOT fix it — the erratum needs one before as well. Both sit
     * in the outgoing region (s_prevClip is not updated until the end), so
     * neither is affected by the region being installed between them.
     * Do not "simplify" this to a single dummy. */
    emit_clip_dummy();

    /* PT route: straight to the TA, same path the poly header takes. */
    pt_submit(&cmd, 1);

    /* TR route: a separate list with its own ordering, so it needs its own
     * copy appended into the RAM staging buffer at the same point. */
    void *dst = safe_pvr_vertbuf_tail(PVR_LIST_TR_POLY);
    if (dst) {
        memcpy(dst, &cmd, sizeof(cmd));
        safe_pvr_vertbuf_written(PVR_LIST_TR_POLY, sizeof(cmd));
    }

    /* Absorb the USERCLIP erratum before any real geometry follows. Must come
     * after the command on BOTH routes, and while s_prevClip still holds the
     * outgoing region. */
    emit_clip_dummy();

    s_prevClip[0] = sx;
    s_prevClip[1] = sy;
    s_prevClip[2] = ex;
    s_prevClip[3] = ey;
}

/* Widen the clip to the entire framebuffer — the closest thing to "off"
 * once every header honours the clip. Always the true framebuffer extent,
 * never the reduced render height: full-screen UI legitimately uses all
 * 480 even in a session whose race renders at 448. */
void R_SetTileClipFullScreen(void)
{
#ifdef SONICR_DC_240P
    /* A tile is 64 virtual units here (32 real, halved at submit), so >> 6.
     * Unlike the per-viewport call these are COUNTS rather than inclusive
     * coordinates, so they need converting to a last-index:
     *
     *   width  640 >> 6 = 10 columns -> last index 9, so the -1 IS required.
     *   height 480 >> 6 = 7. Real height is 240 = 7.5 tiles, and the truncation
     *          has already landed on 7, the inclusive index of the last
     *          (half-height) row. A -1 here would give 6 and clip off the
     *          bottom 16 real lines, so height deliberately gets none. */
    R_SetTileClip(0, 0,
                  (g_screenWidth >> 6) - 1,
                  (g_screenHeightBase >> 6));
#else
    R_SetTileClip(0, 0,
                  (g_screenWidth >> 5) - 1,
                  (g_screenHeightBase >> 5) - 1);
#endif
}

void R_PtSubmit(pvr_vertex_t *v, size_t bytes)
{
    pt_submit(v, bytes >> 5);
}

/* =====================================================================
 * 2D quad helpers (HUD/menu)
 * ===================================================================== */

void R_DrawQuad2D(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1,
                  float z, uint32_t color)
{
    float rhw = (z > 0.0f) ? reciprocal(z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z * reciprocal(farSafe);;

    RenderVertex v[4];
    v[0] = (RenderVertex){ x0, y0, normZ, rhw, color, 0, u0, v0 };
    v[1] = (RenderVertex){ x1, y0, normZ, rhw, color, 0, u1, v0 };
    v[2] = (RenderVertex){ x1, y1, normZ, rhw, color, 0, u1, v1 };
    v[3] = (RenderVertex){ x0, y1, normZ, rhw, color, 0, u0, v1 };
    R_DrawQuad(v);
}

void R_DrawQuad2DSolid(float x0, float y0, float x1, float y1,
                       float z, uint32_t color)
{
    int savedTex = s_desired.textureId;
    s_desired.textureId = -1;
    MARK_HDR_DIRTY();

    float rhw = (z > 0.0f) ?reciprocal(z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z * reciprocal(farSafe);

    RenderVertex v[4];
    v[0] = (RenderVertex){ x0, y0, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[1] = (RenderVertex){ x1, y0, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[2] = (RenderVertex){ x1, y1, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[3] = (RenderVertex){ x0, y1, normZ, rhw, color, 0, 0.0f, 0.0f };
    R_DrawQuad(v);

    s_desired.textureId = savedTex;
    MARK_HDR_DIRTY();
}

/* =====================================================================
 * Texture API — forward to render_pvr.c
 * ===================================================================== */

void R_InitTextures(void)
{
    PVR_InitTextures();
}

void R_UploadTexture(int t)
{
    PVR_UploadTpage(t);
}

void R_MarkTextureDirty(int t)
{
    PVR_MarkTpageDirty(t);
}

void R_ClearTextureDirty(int t)
{
    PVR_ClearTpageDirty(t);
}

void R_FreezeTexture(int t)
{
    PVR_FreezeTpage(t);
}

void R_ThawTexture(int t)
{
    PVR_ThawTpage(t);
}

void R_SetNoColorKey(int t)
{
    PVR_SetNoColorKey(t);
}

void R_ClearNoColorKey(int t)
{
    PVR_ClearNoColorKey(t);
}

void R_SetTpageGreen6(int t, int on)
{
    PVR_SetTpageGreen6(t, on);
}

/* No-op: the 32-bit RGBA8 tpage override (desktop sky) isn't used on DC. */
void R_SetTpageRGBA8(int t, unsigned char *rgba)
{
    (void)t; (void)rgba;
}

void R_UploadTextureRGBA(int t, unsigned char *rgba, int w, int h)
{
    PVR_UploadTpageRGBA(t, rgba, w, h);
}

void R_UploadTextureSubRect(int t, int x, int y, int w, int h)
{
    PVR_UploadTpageSubRect(t, x, y, w, h);
}

void R_SetPendingRGBA(int t, unsigned char *rgba, int w, int h)
{
    PVR_SetPendingRGBA(t, rgba, w, h);
}
