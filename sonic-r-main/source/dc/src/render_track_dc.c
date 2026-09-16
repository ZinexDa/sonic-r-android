/**
 * render_track_dc.c — Dreamcast RenderTrackD3D (0x004533C4) + its PVR clipper
 *
 * Split out of sdl/src/render_track_d3d.c so it is obvious which body is live
 * on which platform. This is the float/FIPR migration target: float view
 * matrix (g_viewMtxF), sinf/cosf, and direct pvr_vertex_t emission through the
 * near-plane/scissor clipper in this file. Every symbol here is DC-exclusive —
 * nothing outside referenced it even before the split.
 *
 * The SDL counterpart is sdl/src/render_track_sdl.c (integer matrix + sin
 * tables). Anything that is about the TRANSLATION rather than the arithmetic
 * representation must be changed in both. Shared helpers live in
 * sdl/src/render_track_internal.h.
 */

#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include <math.h>
#include "r_types.h"
#include "r_state.h"
#include "r_draw.h"
#include "nearclip.h"
#include "render_track_internal.h"

extern void RenderHiddenSubEntry(int worldX, int worldZ, int worldY);

extern int R_EmitHeader(int pvr_list);
extern pvr_vertex_t *R_TrVertbufTail(void);
extern void R_TrVertbufWritten(size_t bytes);
extern void R_PtSubmit(pvr_vertex_t *v, size_t bytes);

/*=======================================================
 * DC near-plane quad clipper: in-place strip output via s_ptScratch.
 *=======================================================
 * Side buffer holding cam-space coords parallel to s_ptScratch[].
 * pvr_vertex_t can't carry pre-perspdiv data (its z field is rhw), so
 * we keep camX/camY/camZ here for the duration of the clip switch.
 * After clipping, lerped verts are projected on the spot (depth=1 at
 * the near plane simplifies the math). Original surviving verts keep
 * their precomputed screen+rhw — already in s_ptScratch[]. */
extern pvr_vertex_t s_ptScratch[16];
extern void R_DrawPvrStrip(pvr_vertex_t *v, int count);
extern void R_DrawPvrTri(pvr_vertex_t *v);
static float s_clipCamX[8];
static float s_clipCamY[8];
static float s_clipCamZ[8];

static inline void modify_track_color(uint32_t c, uint32_t *bc, uint32_t *oc) {
    uint32_t cr = (c >> 16) & 0xff;
    uint32_t cg = (c >> 8) & 0xff;
    uint32_t cb = (c) & 0xff;

    uint32_t br, bg, bb, ocr, ocg, ocb;
    /*
     *  Take a track color that would be rendered using ADD_SIGNED
     *  and split it to be rendered with the PowerVR as a combination
     *  of base and offset (argb + oargb) vertex colors.
     *
     *  Where cc is color component,
     *  bc is base component,
     *  oc is offset component
     *
     *  bc = cc > 0x7F ? 0x7F : cc; 
     *  oc = cc > 0x7F ? (cc - 0x7F) : 0;
     *  bc = 0xe0 - (0x7f - bc);
     *
     *  What you see below is just the above, reordered
     *  in a way that leads to better code generation.
     */
    int dr = cr - 0x7F;
    int dg = cg - 0x7F;
    int db = cb - 0x7F;

    ocr = dr > 0 ? dr : 0;
    ocg = dg > 0 ? dg : 0;
    ocb = db > 0 ? db : 0;

    br = cr - ocr;
    bg = cg - ocg;
    bb = cb - ocb;

    /* raise mid-point of split base color */
    br = br + 0x80;
    bg = bg + 0x80;
    bb = bb + 0x80;

    /* reduce intensity of additive component slightly (down to 87.5%) */
    ocr -= (ocr >> 3);
    ocg -= (ocg >> 3);
    ocb -= (ocb >> 3);

    *bc  = (c & 0xff000000) | (br << 16) | (bg << 8) | (bb << 0);
    *oc = (ocr << 16) | (ocg << 8) | ocb;
}

/* In-place near-plane lerp at slot `dest` between source slots a and b.
 * Aliasing-safe: snapshots both inputs before any write to dest. */
static inline void clip_lerp(pvr_vertex_t *buf, int ia, int ib, int dest)
{
    float xa = s_clipCamX[ia];
    float xb = s_clipCamX[ib];
    float ya = s_clipCamY[ia];
    float yb = s_clipCamY[ib];
    float za = s_clipCamZ[ia];
    float zb = s_clipCamZ[ib];
    pvr_vertex_t va = buf[ia];
    pvr_vertex_t vb = buf[ib];

    /* lerp t such that resulting camZ = 1.0 (near plane) */
    float t = (1.0f - za) * reciprocal((zb - za));

    float cx = xa + (xb - xa) * t;
    float cy = ya + (yb - ya) * t;

    s_clipCamX[dest] = cx;
    s_clipCamY[dest] = cy;
    s_clipCamZ[dest] = 1.0f;

    /* Project at depth=1 → /depth is identity, rhw=1.0 */
    pvr_vertex_t *vd = &buf[dest];
    vd->x = (float)g_screenCenterX + (float)g_projScaleXCurrent * cx;
    vd->y = (float)g_screenCenterY - (float)g_projScaleY * cy;
    vd->z = 1.0f;
    vd->u = va.u + (vb.u - va.u) * t;
    vd->v = va.v + (vb.v - va.v) * t;

    /* not lerping colors is faster... */
    vd->argb = va.argb;
    vd->oargb = va.oargb;

    vd->flags = PVR_CMD_VERTEX;

#if 0
    /* Color lerp componentwise */
    int aA = (int)((va.argb >> 24) & 0xFFu);
    int aR = (int)((va.argb >> 16) & 0xFFu);
    int aG = (int)((va.argb >> 8) & 0xFFu);
    int aB = (int)(va.argb & 0xFFu);
    aA += (int)((float)((int)((vb.argb >> 24) & 0xFFu) - aA) * t);
    aR += (int)((float)((int)((vb.argb >> 16) & 0xFFu) - aR) * t);
    aG += (int)((float)((int)((vb.argb >> 8) & 0xFFu) - aG) * t);
    aB += (int)((float)((int)(vb.argb & 0xFFu) - aB) * t);
    if (aA < 0) {
        aA = 0;
    }
    else if (aA > 255) {
        aA = 255;
    }
    if (aR < 0) {
        aR = 0;
    }
    else if (aR > 255) {
        aR = 255;
    }
    if (aG < 0) {
        aG = 0;
    }
    else if (aG > 255) {
        aG = 255;
    }
    if (aB < 0) {
        aB = 0;
    }
    else if (aB > 255) {
        aB = 255;
    }

    vd->argb = ((uint32_t)aA<<24) | ((uint32_t)aR<<16) |
               ((uint32_t)aG<< 8) |  (uint32_t)aB;
#endif
}

/* Build one input slot from a SrcVertex + per-poly UV. Loads cam-space
 * into the side buffer, screen+rhw+color+UV into s_ptScratch[]. Returns
 * 1 if the vert is in front of the near plane. */
static inline void clip_load_slot(pvr_vertex_t *buf, int slot, const SrcVertex *sv, const int *uvPair, float invFarSafe)
{
    float czf = (float)sv->depth;
    s_clipCamX[slot] = (float)sv->camX;
    s_clipCamY[slot] = (float)sv->camY;
    s_clipCamZ[slot] = czf;

    pvr_vertex_t *vs = &buf[slot];
    SrcVertex_ProjectFloat(sv, &vs->x, &vs->y);
    /* rhw: read the float reciprocal stashed by the projection pass instead
     * of recomputing reciprocal((float)int_depth). Keeps PVR's UV interp
     * synced to the same sub-int precision used to project screenX/Y, so
     * near-camera walls don't wobble as depth steps frame-to-frame. */
    vs->z = (czf >= 1.0f) ? sv->rhw : 0.0f;

    int r = sv->colorR >> 13;
    int g = sv->colorG >> 13;
    int b = sv->colorB >> 13;
    if (g_colorTintEnable) {
        r += g_colorTintR; 
        g += g_colorTintG;
        b += g_colorTintB;
    }
    if (r > 255) {
        r = 255;
    }
    if (g > 255) {
        g = 255;
    }
    if (b > 255) {
        b = 255;
    }

    int fogAlpha;
    float fogD = czf * invFarSafe;
    if (fogD > (float)FOG_FAR) {
        fogAlpha = 0;
    }
    else if (fogD <= (float)FOG_NEAR) {
        fogAlpha = 0xFF;
    }
    else {
        int raw = (int)((fogD + (float)FOG_NEG_FAR) * (float)FOG_MUL);
        fogAlpha = (raw < 0) ? -raw : raw;
    }

    uint32_t baseColor = ((uint32_t)fogAlpha << 24) |
               ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

    modify_track_color(baseColor, &vs->argb, &vs->oargb);

    /* Same form the scissor clipper gets via NearClipFillVertTrack —
     * TRACK_UV_LEGACY selects it. */
    vs->u = TrackUV(uvPair[0]);
    vs->v = TrackUV(uvPair[1]);
    vs->flags = PVR_CMD_VERTEX;
}

/*=======================================================
 * DC viewport-edge scissor path for track polys (split-screen only).
 *
 * Mirrors the character renderer's scissor strategy: NearClipPolygon
 * against near plane, then ScissorClipPolygon against the inward edge,
 * then fan-emit. Per-vertex pvr_vertex_t builder applies the same
 * color/tint/fog/UV math as clip_load_slot above so visual output
 * matches the strip path on overlapping geometry.
 *======================================================= */

/* Build one pvr_vertex_t from a NearClipVert with track-style color,
 * tint, fog alpha, and UV. Mirrors clip_load_slot's pixel formula. */
static inline void track_build_pvr_from_clip(pvr_vertex_t *pv, const NearClipVert *v,
                                             float invFarSafe, uint32_t flags)
{
    int projZ = v->depth;
    if (projZ < 1) {
        projZ = 1;
    }
    float czf = (float)projZ;

    pv->flags = flags;
    NearClipVert_ProjectFloat(v->camX, v->camY, v->depth > 0 ? v->depth : 1, &pv->x, &pv->y);
    pv->z = reciprocal(czf);

    int r = v->colorR >> 13;
    int g = v->colorG >> 13;
    int b = v->colorB >> 13;
    if (g_colorTintEnable) {
        r += g_colorTintR;
        g += g_colorTintG;
        b += g_colorTintB;
    }

    if (r > 255) {
        r = 255;
    }
    if (g > 255) {
        g = 255;
    }
    if (b > 255) {
        b = 255;
    }

    int fogAlpha;
    float fogD = czf * invFarSafe;
    if (fogD > (float)FOG_FAR) {
        fogAlpha = 0;
    }
    else if (fogD <= (float)FOG_NEAR) {
        fogAlpha = 0xFF;
    }
    else {
        int raw = (int)((fogD + (float)FOG_NEG_FAR) * (float)FOG_MUL);
        fogAlpha = (raw < 0) ? -raw : raw;
    }

    uint32_t baseColor = ((uint32_t)fogAlpha << 24) |
               ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;

    modify_track_color(baseColor, &pv->argb, &pv->oargb);

    pv->u = v->u;
    pv->v = v->v;
}

/* File-static perimeter-order temps. Reused across calls. */
static NearClipVert s_trackInBuf[4];          /* SrcVertex → NearClipVert input */
static NearClipVert s_trackNearClipBuf[7];    /* near-plane output */

/* Strip-emit a convex polygon (perimeter order) as one R_DrawPvrStrip via
 * zig-zag vertex ordering. Mirrors strip_emit_pvr_polygon in character
 * but uses track's per-vertex builder (color tint + fog). */
static void track_strip_emit_pvr(const NearClipVert *poly, int n, float invFarSafe)
{
    for (int k = 0; k < n; k++) {
        int srcIdx;
        if (k < 2) {
            srcIdx = k;
        }
        else if (k & 1) {
            srcIdx = (k + 1) >> 1;
        }
        else {
            srcIdx = n - (k >> 1);
        }
        uint32_t flags = (k == n - 1) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        track_build_pvr_from_clip(&s_ptScratch[k], &poly[srcIdx], invFarSafe, flags);
    }
    R_DrawPvrStrip(s_ptScratch, n);
}


/* Post-clip backface cull on already-built PVR vertices. Their x/y are already
 * floats, so this is two FMUL and a subtract with no int->float conversions,
 * and post-clip screen coordinates are bounded enough that the 24-bit mantissa
 * is not at risk. `honorMirror` is 0 for the triangle paths: the binary's
 * triangle clip submit has no g_mirrorMode handling (zero references to
 * 0x6E9920 inside 0x452514), unlike the quad one. */
static inline int TrackPvrCullFails(const pvr_vertex_t *a, const pvr_vertex_t *b,
                                    const pvr_vertex_t *c, int honorMirror)
{
    float cross = (a->y - b->y) * (c->x - b->x) -
                  (a->x - b->x) * (c->y - b->y);
    if (honorMirror && g_mirrorMode) {
        cross = -cross;
    }
    return (cross < 0.0f);
}


/* DC quad clip-and-emit. Pre-scans source depths to determine PT vs TR
 * before building any verts, then writes directly into the destination
 * buffer (scratch for PT, TR vertbuf for TR) — no memcpy. */
static int TrackClipAndEmitQuad_DC(SrcVertex *psv[4], const int *polyUV,
                                   int tpage, float invFarSafe, int needCull)
{
    /* Source quad order is TL,TR,BR,BL (= src 0,1,2,3). PVR strip slot
     * order TL,TR,BL,BR maps to source 0,1,3,2. */
    static const int srcFromStrip[4] = { 0, 1, 3, 2 };

    /* Pre-scan: determine near-plane visibility AND fog translucency
     * from source depths alone — cheap float multiply + compares. */
    int vismask = 0;
    int is_tr = 0;
    int allFogged = 1;      /* every vertex past FOG_FAR -> whole prim invisible */
    for (int i = 0; i < 4; i++) {
        float czf = (float)psv[srcFromStrip[i]]->depth;
        if (czf >= 1.0f) {
            vismask |= (1 << i);
        }
        /* fogD is needed for both tests now, so it is no longer computed
         * behind an `if (!is_tr)` early-out. One multiply per vertex. */
        float fogD = czf * invFarSafe;
        if (fogD > (float)FOG_NEAR) {
            is_tr = 1;
        }
        if (fogD <= (float)FOG_FAR) {
            allFogged = 0;
        }
    }
    if (vismask == 0) {
        return 0;
    }

#if TRACK_FULL_FOG_CULL
    if (allFogged) {
        return 0;
    }
#endif

    R_SetTexture(tpage);
    R_SetTexEnv(R_TEXENV_ADD_SIGNED);

    int list = is_tr ? PVR_LIST_TR_POLY : PVR_LIST_PT_POLY;
    if (!R_EmitHeader(list)) {
        return 0;
    }

    pvr_vertex_t *dest;
    if (is_tr) {
        dest = R_TrVertbufTail();
        if (!dest) {
            return 0;
        }
    }
    else {
        dest = s_ptScratch;
    }

    /* Build verts directly into destination buffer. */
    for (int i = 0; i < 4; i++) {
        int src = srcFromStrip[i];
        clip_load_slot(dest, i, psv[src], &polyUV[src * 2], invFarSafe);
    }

    int sendverts = 4;

    if (vismask == 15) {
        dest[3].flags = PVR_CMD_VERTEX_EOL;
        goto finalize;
    }

    /* In-place clip switch — translated from reference impl. Buffer is
     * in PVR strip order; cases match strip-cyclic adjacency masks. */
    switch (vismask) {
        case 1: /* slot 0 visible */
            sendverts = 3;
            clip_lerp(dest, 0, 1, 1);
            clip_lerp(dest, 0, 2, 2);
            dest[2].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 2: /* slot 1 visible */
            sendverts = 3;
            clip_lerp(dest, 1, 0, 0);
            clip_lerp(dest, 1, 3, 2);
            dest[2].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 3: /* slots 0+1 visible */
            clip_lerp(dest, 0, 2, 2);
            clip_lerp(dest, 1, 3, 3);
            dest[3].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 4: /* slot 2 visible */
            sendverts = 3;
            clip_lerp(dest, 2, 0, 0);
            clip_lerp(dest, 2, 3, 1);
            dest[2].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 5: /* slots 0+2 visible */
            clip_lerp(dest, 0, 1, 1);
            clip_lerp(dest, 2, 3, 3);
            dest[3].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 7: /* slots 0+1+2 visible — 5-vert strip */
            sendverts = 5;
            clip_lerp(dest, 2, 3, 4);
            clip_lerp(dest, 1, 3, 3);
            dest[3].flags = PVR_CMD_VERTEX;
            dest[4].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 8: /* slot 3 visible */
            sendverts = 3;
            clip_lerp(dest, 1, 3, 0);
            clip_lerp(dest, 2, 3, 2);
            dest[1] = dest[3];
            s_clipCamX[1] = s_clipCamX[3];
            s_clipCamY[1] = s_clipCamY[3];
            s_clipCamZ[1] = s_clipCamZ[3];
            dest[1].flags = PVR_CMD_VERTEX;
            dest[2].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 10: /* slots 1+3 visible */
            clip_lerp(dest, 0, 1, 0);
            clip_lerp(dest, 2, 3, 2);
            dest[3].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 11: /* slots 0+1+3 visible — 5-vert strip */
            sendverts = 5;
            clip_lerp(dest, 2, 3, 4);
            clip_lerp(dest, 0, 2, 2);
            dest[3].flags = PVR_CMD_VERTEX;
            dest[4].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 12: /* slots 2+3 visible */
            clip_lerp(dest, 0, 2, 0);
            clip_lerp(dest, 1, 3, 1);
            dest[3].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 13: /* slots 0+2+3 visible — 5-vert strip */
            sendverts = 5;
            dest[4] = dest[3];
            s_clipCamX[4] = s_clipCamX[3];
            s_clipCamY[4] = s_clipCamY[3];
            s_clipCamZ[4] = s_clipCamZ[3];
            clip_lerp(dest, 1, 3, 3);
            clip_lerp(dest, 0, 1, 1);
            dest[3].flags = PVR_CMD_VERTEX;
            dest[4].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 14: /* slots 1+2+3 visible — 5-vert strip */
            sendverts = 5;
            dest[4] = dest[2];
            s_clipCamX[4] = s_clipCamX[2];
            s_clipCamY[4] = s_clipCamY[2];
            s_clipCamZ[4] = s_clipCamZ[2];
            clip_lerp(dest, 0, 2, 2);
            clip_lerp(dest, 0, 1, 0);
            dest[3].flags = PVR_CMD_VERTEX;
            dest[4].flags = PVR_CMD_VERTEX_EOL;
            break;
        default:
            return 0;
    }

finalize:
    /* Deferred cull for polygons that straddled the near plane — see the
     * needCull note on TrackClipAndEmitTriScissorDC. Strip slots 0,1,2 are
     * TL,TR,BL, which carries the same orientation as the source quad.
     * Returning here leaves an already-emitted header with no vertices behind
     * it; the next primitive's header supersedes it. */
    if (needCull && TrackPvrCullFails(&dest[0], &dest[1], &dest[2], 1)) {
        return 0;
    }

    size_t bytes = (size_t)sendverts * sizeof(pvr_vertex_t);
#ifdef SONICR_DC_240P
    for (int i = 0; i < sendverts; i++) {
        dest[i].x *= 0.5f;
        dest[i].y *= 0.5f;
    }
#endif
    if (is_tr) {
        R_TrVertbufWritten(bytes);
    }
    else {
        R_PtSubmit(dest, bytes);
    }
    return sendverts - 2;
}

/* DC tri clip-and-emit. Output is a 3- or 4-vert PVR strip submitted via
 * one R_DrawPvrStrip call. Worst case (1 vert behind near plane) expands
 * the tri to a 4-vert quad strip. */
static int TrackClipAndEmitTri_DC(SrcVertex *psv[3], const int *polyUV,
                                  int tpage, float invFarSafe, int needCull)
{
    int vismask = 0;
    int is_tr = 0;
    int allFogged = 1;      /* see TrackClipAndEmitQuad_DC */
    for (int i = 0; i < 3; i++) {
        float czf = (float)psv[i]->depth;
        if (czf >= 1.0f) {
            vismask |= (1 << i);
        }
        float fogD = czf * invFarSafe;
        if (fogD > (float)FOG_NEAR) {
            is_tr = 1;
        }
        if (fogD <= (float)FOG_FAR) {
            allFogged = 0;
        }
    }
    if (vismask == 0) {
        return 0;
    }

#if TRACK_FULL_FOG_CULL
    if (allFogged) {
        return 0;
    }
#endif

    R_SetTexture(tpage);
    R_SetTexEnv(R_TEXENV_ADD_SIGNED);

    int list = is_tr ? PVR_LIST_TR_POLY : PVR_LIST_PT_POLY;
    if (!R_EmitHeader(list)) {
        return 0;
    }

    pvr_vertex_t *dest;
    if (is_tr) {
        dest = R_TrVertbufTail();
        if (!dest) {
            return 0;
        }
    }
    else {
        dest = s_ptScratch;
    }

    for (int i = 0; i < 3; i++) {
        clip_load_slot(dest, i, psv[i], &polyUV[i * 2], invFarSafe);
    }

    int sendverts = 3;

    if (vismask == 7) {
        dest[2].flags = PVR_CMD_VERTEX_EOL;
        goto finalize;
    }

    switch (vismask) {
        case 1:
            clip_lerp(dest, 0, 1, 1);
            clip_lerp(dest, 0, 2, 2);
            dest[2].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 2:
            clip_lerp(dest, 0, 1, 0);
            clip_lerp(dest, 1, 2, 2);
            dest[2].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 3:
            sendverts = 4;
            clip_lerp(dest, 1, 2, 3);
            clip_lerp(dest, 0, 2, 2);
            dest[2].flags = PVR_CMD_VERTEX;
            dest[3].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 4:
            clip_lerp(dest, 0, 2, 0);
            clip_lerp(dest, 1, 2, 1);
            dest[2].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 5:
            sendverts = 4;
            clip_lerp(dest, 1, 2, 3);
            clip_lerp(dest, 0, 1, 1);
            dest[2].flags = PVR_CMD_VERTEX;
            dest[3].flags = PVR_CMD_VERTEX_EOL;
            break;
        case 6:
            sendverts = 4;
            dest[3] = dest[2];
            s_clipCamX[3] = s_clipCamX[2];
            s_clipCamY[3] = s_clipCamY[2];
            s_clipCamZ[3] = s_clipCamZ[2];
            clip_lerp(dest, 0, 2, 2);
            clip_lerp(dest, 0, 1, 0);
            dest[2].flags = PVR_CMD_VERTEX;
            dest[3].flags = PVR_CMD_VERTEX_EOL;
            break;
        default:
            return 0;
    }

finalize:
    /* Deferred cull — triangles do not honour mirror mode, matching the
     * binary's triangle clip submit. */
    if (needCull && TrackPvrCullFails(&dest[0], &dest[1], &dest[2], 0)) {
        return 0;
    }

    size_t bytes = (size_t)sendverts * sizeof(pvr_vertex_t);
#ifdef SONICR_DC_240P
    for (int i = 0; i < sendverts; i++) {
        dest[i].x *= 0.5f;
        dest[i].y *= 0.5f;
    }
#endif
    if (is_tr) {
        R_TrVertbufWritten(bytes);
    }
    else {
        R_PtSubmit(dest, bytes);
    }
    return sendverts - 2;
}

/**
 * RenderTrackD3D — 0x004533C4 — 6316 bytes
 *
 * Faithful translation of the D3D track/object renderer, retargeted to the
 * SH4 FPU: float view matrix (g_viewMtxF) and FIPR dot products in place of
 * the binary's integer transform.
 */
void RenderTrackD3D(void)
{
    /* DC float/fipr migration target. */
    int remaining = g_totalObjects;   /* 0x6EAD30 */
    int *obj = (int *)g_objectStructArray;  /* 0x712D44 */
    float farClipF = g_farClipFloat;  /* 0x8FB364 */

    if (remaining <= 0) {
        return;
    }

    if (g_vertexArrayBase == NULL || g_polygonArrayBase == NULL) {
        return;
    }

    int farClipThresh = g_farClipDepth; /* 0x8FB35C */
    if (farClipThresh <= 0) {
        farClipThresh = 0x2B00; /* default: 88064 >> 3 = 11008 = 0x2B00 */
    }

    /* Per-call constants for the DC emit path: hoisted out of the per-poly
     * loop. invFarSafe is the per-vertex fog ramp denominator. */
    float farSafe = (farClipF > 0.0f) ? farClipF : 88064.0f;
    float invFarSafe = reciprocal(farSafe);

    do {
        int visFlag = *(short *)((char *)obj + 0x2C);
        if (visFlag == -1) {
            goto next_object;
        }

        int mode = *(short *)((char *)obj + 0x2E);
        int dx;
        int dy;
        int dz;

        if (mode == 2) {
            dx = obj[8] - g_camIntX;
            dy = obj[9] - g_camIntY;
            dz = obj[10] - g_camIntZ;
        } else {
            dx = obj[0] - g_camIntX;
            dy = obj[1] - g_camIntY;
            dz = obj[2] - g_camIntZ;
        }

        int boundSphere = visFlag;
        int originScrX;
        int originScrY;
        int projBound;

        /* object-origin transform + projection
         * Float fipr-based replacement of the int-divide block. Uses
         * g_viewMtxF[] (pre-divided by 4096 in BuildViewMatrix), three
         * vec3f_dot calls (each compiles to fipr), and one reciprocal
         * (fsrra) reciprocal of cz applied as a multiply. Kills six
         * __sdivsi3_i4i call sites that used to live here. */
        float dx_f = (float)dx;
        float dy_f = (float)dy;
        float dz_f = (float)dz;
        float boundSphere_f = (float)boundSphere;

        float cz_f;
        float cx_f;
        float cy_f;
        vec3f_dot(dx_f, dy_f, dz_f,
                  g_viewMtxF[2], g_viewMtxF[6], g_viewMtxF[10], cz_f);

        if (((cz_f - boundSphere_f) * 0.125f) > (float)farClipThresh) {
            goto next_object;
        }
        if (cz_f < -boundSphere_f) {
            goto next_object;
        }
        if (cz_f < 1.0f) {
            cz_f = 1.0f;
        }

        vec3f_dot(dx_f, dy_f, dz_f,
                  g_viewMtxF[0], g_viewMtxF[4], g_viewMtxF[8], cx_f);

        float inv_cz = reciprocal(cz_f);
        originScrX = g_screenCenterX + (int)((float)g_projScaleXCurrent * cx_f * inv_cz);
        projBound  = (int)((float)g_projScaleXCurrent * boundSphere_f * inv_cz);

        if (originScrX - projBound > g_clipRight) {
            goto next_object;
        }
        if (originScrX + projBound < g_clipLeft) {
            goto next_object;
        }

        vec3f_dot(dx_f, dy_f, dz_f,
                  g_viewMtxF[1], g_viewMtxF[5], g_viewMtxF[9], cy_f);

        originScrY = g_screenCenterY - (int)((float)g_projScaleY * cy_f * inv_cz);

        if (originScrY - projBound > g_clipBottom) {
            goto next_object;
        }
        if (originScrY + projBound < g_clipTop) {
            goto next_object;
        }
        
        unsigned int vtxStartIdx = *(unsigned short *)((char *)obj + 0x38);
        SrcVertex *vtxBase = &g_vertexArrayBase[vtxStartIdx];

        g_processedObjectCount++;
        unsigned int vertCount = *(unsigned short *)((char *)obj + 0x3A);

        if (mode == 2) {
            /* mode-2 decoration transform
             * Float fipr-based replacement of the int Euler matrix +
             * per-vertex transform + projection. Trig comes from sinf/cosf
             * pairs on the same argument — fastmath + mfsca fold each pair
             * into a single fsca instruction, so 3 fsca total per object
             * for {yaw, pitch, roll}. Local rotation matrix LF** is built
             * once per object; per-vertex inner loop is 3 fipr (local
             * rotation) + 2 fipr + 1 fmul (view transform) + 1 fsrra
             * reciprocal (perspective). */
            int yaw = *(short *)((char *)obj + 0x18);
            int pitch = *(short *)((char *)obj + 0x1A);
            int roll = *(short *)((char *)obj + 0x1C);

            /* 12-bit angle index → radians. The sin/cos pair on the same
             * argument lets the compiler emit one fsca per axis. */
            const float kIdxToRad = 0.00153398f; // 6.28318530717958647692f / 4096.0f;
            float yawR = (float)yaw * kIdxToRad;
            float pitchR = (float)pitch * kIdxToRad;
            float rollR = (float)roll * kIdxToRad;

            float sinY = sinf(yawR),   cosY = cosf(yawR);
            float sinP = sinf(pitchR), cosP = cosf(pitchR);
            float sinR = sinf(rollR),  cosR = cosf(rollR);

            /* Build the 3×3 local rotation matrix. Same composition as the
             * int legacy (yaw·roll then pitch), just lifted to float. */
            float cYsR = -(cosY * sinR);
            float sRsY =  (sinR * sinY);

            float LF00 = cosR * cosP;
            float LF01 = cYsR * cosP + sinY * sinP;
            float LF02 = cosY * sinP + sRsY * cosP;
            float LF10 = sinR;
            float LF11 = cosY * cosR;
            float LF12 = -(sinY * cosR);
            float LF20 = -(cosR * sinP);
            float LF21 = sinY * cosP - cYsR * sinP;
            float LF22 = cosP * cosY - sinP * sRsY;

            /* Pivot offset (pivot - camera), computed once per object */
            float pivOffX = (float)(obj[8]  - g_camIntX);
            float pivOffY = (float)(obj[9]  - g_camIntY);
            float pivOffZ = (float)(obj[10] - g_camIntZ);

            SrcVertex *vtx = vtxBase;
            for (unsigned int vi = 0; vi < vertCount; vi++) {
                float vx_f = (float)vtx->posX;
                float vy_f = (float)vtx->posY;
                float vz_f = (float)vtx->posZ;

                /* Local rotation: 3 fipr dots */
                float r0_f, r1_f, r2_f;
                vec3f_dot(vx_f, vy_f, vz_f, LF00, LF01, LF02, r0_f);
                vec3f_dot(vx_f, vy_f, vz_f, LF10, LF11, LF12, r1_f);
                vec3f_dot(vx_f, vy_f, vz_f, LF20, LF21, LF22, r2_f);

                /* Translate to camera-relative world position */
                float dxv_f = r0_f + pivOffX;
                float dyv_f = r1_f + pivOffY;
                float dzv_f = r2_f + pivOffZ;

                /* View transform (drop m10*dy term to match legacy mode 0) */
                float camSX_f = g_viewMtxF[0] * dxv_f + g_viewMtxF[8] * dzv_f;
                float camSY_f, camSZ_f;
                vec3f_dot(dxv_f, dyv_f, dzv_f,
                          g_viewMtxF[1], g_viewMtxF[5], g_viewMtxF[9],  camSY_f);
                vec3f_dot(dxv_f, dyv_f, dzv_f,
                          g_viewMtxF[2], g_viewMtxF[6], g_viewMtxF[10], camSZ_f);

                int camSX = (int)camSX_f;
                int camSY = (int)camSY_f;
                int camSZ = (int)camSZ_f;

                vtx->camX = camSX;
                vtx->camY = camSY;
                vtx->depth = camSZ;

                if (camSZ > 0) {
                    float inv_csz = reciprocal(camSZ_f);
                    float sxf = (float)g_projScaleXCurrent * camSX_f * inv_csz;
                    float syf = (float)g_projScaleY * camSY_f * inv_csz;
                    vtx->screenX = g_screenCenterX + (int)sxf;
                    vtx->screenY = g_screenCenterY - (int)syf;
                    vtx->rhw = inv_csz;
                }

                vtx++;
            }
        }
        else {
            /* mode-0 per-vertex transform + projection
             * Float fipr-based replacement of the int-shift block. Y and Z
             * rows are full 3-term dots via vec3f_dot (fipr); X mirrors
             * legacy by dropping the m10*vdy term (binary's "non-banked
             * camera" optimization). Per-vertex divides become a single
             * fsrra reciprocal of camSZ shared between screenX and screenY.
             *
             * Per visible vertex this kills three >>12 shifts and two
             * /camSZ libcalls. Output fields are still int — the cast
             * happens at the end. Downstream (NearClipFillVert / cull /
             * emit) reads SrcVertex int fields unchanged. */
            SrcVertex *vtx = vtxBase;
            for (unsigned int vi = 0; vi < vertCount; vi++) {
                float vdx_f = (float)(vtx->posX - g_camIntX);
                float vdy_f = (float)(vtx->posY - g_camIntY);
                float vdz_f = (float)(vtx->posZ - g_camIntZ);

                /* X: 2-term (m10 dropped, matching legacy) */
                float camSX_f = g_viewMtxF[0] * vdx_f + g_viewMtxF[8] * vdz_f;
                /* Y: full 3-term via fipr */
                float camSY_f, camSZ_f;
                vec3f_dot(vdx_f, vdy_f, vdz_f,
                          g_viewMtxF[1], g_viewMtxF[5], g_viewMtxF[9], camSY_f);
                /* Z (depth): full 3-term via fipr */
                vec3f_dot(vdx_f, vdy_f, vdz_f,
                          g_viewMtxF[2], g_viewMtxF[6], g_viewMtxF[10], camSZ_f);

                int camSX = (int)camSX_f;
                int camSY = (int)camSY_f;
                int camSZ = (int)camSZ_f;

                vtx->camX = camSX;
                vtx->camY = camSY;
                vtx->depth = camSZ;

                if (camSZ > 0) {
                    float inv_csz = reciprocal(camSZ_f);
                    float sxf = (float)g_projScaleXCurrent * camSX_f * inv_csz;
                    float syf = (float)g_projScaleY * camSY_f * inv_csz;
                    vtx->screenX = g_screenCenterX + (int)sxf;
                    vtx->screenY = g_screenCenterY - (int)syf;
                    vtx->rhw = inv_csz;
                }

                vtx++;
            }
        }

        /* 0x453B47: Radiant Emerald recolours this object's transformed
         * vertices before the poly loop. */
        if (g_trackId == TRACK_RADIANT_EMERALD) {
            TrackEmeraldRecolourObject(vtxBase, vertCount);
        }

        unsigned int polyCount = *(unsigned short *)((char *)obj + 0x32);
        unsigned int polyStartIdx = *(unsigned short *)((char *)obj + 0x30);
        int *polyPtr = (int *)((char *)g_polygonArrayBase + polyStartIdx * 0x30);

        int farClipTimes8 = g_farClipDepth << 3;
        if (farClipTimes8 <= 0) {
            farClipTimes8 = 0x15800;
        }

        for (unsigned int pi = 0; pi < polyCount; pi++) {
            char *pp = (char *)polyPtr;

            if (g_tpageStateArray[*(unsigned char *)(pp + 0x28)] != 0x04) {
                goto next_poly;
            }

            SrcVertex *pv0 = &g_vertexArrayBase[*(unsigned short *)(pp + 0x20)];
            SrcVertex *pv1 = &g_vertexArrayBase[*(unsigned short *)(pp + 0x22)];
            SrcVertex *pv2 = &g_vertexArrayBase[*(unsigned short *)(pp + 0x24)];

            /* Far clip — binary 0x453280 / 0x45329C / 0x4532B6 test each vertex
             * independently and skip the polygon if ANY one is beyond the far
             * plane. This used to require ALL of them, which kept polygons the
             * original drops. */
            if (pv0->depth > farClipTimes8 || pv1->depth > farClipTimes8 ||
                pv2->depth > farClipTimes8) {
                goto next_poly;
            }

            unsigned char flags = *(unsigned char *)(pp + 0x2E);

            if ((flags & 1) == 0) {
                int anyInFront = (pv0->depth >= 1) || (pv1->depth >= 1) || (pv2->depth >= 1);
                if (!anyInFront) {
                    goto next_poly;
                }

                int allProj3 = (pv0->depth >= 1 && pv1->depth >= 1 && pv2->depth >= 1);
                int needCull3 = 0;

                if ((flags & 4) == 0) {
                    if (allProj3) {
                        /* 32-bit integer cross, matching the binary's imul —
                         * two MUL.L and a subtract. The float form this replaces
                         * paid for four int->float conversions (LDS + FLOAT each)
                         * to save two integer multiplies, and could cancel in the
                         * 24-bit mantissa and flip the sign. int64 was the thing
                         * worth avoiding here (__muldi3 libcall on sh-elf);
                         * 32-bit MUL.L is native. Also carries the g_mirrorMode
                         * sign flip, which the DC path never had. */
                        if (TrackBackfaceCrossM(pv0->screenY, pv1->screenY,
                                                pv2->screenX, pv1->screenX,
                                                pv0->screenX, pv2->screenY) < 0)
                        {
                            goto next_poly;
                        }
                    }
                    else {
                        /* Straddling the near plane: screenX/screenY come from
                         * a sub-near depth and are meaningless, so defer the
                         * cull to after clipping, where the binary does it. The
                         * camera-space fallback that used to live here was ours,
                         * not the original's, and its sign convention was the
                         * exact negation of the quad branch's — one of the two
                         * was culling backwards. */
                        needCull3 = 1;
                    }
                }

                SrcVertex *pvs3[3] = { pv0, pv1, pv2 };

                /* Four-edge viewport reject — binary 0x4546E9. Skips the PVR
                 * list header and all the per-vertex fog/colour/UV/rhw work for
                 * polygons that fall entirely outside one clip edge. Gated on
                 * allProj3 for the same reason as the cull above: the binary
                 * only does this on its fast path, where every vertex is in
                 * front and has a meaningful screen position. */
                if (allProj3 && TrackViewportReject(pvs3, 3)) {
                    goto next_poly;
                }

                /* The PVR user tile clip owns the viewport edges, so the strip
                 * path is the only one — it still does the near-plane clip via
                 * its vismask. */
                TrackClipAndEmitTri_DC(pvs3, polyPtr, *(unsigned char *)(pp + 0x28),
                                       invFarSafe, needCull3);
            }
            else {
                SrcVertex *pv3 = &g_vertexArrayBase[*(unsigned short *)(pp + 0x26)];

                /* Binary 0x4532D0 — the 4th vertex gets the same independent
                 * test as the first three above, which already ran. */
                if (pv3->depth > farClipTimes8) {
                    goto next_poly;
                }

                int allProjected = (pv0->depth >= 1 && pv1->depth >= 1 &&
                                    pv2->depth >= 1 && pv3->depth >= 1);
                int needCull4 = 0;
                if ((flags & 4) == 0) {
                    /* Fully projected: cull here, before any header is emitted,
                     * with the exact 32-bit integer cross. Straddling: defer to
                     * after clipping. See the Tri branch. */
                    if (allProjected) {
                        int32_t cross = TrackBackfaceCrossM(pv0->screenY, pv1->screenY,
                                                            pv2->screenX, pv1->screenX,
                                                            pv0->screenX, pv2->screenY);
                        if ((flags & 2) == 0) {
                            if (cross < 0) {
                                goto next_poly;
                            }
                        }
                        else if (cross < 0) {
                            if (TrackBackfaceCrossM(pv0->screenY, pv2->screenY,
                                                    pv3->screenX, pv2->screenX,
                                                    pv0->screenX, pv3->screenY) < 0) 
                            {
                                goto next_poly;
                            }
                        }
                    }
                    else {
                        needCull4 = 1;
                    }
                }

                int tpage = *(unsigned char *)(pp + 0x28);

                SrcVertex *pvs[4] = { pv0, pv1, pv2, pv3 };

                /* Four-edge viewport reject — binary 0x453EED. */
                if (allProjected && TrackViewportReject(pvs, 4)) {
                    goto next_poly;
                }

                /* Strip path only — see the Tri branch above. */
                TrackClipAndEmitQuad_DC(pvs, polyPtr, tpage, invFarSafe, needCull4);
            }

        next_poly:
            polyPtr = (int *)((char *)polyPtr + 0x30);
        }

    next_object:
        int mode_re = *(short *)((char *)obj + 0x2E);
        if (mode_re == 0 && g_raceType != RACE_TIMEATTACK && g_raceSubMode != SUBMODE_TAG) {
            unsigned short subCount_re = *(unsigned short *)((char *)obj + 0x36);
            if (subCount_re > 0) {
                if (g_ringSpawnArray != NULL) {
                    unsigned short subStart_re = *(unsigned short *)((char *)obj + 0x34);
                    int *entry_re = (int *)((char *)g_ringSpawnArray + subStart_re * 16);
                    for (int ri = 0; ri < subCount_re; ri++, entry_re += 4) {
                        if (entry_re[3] == 0) {
                            RenderHiddenSubEntry(entry_re[0], entry_re[1], entry_re[2]);
                        }
                    }
                }
            }
        }

        obj += 17;
        remaining--;
    } while (remaining > 0);
}
