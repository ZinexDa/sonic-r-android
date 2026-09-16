/**
 * r_gles_backend.c — OpenGL ES 2.0 geometry rendering backend for Sonic R Android
 *
 * Implements r_draw.h, r_state.h, r_texture.h, and frame lifecycle using GLES2
 * shaders, dynamic VBOs, and CPU-computed MVP transformation.
 */

#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <SDL.h>

#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "r_types.h"
#include "r_state.h"
#include "r_draw.h"
#include "r_texture.h"
#include "platform.h"
#include "net_transport.h"
#include "touch_overlay.h"
#include <android/log.h>
#include <time.h>

#define GLES_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SonicR-GLES", __VA_ARGS__)
#define GLES_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "SonicR-GLES", __VA_ARGS__)
#define GLES_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "SonicR-GLES", __VA_ARGS__)

/* Diagnostic High-Resolution Timing */
uint64_t GetTimeNs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t g_perfLogicNs = 0;
uint64_t g_perfWaitCapNs = 0;

static uint64_t s_timeStateFlushNs = 0;
static uint64_t s_timeVboUploadNs = 0;
static uint64_t s_timeDrawArraysNs = 0;
static uint64_t s_timeTpageUploadNs = 0;
static int      s_tpageUploadCount = 0;
static uint64_t s_timeTouchOverlayNs = 0;
static uint64_t s_timeSwapNs = 0;
static uint64_t s_lastFlipNs = 0;

static int      s_accFrames = 0;
static uint64_t s_accTotalNs = 0;
static uint64_t s_accLogicNs = 0;
static uint64_t s_accStateFlushNs = 0;
static uint64_t s_accVboUploadNs = 0;
static uint64_t s_accDrawArraysNs = 0;
static uint64_t s_accTpageUploadNs = 0;
static int      s_accTpageUploadCount = 0;
static uint64_t s_accTouchOverlayNs = 0;
static uint64_t s_accSwapNs = 0;
static uint64_t s_accWaitCapNs = 0;
static int      s_accDrawCalls = 0;
static int      s_accVertCount = 0;

static uint64_t s_statFrameCount = 0;
static int s_statDrawCalls = 0;
static int s_statVertCount = 0;
static int s_statColorClears = 0;
static int s_statDepthClears = 0;

/* Backing pixel dimensions */
int g_glBackingWidth = 640;
int g_glBackingHeight = 480;
int g_glViewportOffsetX = 0;
int g_glViewportOffsetY = 0;

/* Vertex format for GLES2 (28 bytes) */
typedef struct {
    float   x, y, z, rhw;  /* position + 1/Z */
    uint8_t r, g, b, a;    /* normalized RGBA color */
    float   u, v;          /* texture coordinates (ready for texturing milestone) */
} GLES2Vertex;

/* Dynamic geometry batching */
#define MAX_BATCH_VERTS 8192
static GLES2Vertex s_batchVerts[MAX_BATCH_VERTS];
static int s_batchCount = 0;
static int s_lastMvpW = 0;
static int s_lastMvpH = 0;
static int s_currentTextured = -1;

/* Shader program and uniforms */
static GLuint s_program = 0;
static GLint  s_locPosition = -1;
static GLint  s_locColor = -1;
static GLint  s_locTexCoord = -1;
static GLint  s_locMvp = -1;
static GLint  s_locTexture = -1;
static GLint  s_locTextured = -1;
static GLint  s_locTexEnv = -1;
static GLint  s_locAlphaTest = -1;
static GLint  s_locAlphaRef = -1;
static GLuint s_whiteTexture = 0;
static int    s_shadersInited = 0;

/* Texture array and dirty tracking */
GLuint s_glTextures[52];
static int s_glTexturesInited = 0;
int s_glTextureDirty[52];
static unsigned char s_tpageFilter[52];
static unsigned char s_glTextureFilter[52];
static int s_tpageKeepPixels[52];
static int s_tpageNoColorKey[52];
static int s_tpageGreen6[52];
static unsigned char *s_tpageRGBA8Buf[52];
static unsigned char *s_pendingRGBA[52];
static int s_pendingRGBAWidth[52];
static int s_pendingRGBAHeight[52];

/* Static conversion buffer for RGBA8 uploads (max 1024x512x4 = 2MB) */
static uint8_t s_rgba8Storage[1024 * 512 * 4];

/* =====================================================================
 * State Tracking
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
    .scissorX       = 0,
    .scissorY       = 0,
    .scissorW       = 640,
    .scissorH       = 480,
};

/* =====================================================================
 * Shader Compilation & Init
 * ===================================================================== */

static const char *s_vertexShaderSource =
    "attribute vec4 a_position;\n"
    "attribute vec4 a_color;\n"
    "attribute vec2 a_texcoord;\n"
    "uniform mat4 u_mvp;\n"
    "varying vec4 v_color;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    float w = (a_position.w > 0.0) ? (1.0 / a_position.w) : 1.0;\n"
    "    gl_Position = u_mvp * vec4(a_position.xyz * w, w);\n"
    "    v_color = a_color;\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";

static const char *s_fragmentShaderSource =
    "precision mediump float;\n"
    "varying vec4 v_color;\n"
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "uniform int u_textured;\n"
    "uniform int u_texEnv;\n"
    "uniform int u_alphaTest;\n"
    "uniform float u_alphaRef;\n"
    "void main() {\n"
    "    vec4 texColor = (u_textured != 0) ? texture2D(u_texture, v_texcoord) : vec4(1.0);\n"
    "    vec4 finalColor;\n"
    "    if (u_textured != 0) {\n"
    "        if (u_texEnv == 1) {\n"
    "            finalColor.rgb = clamp(texColor.rgb + v_color.rgb - 0.5, 0.0, 1.0);\n"
    "            finalColor.a = texColor.a * v_color.a;\n"
    "        } else {\n"
    "            finalColor = texColor * v_color;\n"
    "        }\n"
    "    } else {\n"
    "        finalColor = v_color;\n"
    "    }\n"
    "    if (u_alphaTest != 0 && finalColor.a <= u_alphaRef) {\n"
    "        discard;\n"
    "    }\n"
    "    gl_FragColor = finalColor;\n"
    "}\n";

static GLuint CompileShader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    if (!shader) {
        SDL_Log("GLES2: glCreateShader failed for type %d", type);
        return 0;
    }
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint infoLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen > 1) {
            char *infoLog = (char *)malloc(infoLen);
            glGetShaderInfoLog(shader, infoLen, NULL, infoLog);
            SDL_Log("GLES2: Error compiling shader (%s):\n%s",
                    (type == GL_VERTEX_SHADER) ? "vertex" : "fragment", infoLog);
            free(infoLog);
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static int InitGLES2Pipeline(void)
{
    if (s_shadersInited) {
        return 0;
    }

    GLuint vs = CompileShader(GL_VERTEX_SHADER, s_vertexShaderSource);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, s_fragmentShaderSource);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return -1;
    }

    s_program = glCreateProgram();
    glAttachShader(s_program, vs);
    glAttachShader(s_program, fs);

    glBindAttribLocation(s_program, 0, "a_position");
    glBindAttribLocation(s_program, 1, "a_color");
    glBindAttribLocation(s_program, 2, "a_texcoord");

    glLinkProgram(s_program);

    GLint linked = 0;
    glGetProgramiv(s_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint infoLen = 0;
        glGetProgramiv(s_program, GL_INFO_LOG_LENGTH, &infoLen);
        if (infoLen > 1) {
            char *infoLog = (char *)malloc(infoLen);
            glGetProgramInfoLog(s_program, infoLen, NULL, infoLog);
            SDL_Log("GLES2: Error linking shader program:\n%s", infoLog);
            free(infoLog);
        }
        glDeleteProgram(s_program);
        s_program = 0;
        return -1;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    s_locPosition  = glGetAttribLocation(s_program, "a_position");
    s_locColor     = glGetAttribLocation(s_program, "a_color");
    s_locTexCoord  = glGetAttribLocation(s_program, "a_texcoord");
    s_locMvp       = glGetUniformLocation(s_program, "u_mvp");
    s_locTexture   = glGetUniformLocation(s_program, "u_texture");
    s_locTextured  = glGetUniformLocation(s_program, "u_textured");
    s_locTexEnv    = glGetUniformLocation(s_program, "u_texEnv");
    s_locAlphaTest = glGetUniformLocation(s_program, "u_alphaTest");
    s_locAlphaRef  = glGetUniformLocation(s_program, "u_alphaRef");

    /* Create 1x1 placeholder white texture */
    glGenTextures(1, &s_whiteTexture);
    glBindTexture(GL_TEXTURE_2D, s_whiteTexture);
    uint32_t whitePixel = 0xFFFFFFFF;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &whitePixel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Create 52 tpage texture objects */
    glGenTextures(52, s_glTextures);
    for (int i = 0; i < 52; i++) {
        glBindTexture(GL_TEXTURE_2D, s_glTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &whitePixel);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_glTextureDirty[i] = 1;
        s_glTextureFilter[i] = 0;
        s_tpageFilter[i] = 0;
        s_tpageNoColorKey[i] = 0;
        s_tpageGreen6[i] = 0;
        s_tpageKeepPixels[i] = 0;
        s_tpageRGBA8Buf[i] = NULL;
        s_pendingRGBA[i] = NULL;
    }
    s_glTexturesInited = 1;

    s_shadersInited = 1;
    glUseProgram(s_program);
    glUniform1i(s_locTexture, 0);
    s_lastMvpW = 0;
    s_lastMvpH = 0;
    s_currentTextured = -1;
    SDL_Log("GLES2: Texturing pipeline initialized successfully");
    return 0;
}

/* =====================================================================
 * State Management & Flushing
 * ===================================================================== */

static void UploadTpageInternal(int tpage)
{
    if (tpage < 0 || tpage >= 52) {
        return;
    }

    int w = g_tpageWidth[tpage];
    int h = g_tpageHeight[tpage];
    if (w <= 0) w = 256;
    if (h <= 0) h = 256;

    glBindTexture(GL_TEXTURE_2D, s_glTextures[tpage]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    s_glTextureFilter[tpage] = 0;

    /* Persistent full-quality RGBA8 override (32-bit sky) */
    if (s_tpageRGBA8Buf[tpage]) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_tpageRGBA8Buf[tpage]);
        s_glTextureDirty[tpage] = 0;
        return;
    }

    /* Check for pending full-quality RGBA data (from TintBackgroundTPage) */
    if (s_pendingRGBA[tpage]) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                     s_pendingRGBAWidth[tpage], s_pendingRGBAHeight[tpage], 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_pendingRGBA[tpage]);
        free(s_pendingRGBA[tpage]);
        s_pendingRGBA[tpage] = NULL;
        s_glTextureDirty[tpage] = 0;
        return;
    }

    unsigned short *pixels = (unsigned short *)g_tpagePixelBuf[tpage];
    if (!pixels) {
        return;
    }

    int count = w * h;
    if (count > (int)(sizeof(s_rgba8Storage) / 4)) {
        count = sizeof(s_rgba8Storage) / 4;
    }

    uint8_t *out8 = s_rgba8Storage;
    if (s_tpageNoColorKey[tpage]) {
        int green6 = s_tpageGreen6[tpage];
        for (int i = 0; i < count; i++) {
            unsigned short p = pixels[i];
            unsigned char r5 = (p >> 11) & 0x1F;
            unsigned char b5 = p & 0x1F;
            unsigned char g8;
            if (green6) {
                unsigned char g6 = (p >> 5) & 0x3F; /* true 6-bit green */
                g8 = (unsigned char)((g6 << 2) | (g6 >> 4));
            } else {
                unsigned char g5 = (p >> 6) & 0x1F;
                g8 = (unsigned char)((g5 << 3) | (g5 >> 2));
            }
            out8[i * 4 + 0] = (r5 << 3) | (r5 >> 2);
            out8[i * 4 + 1] = g8;
            out8[i * 4 + 2] = (b5 << 3) | (b5 >> 2);
            out8[i * 4 + 3] = 0xFF;
        }
    } else {
        for (int i = 0; i < count; i++) {
            unsigned short p = pixels[i];
            unsigned char r5 = (p >> 11) & 0x1F;
            unsigned char g5 = (p >> 6) & 0x1F;
            unsigned char b5 = p & 0x1F;
            unsigned char a = IS_COLOR_KEY_RGB5(r5, g5, b5) ? 0 : 0xFF;
            out8[i * 4 + 0] = (r5 << 3) | (r5 >> 2);
            out8[i * 4 + 1] = (g5 << 3) | (g5 >> 2);
            out8[i * 4 + 2] = (b5 << 3) | (b5 >> 2);
            out8[i * 4 + 3] = a;
        }
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, out8);
    s_glTextureDirty[tpage] = 0;
}

/* Forward declaration for batch flushing */
void R_FlushBatch(void);

static inline void CopyVertex(GLES2Vertex *dst, const RenderVertex *src)
{
    dst->x = src->sx;
    dst->y = src->sy;
    dst->z = src->sz;
    dst->rhw = src->rhw;

    uint32_t c = src->color;
    dst->a = (uint8_t)((c >> 24) & 0xFF);
    dst->r = (uint8_t)((c >> 16) & 0xFF);
    dst->g = (uint8_t)((c >>  8) & 0xFF);
    dst->b = (uint8_t)((c      ) & 0xFF);

    dst->u = src->u;
    dst->v = src->v;
}

static inline int StateDiffers(const R_StateSnapshot *a, const R_StateSnapshot *b)
{
    if (a->textureId != b->textureId) return 1;
    if (a->blendMode != b->blendMode) return 1;
    if (a->depthTest != b->depthTest) return 1;
    if (a->depthFunc != b->depthFunc) return 1;
    if (a->depthWrite != b->depthWrite) return 1;
    if (a->texEnv != b->texEnv) return 1;
    if (a->alphaTest != b->alphaTest) return 1;
    if (a->alphaRef != b->alphaRef) return 1;
    if (a->scissorEnabled != b->scissorEnabled) return 1;
    if (a->scissorEnabled) {
        if (a->scissorX != b->scissorX || a->scissorY != b->scissorY ||
            a->scissorW != b->scissorW || a->scissorH != b->scissorH) return 1;
    }
    if (a->textureId >= 0 && a->textureId < 52) {
        if (s_glTextureDirty[a->textureId]) return 1;
        unsigned char want = s_tpageFilter[a->textureId];
        if (want == 0) want = (unsigned char)(a->filter + 1);
        if (s_glTextureFilter[a->textureId] != want) return 1;
    }
    return 0;
}

static void UploadTpage(int tpage)
{
    if (s_batchCount > 0) {
        R_FlushBatch();
    }
    uint64_t t0 = GetTimeNs();
    UploadTpageInternal(tpage);
    s_timeTpageUploadNs += (GetTimeNs() - t0);
    s_tpageUploadCount++;
}

static void ApplyTpageFilter(int tp)
{
    if (tp < 0 || tp >= 52) return;
    unsigned char want = s_tpageFilter[tp];
    if (want == 0) {
        want = (unsigned char)(s_desired.filter + 1);
    }
    if (s_glTextureFilter[tp] != want) {
        GLenum f = (want == (unsigned char)(R_FILTER_LINEAR + 1)) ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        s_glTextureFilter[tp] = want;
    }
}

void R_FlushState(void)
{
    if (!s_shadersInited) {
        if (InitGLES2Pipeline() != 0) return;
    }
    if (s_batchCount > 0) {
        R_FlushBatch();
    }
    uint64_t t0 = GetTimeNs();

    glUseProgram(s_program);

    /* Update MVP matrix uniform only if screen dimensions changed */
    int sw = g_screenWidth > 0 ? g_screenWidth : 640;
    int sh = g_screenHeight > 0 ? g_screenHeight : 480;
    if (sw != s_lastMvpW || sh != s_lastMvpH) {
        float w = (float)sw;
        float h = (float)sh;
        float mvp[16] = {
            2.0f / w,  0.0f,       0.0f,  0.0f,
            0.0f,     -2.0f / h,   0.0f,  0.0f,
            0.0f,      0.0f,       2.0f,  0.0f,
           -1.0f,      1.0f,      -1.0f,  1.0f
        };
        glUniformMatrix4fv(s_locMvp, 1, GL_FALSE, mvp);
        s_lastMvpW = sw;
        s_lastMvpH = sh;
    }

    /* Texture binding and state update */
    int tp = s_desired.textureId;
    int isTextured = 0;
    if (tp >= 0 && tp < 52) {
        if (g_tpagePixelBuf[tp] != NULL || s_tpageRGBA8Buf[tp] != NULL || s_pendingRGBA[tp] != NULL) {
            if (s_glTextureDirty[tp]) {
                UploadTpage(tp);
            }
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, s_glTextures[tp]);
            ApplyTpageFilter(tp);
            isTextured = 1;
        }
    }

    if (!isTextured) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_whiteTexture);
    }

    if (s_currentTextured != isTextured) {
        glUniform1i(s_locTextured, isTextured);
        s_currentTextured = isTextured;
    }

    if (s_desired.texEnv != s_current.texEnv) {
        glUniform1i(s_locTexEnv, (s_desired.texEnv == R_TEXENV_ADD_SIGNED) ? 1 : 0);
        s_current.texEnv = s_desired.texEnv;
    }
    s_current.textureId = s_desired.textureId;

    /* Blend Mode */
    if (s_desired.blendMode != s_current.blendMode) {
        switch (s_desired.blendMode) {
            case R_BLEND_NONE:
                glDisable(GL_BLEND);
                break;
            case R_BLEND_ALPHA:
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                break;
            case R_BLEND_ADDITIVE:
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE);
                break;
        }
        s_current.blendMode = s_desired.blendMode;
    }

    /* Depth test */
    if (s_desired.depthTest != s_current.depthTest) {
        if (s_desired.depthTest) {
            glEnable(GL_DEPTH_TEST);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
        s_current.depthTest = s_desired.depthTest;
    }

    /* Depth func */
    if (s_desired.depthFunc != s_current.depthFunc) {
        switch (s_desired.depthFunc) {
            case R_DEPTH_LEQUAL: glDepthFunc(GL_LEQUAL); break;
            case R_DEPTH_LESS:   glDepthFunc(GL_LESS); break;
            case R_DEPTH_ALWAYS: glDepthFunc(GL_ALWAYS); break;
        }
        s_current.depthFunc = s_desired.depthFunc;
    }

    /* Depth write */
    if (s_desired.depthWrite != s_current.depthWrite) {
        glDepthMask(s_desired.depthWrite ? GL_TRUE : GL_FALSE);
        s_current.depthWrite = s_desired.depthWrite;
    }

    /* Alpha test */
    if (s_desired.alphaTest != s_current.alphaTest) {
        glUniform1i(s_locAlphaTest, s_desired.alphaTest ? 1 : 0);
        s_current.alphaTest = s_desired.alphaTest;
    }
    if (s_desired.alphaRef != s_current.alphaRef) {
        glUniform1f(s_locAlphaRef, s_desired.alphaRef);
        s_current.alphaRef = s_desired.alphaRef;
    }

    /* Culling: Sonic R does software culling; GPU face culling stays disabled */
    glDisable(GL_CULL_FACE);

    /* Scissor test */
    if (s_desired.scissorEnabled != s_current.scissorEnabled) {
        if (s_desired.scissorEnabled) {
            glEnable(GL_SCISSOR_TEST);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
        s_current.scissorEnabled = s_desired.scissorEnabled;
    }

    if (s_desired.scissorEnabled &&
        (s_desired.scissorX != s_current.scissorX ||
         s_desired.scissorY != s_current.scissorY ||
         s_desired.scissorW != s_current.scissorW ||
         s_desired.scissorH != s_current.scissorH))
    {
        glScissor(s_desired.scissorX, s_desired.scissorY,
                  s_desired.scissorW, s_desired.scissorH);
        s_current.scissorX = s_desired.scissorX;
        s_current.scissorY = s_desired.scissorY;
        s_current.scissorW = s_desired.scissorW;
        s_current.scissorH = s_desired.scissorH;
    }
    s_timeStateFlushNs += (GetTimeNs() - t0);
}


void R_SetTexture(int tpageIndex)
{
    s_desired.textureId = tpageIndex;
}

void R_SetBlendMode(R_BlendMode mode)
{
    s_desired.blendMode = mode;
}

void R_SetDepthTest(int enable)
{
    s_desired.depthTest = enable;
}

void R_SetDepthFunc(R_DepthFunc func)
{
    s_desired.depthFunc = func;
}

void R_SetDepthWrite(int enable)
{
    s_desired.depthWrite = enable;
}

void R_SetTexEnv(R_TexEnvMode mode)
{
    s_desired.texEnv = mode;
}

void R_SetFilter(R_FilterMode mode)
{
    s_desired.filter = mode;
}

void R_SetTpageFilter(int tpage, R_FilterMode mode)
{
    if (tpage >= 0 && tpage < 52) {
        s_tpageFilter[tpage] = (unsigned char)(mode + 1);
        s_glTextureFilter[tpage] = 0;
    }
}

void R_ClearTpageFilter(int tpage)
{
    if (tpage >= 0 && tpage < 52) {
        s_tpageFilter[tpage] = 0;
        s_glTextureFilter[tpage] = 0;
    }
}

void R_SetTpageSatBoost(int tpage, int k256)
{
    (void)tpage; (void)k256;
}

void R_SetCullMode(R_CullMode mode)
{
    s_desired.cullMode = mode;
}

void R_SetAlphaTest(int enable)
{
    s_desired.alphaTest = enable;
}

void R_SetAlphaRef(float ref)
{
    s_desired.alphaRef = ref;
}

void R_SetScissor(int x, int y, int w, int h)
{
    s_desired.scissorEnabled = 1;
    s_desired.scissorX = g_glViewportOffsetX + (x * g_glBackingWidth) / (g_screenWidth > 0 ? g_screenWidth : 640);
    s_desired.scissorY = g_glViewportOffsetY + ((g_screenHeight - y - h) * g_glBackingHeight) / (g_screenHeight > 0 ? g_screenHeight : 480);
    s_desired.scissorW = (w * g_glBackingWidth) / (g_screenWidth > 0 ? g_screenWidth : 640);
    s_desired.scissorH = (h * g_glBackingHeight) / (g_screenHeight > 0 ? g_screenHeight : 480);
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
        if (s_batchCount > 0) {
            R_FlushBatch();
        }
        s_desired = s_stateStack[--s_stackDepth];
    }
}

void R_ResetState(void)
{
    if (s_batchCount > 0) {
        R_FlushBatch();
    }
    s_desired = s_defaults;
    memset(&s_current, 0xFF, sizeof(s_current));
    s_current.textureId = -99;
    s_current.alphaRef = -1.0f;
    s_currentTextured = -1;
    memset(s_glTextureFilter, 0, sizeof(s_glTextureFilter));
    s_stackDepth = 0;
}

void R_DebugGetState(int *depthTest, int *depthWrite, int *depthFunc,
                     int *blendMode, int *alphaTest, float *alphaRef)
{
    if (depthTest)  *depthTest  = s_current.depthTest;
    if (depthWrite) *depthWrite = s_current.depthWrite;
    if (depthFunc)  *depthFunc  = s_current.depthFunc;
    if (blendMode)  *blendMode  = s_current.blendMode;
    if (alphaTest)  *alphaTest  = s_current.alphaTest;
    if (alphaRef)   *alphaRef   = s_current.alphaRef;
}

/* =====================================================================
 * Geometry Submission & Dynamic Batching
 * ===================================================================== */

static void EnsureBatchSpaceAndState(int neededVerts)
{
    if (s_batchCount + neededVerts > MAX_BATCH_VERTS) {
        R_FlushBatch();
    }
    int sw = g_screenWidth > 0 ? g_screenWidth : 640;
    int sh = g_screenHeight > 0 ? g_screenHeight : 480;
    if (!s_shadersInited || StateDiffers(&s_desired, &s_current) ||
        sw != s_lastMvpW || sh != s_lastMvpH)
    {
        if (s_batchCount > 0) {
            R_FlushBatch();
        }
        R_FlushState();
    }
}

void R_FlushBatch(void)
{
    if (s_batchCount == 0) {
        return;
    }
    s_statDrawCalls++;

    uint64_t t_draw0 = GetTimeNs();
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glEnableVertexAttribArray(s_locPosition);
    glVertexAttribPointer(s_locPosition, 4, GL_FLOAT, GL_FALSE,
                          sizeof(GLES2Vertex), (const void *)&s_batchVerts[0].x);

    glEnableVertexAttribArray(s_locColor);
    glVertexAttribPointer(s_locColor, 4, GL_UNSIGNED_BYTE, GL_TRUE,
                          sizeof(GLES2Vertex), (const void *)&s_batchVerts[0].r);

    if (s_locTexCoord >= 0) {
        glEnableVertexAttribArray(s_locTexCoord);
        glVertexAttribPointer(s_locTexCoord, 2, GL_FLOAT, GL_FALSE,
                              sizeof(GLES2Vertex), (const void *)&s_batchVerts[0].u);
    }

    glDrawArrays(GL_TRIANGLES, 0, s_batchCount);
    s_timeDrawArraysNs += (GetTimeNs() - t_draw0);

    s_batchCount = 0;
}

void R_DrawQuad(const RenderVertex v[4])
{
    if (!v) return;
    EnsureBatchSpaceAndState(6);
    s_statVertCount += 4;
    GLES2Vertex *p = &s_batchVerts[s_batchCount];
    /* Tri 1: 0, 1, 2 */
    CopyVertex(&p[0], &v[0]);
    CopyVertex(&p[1], &v[1]);
    CopyVertex(&p[2], &v[2]);
    /* Tri 2: 0, 2, 3 */
    CopyVertex(&p[3], &v[0]);
    CopyVertex(&p[4], &v[2]);
    CopyVertex(&p[5], &v[3]);
    s_batchCount += 6;
}

void R_DrawTri(const RenderVertex v[3])
{
    if (!v) return;
    EnsureBatchSpaceAndState(3);
    s_statVertCount += 3;
    GLES2Vertex *p = &s_batchVerts[s_batchCount];
    CopyVertex(&p[0], &v[0]);
    CopyVertex(&p[1], &v[1]);
    CopyVertex(&p[2], &v[2]);
    s_batchCount += 3;
}

void R_DrawTriFan(const RenderVertex *v, int count)
{
    if (!v || count < 3) return;
    if (count == 4) {
        R_DrawQuad(v);
        return;
    }
    if (count == 3) {
        R_DrawTri(v);
        return;
    }
    int numTris = count - 2;
    int needed = numTris * 3;
    EnsureBatchSpaceAndState(needed);
    s_statVertCount += count;
    GLES2Vertex *p = &s_batchVerts[s_batchCount];
    int idx = 0;
    for (int i = 1; i <= numTris; i++) {
        CopyVertex(&p[idx++], &v[0]);
        CopyVertex(&p[idx++], &v[i]);
        CopyVertex(&p[idx++], &v[i + 1]);
    }
    s_batchCount += needed;
}

void R_DrawQuad2D(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1,
                  float z, uint32_t color)
{
    float rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z / farSafe;

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

    float rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z / farSafe;

    RenderVertex v[4];
    v[0] = (RenderVertex){ x0, y0, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[1] = (RenderVertex){ x1, y0, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[2] = (RenderVertex){ x1, y1, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[3] = (RenderVertex){ x0, y1, normZ, rhw, color, 0, 0.0f, 0.0f };
    R_DrawQuad(v);

    s_desired.textureId = savedTex;
}

/* =====================================================================
 * Synthetic Test Helper (SONICR_ANDROID_RENDER_TEST)
 * ===================================================================== */

#ifdef SONICR_ANDROID_RENDER_TEST
static void RenderSyntheticTest(void)
{
    /* 1. Red Triangle in top-left */
    RenderVertex tri[3];
    tri[0] = (RenderVertex){ 100.0f,  80.0f, 0.5f, 1.0f, 0xFFFF0000, 0, 0.0f, 0.0f };
    tri[1] = (RenderVertex){ 250.0f,  80.0f, 0.5f, 1.0f, 0xFFFF0000, 0, 0.0f, 0.0f };
    tri[2] = (RenderVertex){ 175.0f, 230.0f, 0.5f, 1.0f, 0xFFFF0000, 0, 0.0f, 0.0f };
    R_DrawTri(tri);

    /* 2. Green Quad in center */
    RenderVertex quad[4];
    quad[0] = (RenderVertex){ 240.0f, 160.0f, 0.4f, 1.0f, 0xFF00FF00, 0, 0.0f, 0.0f };
    quad[1] = (RenderVertex){ 400.0f, 160.0f, 0.4f, 1.0f, 0xFF00FF00, 0, 0.0f, 0.0f };
    quad[2] = (RenderVertex){ 400.0f, 320.0f, 0.4f, 1.0f, 0xFF00FF00, 0, 0.0f, 0.0f };
    quad[3] = (RenderVertex){ 240.0f, 320.0f, 0.4f, 1.0f, 0xFF00FF00, 0, 0.0f, 0.0f };
    R_DrawQuad(quad);

    /* 3. Yellow 2D Quad in bottom-right */
    R_DrawQuad2DSolid(420.0f, 260.0f, 560.0f, 400.0f, 0.3f, 0xFFFFFF00);

    /* 4. Textured Quad with Color-Keyed Hole in bottom-left (tpage 51) */
    if (g_tpagePixelBuf[51] == NULL) {
        g_tpagePixelBuf[51] = malloc(64 * 64 * sizeof(unsigned short));
        unsigned short *p = (unsigned short *)g_tpagePixelBuf[51];
        for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 64; x++) {
                /* Outer 8-pixel border: Magenta (0xF81F: R=31, G=0, B=31)
                 * Inner 48x48: Color Key (0x07C0: R=0, G=31, B=0) */
                if (x < 8 || x >= 56 || y < 8 || y >= 56) {
                    p[y * 64 + x] = 0xF81F;
                } else {
                    p[y * 64 + x] = 0x07C0;
                }
            }
        }
        g_tpageWidth[51] = 64;
        g_tpageHeight[51] = 64;
        s_glTextureDirty[51] = 1;
        s_tpageNoColorKey[51] = 0;
    }

    RenderVertex texQuad[4];
    texQuad[0] = (RenderVertex){  80.0f, 280.0f, 0.45f, 1.0f, 0xFFFFFFFF, 0, 0.0f, 0.0f };
    texQuad[1] = (RenderVertex){ 220.0f, 280.0f, 0.45f, 1.0f, 0xFFFFFFFF, 0, 1.0f, 0.0f };
    texQuad[2] = (RenderVertex){ 220.0f, 420.0f, 0.45f, 1.0f, 0xFFFFFFFF, 0, 1.0f, 1.0f };
    texQuad[3] = (RenderVertex){  80.0f, 420.0f, 0.45f, 1.0f, 0xFFFFFFFF, 0, 0.0f, 1.0f };

    R_SetTexture(51);
    R_SetBlendMode(R_BLEND_ALPHA);
    R_SetAlphaTest(1);
    R_SetAlphaRef(0.01f);
    R_SetTexEnv(R_TEXENV_MODULATE);
    R_DrawQuad(texQuad);
    R_SetTexture(-1);
}
#endif

/* =====================================================================
 * Frame Lifecycle
 * ===================================================================== */

void BeginFrame(void)
{
    int fullW = 0, fullH = 0;
    platform_get_drawable_size(&fullW, &fullH);
    if (fullW <= 0) fullW = 640;
    if (fullH <= 0) fullH = 480;

    int vpW, vpH;
    if (fullW * 3 > fullH * 4) {
        vpH = fullH;
        vpW = (fullH * 4) / 3;
    } else {
        vpW = fullW;
        vpH = (fullW * 3) / 4;
    }
    int offsetX = (fullW - vpW) / 2;
    int offsetY = (fullH - vpH) / 2;

    g_glViewportOffsetX = offsetX;
    g_glViewportOffsetY = offsetY;
    g_glBackingWidth = vpW;
    g_glBackingHeight = vpH;

    /* Set 4:3 active viewport */
    glViewport(offsetX, offsetY, vpW, vpH);

    R_ResetState();
}

void EndFrame(void)
{
    if (s_batchCount > 0) {
        R_FlushBatch();
    }
}

void R_EndFrame(void)
{
    if (s_batchCount > 0) {
        R_FlushBatch();
    }
}

void FlipD3D(void)
{
    if (s_batchCount > 0) {
        R_FlushBatch();
    }
#ifdef SONICR_ANDROID_RENDER_TEST
    int fullW = 0, fullH = 0;
    platform_get_drawable_size(&fullW, &fullH);
    if (fullW <= 0) fullW = 640;
    if (fullH <= 0) fullH = 480;

    int vpW, vpH;
    if (fullW * 3 > fullH * 4) {
        vpH = fullH;
        vpW = (fullH * 4) / 3;
    } else {
        vpW = fullW;
        vpH = (fullW * 3) / 4;
    }
    int offsetX = (fullW - vpW) / 2;
    int offsetY = (fullH - vpH) / 2;

    g_glViewportOffsetX = offsetX;
    g_glViewportOffsetY = offsetY;
    g_glBackingWidth = vpW;
    g_glBackingHeight = vpH;

    /* Clear full window to Sonic Blue */
    glViewport(0, 0, fullW, fullH);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.1215f, 0.2784f, 0.6510f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* Render synthetic test shapes in 4:3 active viewport */
    glViewport(offsetX, offsetY, vpW, vpH);
    R_ResetState();
    RenderSyntheticTest();
#endif
    uint64_t nowNs = GetTimeNs();
    uint64_t frameDeltaNs = (s_lastFlipNs > 0) ? (nowNs - s_lastFlipNs) : 0;
    s_lastFlipNs = nowNs;

    int fullW = 0, fullH = 0;
    platform_get_drawable_size(&fullW, &fullH);

    uint64_t t_touch0 = GetTimeNs();
    TouchOverlay_Render(fullW, fullH);
    s_timeTouchOverlayNs += (GetTimeNs() - t_touch0);

    R_ResetState();
    glDepthMask(GL_TRUE);

    s_statFrameCount++;

    uint64_t t_swap0 = GetTimeNs();
    platform_gl_swap();
    s_timeSwapNs += (GetTimeNs() - t_swap0);

    /* Telemetry Accumulation */
    s_accFrames++;
    s_accTotalNs += frameDeltaNs;
    s_accLogicNs += g_perfLogicNs;
    s_accStateFlushNs += s_timeStateFlushNs;
    s_accVboUploadNs += s_timeVboUploadNs;
    s_accDrawArraysNs += s_timeDrawArraysNs;
    s_accTpageUploadNs += s_timeTpageUploadNs;
    s_accTpageUploadCount += s_tpageUploadCount;
    s_accTouchOverlayNs += s_timeTouchOverlayNs;
    s_accSwapNs += s_timeSwapNs;
    s_accWaitCapNs += g_perfWaitCapNs;
    s_accDrawCalls += s_statDrawCalls;
    s_accVertCount += s_statVertCount;

    /* Warn on noticeably slow individual racing frames (> 66ms / < 15 FPS) */
    if (frameDeltaNs > 66000000ULL && s_statVertCount > 500) {
        GLES_LOGW("PERF-SLOW: frame=%.1fms | Logic=%.1fms | Render[VBO=%.1fms Draw=%.1fms State=%.1fms TpUp=%.1fms(cnt=%d)] | Swap=%.1fms Touch=%.1fms | Draws=%d Verts=%d",
                  (double)frameDeltaNs / 1e6,
                  (double)g_perfLogicNs / 1e6,
                  (double)s_timeVboUploadNs / 1e6,
                  (double)s_timeDrawArraysNs / 1e6,
                  (double)s_timeStateFlushNs / 1e6,
                  (double)s_timeTpageUploadNs / 1e6,
                  s_tpageUploadCount,
                  (double)s_timeSwapNs / 1e6,
                  (double)s_timeTouchOverlayNs / 1e6,
                  s_statDrawCalls, s_statVertCount);
    }

    if (s_accFrames >= 30) {
        double totalSec = (double)s_accTotalNs / 1e9;
        double fps = (totalSec > 0.0001) ? ((double)s_accFrames / totalSec) : 0.0;
        double avgFrameMs = ((double)s_accTotalNs / s_accFrames) / 1e6;
        double avgLogicMs = ((double)s_accLogicNs / s_accFrames) / 1e6;
        double avgVboMs   = ((double)s_accVboUploadNs / s_accFrames) / 1e6;
        double avgDrawMs  = ((double)s_accDrawArraysNs / s_accFrames) / 1e6;
        double avgStateMs = ((double)s_accStateFlushNs / s_accFrames) / 1e6;
        double avgTpUpMs  = ((double)s_accTpageUploadNs / s_accFrames) / 1e6;
        double avgSwapMs  = ((double)s_accSwapNs / s_accFrames) / 1e6;
        double avgTouchMs = ((double)s_accTouchOverlayNs / s_accFrames) / 1e6;
        double avgCapMs   = ((double)s_accWaitCapNs / s_accFrames) / 1e6;
        int avgDraws      = s_accDrawCalls / s_accFrames;
        int avgVerts      = s_accVertCount / s_accFrames;

        GLES_LOGI("PERF: FPS=%.1f (avg=%.1fms) | Logic=%.1fms | Render[VBO=%.1fms Draw=%.1fms State=%.1fms TpUp=%.1fms(cnt=%d)] | Swap=%.1fms Touch=%.1fms Cap=%.1fms | Draws=%d Verts=%d",
                  fps, avgFrameMs, avgLogicMs, avgVboMs, avgDrawMs, avgStateMs, avgTpUpMs,
                  s_accTpageUploadCount, avgSwapMs, avgTouchMs, avgCapMs, avgDraws, avgVerts);

        s_accFrames = 0;
        s_accTotalNs = 0;
        s_accLogicNs = 0;
        s_accStateFlushNs = 0;
        s_accVboUploadNs = 0;
        s_accDrawArraysNs = 0;
        s_accTpageUploadNs = 0;
        s_accTpageUploadCount = 0;
        s_accTouchOverlayNs = 0;
        s_accSwapNs = 0;
        s_accWaitCapNs = 0;
        s_accDrawCalls = 0;
        s_accVertCount = 0;
    }

    /* Reset per-frame counters */
    g_perfLogicNs = 0;
    g_perfWaitCapNs = 0;
    s_timeStateFlushNs = 0;
    s_timeVboUploadNs = 0;
    s_timeDrawArraysNs = 0;
    s_timeTpageUploadNs = 0;
    s_tpageUploadCount = 0;
    s_timeTouchOverlayNs = 0;
    s_timeSwapNs = 0;

    s_statDrawCalls = 0;
    s_statVertCount = 0;
    s_statColorClears = 0;
    s_statDepthClears = 0;
}

void R_Flip(void)
{
    FlipD3D();
}

void R_ClearDepth(void)
{
    if (s_batchCount > 0) {
        R_FlushBatch();
    }
    s_statDepthClears++;
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    if (!s_desired.depthWrite) {
        glDepthMask(GL_FALSE);
    }
}

/* =====================================================================
 * Texture Management API
 * ===================================================================== */

void R_InitTextures(void)
{
    if (!s_shadersInited) {
        InitGLES2Pipeline();
    }
}

void R_UploadTexture(int tpage)
{
    if (tpage >= 0 && tpage < 52) {
        UploadTpage(tpage);
    }
}

void R_UploadTextureRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    if (s_batchCount > 0) {
        R_FlushBatch();
    }
    if (!s_glTexturesInited) {
        InitGLES2Pipeline();
    }
    if (tpage < 0 || tpage >= 52 || !rgba) {
        return;
    }

    glBindTexture(GL_TEXTURE_2D, s_glTextures[tpage]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    s_glTextureFilter[tpage] = 0;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    s_glTextureDirty[tpage] = 0;
}

void R_UploadTextureSubRect(int tpage, int destX, int destY, int width, int height)
{
    if (s_batchCount > 0) {
        R_FlushBatch();
    }
    if (!s_glTexturesInited) {
        InitGLES2Pipeline();
    }
    if (tpage < 0 || tpage >= 52) {
        return;
    }

    unsigned short *pixels = (unsigned short *)g_tpagePixelBuf[tpage];
    if (pixels == NULL) {
        return;
    }

    int tpageW = g_tpageWidth[tpage];
    if (tpageW <= 0) {
        tpageW = 256;
    }

    glBindTexture(GL_TEXTURE_2D, s_glTextures[tpage]);
    uint8_t *out8 = s_rgba8Storage;
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            unsigned short p = pixels[(destY + row) * tpageW + (destX + col)];
            unsigned char r5 = (p >> 11) & 0x1F;
            unsigned char g5 = (p >> 6) & 0x1F;
            unsigned char b5 = p & 0x1F;
            unsigned char a = IS_COLOR_KEY_RGB5(r5, g5, b5) ? 0 : 0xFF;
            int idx = (row * width + col) * 4;
            out8[idx + 0] = (r5 << 3) | (r5 >> 2);
            out8[idx + 1] = (g5 << 3) | (g5 >> 2);
            out8[idx + 2] = (b5 << 3) | (b5 >> 2);
            out8[idx + 3] = a;
        }
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, destX, destY, width, height,
                    GL_RGBA, GL_UNSIGNED_BYTE, out8);
}

void R_MarkTextureDirty(int tpage)
{
    if (tpage >= 0 && tpage < 52) {
        if (s_batchCount > 0 && s_desired.textureId == tpage) {
            R_FlushBatch();
        }
        s_glTextureDirty[tpage] = 1;
        s_glTextureFilter[tpage] = 0;
    }
}

void R_ClearTextureDirty(int tpage)
{
    if (tpage >= 0 && tpage < 52) {
        s_glTextureDirty[tpage] = 0;
    }
}

void R_FreezeTexture(int tpage)
{
    if (!s_glTexturesInited) {
        InitGLES2Pipeline();
    }
    if (tpage >= 0 && tpage < 52 && g_tpagePixelBuf[tpage] != NULL) {
        UploadTpage(tpage);
    }
}

void R_ThawTexture(int tpage)
{
    (void)tpage;
}

void R_SetPendingRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    if (tpage >= 0 && tpage < 52) {
        if (s_pendingRGBA[tpage]) free(s_pendingRGBA[tpage]);
        s_pendingRGBA[tpage] = rgba;
        s_pendingRGBAWidth[tpage] = w;
        s_pendingRGBAHeight[tpage] = h;
        s_glTextureDirty[tpage] = 1;
    } else if (rgba) {
        free(rgba);
    }
}

void R_SetNoColorKey(int tpage)
{
    if (tpage >= 0 && tpage < 52) {
        s_tpageNoColorKey[tpage] = 1;
        s_glTextureDirty[tpage] = 1;
    }
}

void R_ClearNoColorKey(int tpage)
{
    if (tpage >= 0 && tpage < 52) {
        s_tpageNoColorKey[tpage] = 0;
        s_glTextureDirty[tpage] = 1;
    }
}

void R_SetTpageGreen6(int tpage, int on)
{
    if (tpage >= 0 && tpage < 52) {
        s_tpageGreen6[tpage] = on ? 1 : 0;
        s_glTextureDirty[tpage] = 1;
    }
}

void R_SetTpageRGBA8(int tpage, unsigned char *rgba)
{
    if (tpage < 0 || tpage >= 52) {
        if (rgba) free(rgba);
        return;
    }
    if (s_tpageRGBA8Buf[tpage] && s_tpageRGBA8Buf[tpage] != rgba) {
        free(s_tpageRGBA8Buf[tpage]);
    }
    s_tpageRGBA8Buf[tpage] = rgba;
    s_glTextureDirty[tpage] = 1;
}

void GL_KeepPixels(int tpage)
{
    if (tpage >= 0 && tpage < 52) {
        s_tpageKeepPixels[tpage] = 1;
    }
}

/* =====================================================================
 * TPage state-machine & viewport stubs
 * ===================================================================== */

void SetViewportFromConfig(int *config)
{
    if (config == NULL) {
        return;
    }
    g_clipLeft = config[0];
    g_clipLeftDouble = g_clipLeft * 2;
    g_clipTop = config[1];
    g_clipRight = config[2];
    g_clipBottom = config[3];
    g_projScaleX = config[4];
    g_projScaleXCurrent = config[5];
    g_projScaleY = config[6];
    g_screenCenterX = config[7];
    g_screenCenterY = config[8];
    g_screenWidthFull = config[9];
    g_screenHeightFull = config[0xa];
    g_vpClipLeft10 = config[0xb];
    g_vpClipRight10 = config[0xc];
    g_vpClipLeft16 = config[0xd];
    g_vpClipRight16 = config[0xe];
    g_vpParam0F = config[0xf];
    g_vpParam10 = config[0x10];
    g_dispCenterX = (g_clipLeft + g_clipRight) / 2;
    g_dispCenterY = (g_clipTop + g_clipBottom) / 2;
}

void ProcessTpageStates(void)
{
    /* Clear full window and depth at start of rendering passes */
    s_statColorClears++;
    s_statDepthClears++;
    R_DisableScissor();
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    R_SetDepthWrite(1);
    R_FlushState();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    int changed;
    do {
        changed = 0;
        for (int i = 0; i < 0x34; i++) {
            unsigned char state = (unsigned char)g_tpageStateArray[i];
            if (state > 7) {
                continue;
            }

            switch (state) {
                case 6:
                case 7:
                    if (state == 6) {
                        if (g_tpagePixelBuf[i] != NULL) {
                            if (!s_tpageKeepPixels[i]) {
                                free(g_tpagePixelBuf[i]);
                                g_tpagePixelBuf[i] = NULL;
                            }
                        }
                        s_glTextureDirty[i] = 1;
                        s_glTextureFilter[i] = 0;
                        if (s_pendingRGBA[i] != NULL) {
                            free(s_pendingRGBA[i]);
                            s_pendingRGBA[i] = NULL;
                        }
                    }
                    g_tpageStateArray[i] = 0;
                    break;

                case 1:
                case 2:
                    g_tpageStateArray[i] = 3;
                    changed = 1;
                    break;

                case 3:
                    g_tpageStateArray[i] = 4;
                    changed = 1;
                    break;

                case 5:
                    g_tpageStateArray[i] = 4;
                    break;

                case 0:
                case 4:
                default:
                    break;
            }
        }
    } while (changed);
}

void CleanupD3DTPages(void)
{
    for (int i = 0; i < 0x34; i++) {
        char state = g_tpageStateArray[i];
        if (state == 0 || state == 5 || i == TPAGE_PLATFORM_ICONS) {
            continue;
        }
        g_tpageStateArray[i] = 6;
    }
    ProcessTpageStates();
}

void R_ClearAndReset(void)
{
    ProcessTpageStates();
}

void FinalizeMenuTexturesD3D(void)
{
}

void RenderBackground(void)
{
    int sw = g_screenWidth > 0 ? g_screenWidth : 640;
    int sh = g_screenHeight > 0 ? g_screenHeight : 480;
    s_statColorClears++;
    R_SetScissor(0, 0, sw, sh);
    R_FlushState();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    R_DisableScissor();
    R_FlushState();
}

void RenderWavingMenuBackground(void)
{
    int tpage = g_uiTexPage;                                /* 0x8F6C48 */

    if (tpage < 0 || tpage >= 52) {
        return;
    }
    if (g_tpageStateArray[tpage] != 4) {
        return;
    }
    if (g_tpagePixelBuf[tpage] == NULL && s_pendingRGBA[tpage] == NULL && s_tpageRGBA8Buf[tpage] == NULL) {
        return;
    }

    struct IntVert { int sx, sy, uvU, uvV; };
    struct IntVert buf[8][8];

    int sinAngle = (g_totalFrames & 0x7F) << 5;             /* 0x4C68C9 */
    int yWorld   = 0x483;                                   /* 0x4C68C4 */
    int uvV      = 0x8000;                                  /* 0x4C68CE */

    for (int row = 0; row < 8; row++) {
        int vertAngle = sinAngle;                           /* 0x4C6965 */
        int xWorld    = -0x604;                             /* 0x4C6981 */
        int uvU       = 0x8000;                             /* 0x4C6960 */

        for (int col = 0; col < 8; col++) {
            /* 0x4C6996: depth = 0x8CA - (sin >> 7) */
            int depth = 0x8CA - (g_sinTable[vertAngle & 0xFFF] >> 7);

            /* 0x4C698D: centerX + projScaleX * xWorld / depth */
            buf[row][col].sx = g_screenCenterX + (g_projScaleXCurrent * xWorld) / depth;
            /* 0x4C69B8: centerY - projScaleY * yWorld / depth */
            buf[row][col].sy = g_screenCenterY - (g_projScaleY * yWorld) / depth;
            buf[row][col].uvU = uvU;
            buf[row][col].uvV = uvV;

            vertAngle = (vertAngle - 0x14D) & 0xFFF;        /* 0x4C69D0 */
            xWorld += 0x1B8;                                /* 0x4C69F1 */
            uvU += 0x246DB6;                                /* 0x4C69E9 */
        }

        sinAngle = (sinAngle - 0xDE) & 0xFFF;               /* 0x4C6935 */
        yWorld -= 0x14A;                                    /* 0x4C6928 */
        uvV += 0x246DB6;                                    /* 0x4C690E */
    }

    R_PushState();
    R_SetTexture(tpage);
    R_SetTexEnv(R_TEXENV_MODULATE);
    R_SetBlendMode(R_BLEND_ALPHA);
    R_SetDepthTest(1);
    R_SetDepthFunc(R_DEPTH_LEQUAL);
    R_SetDepthWrite(1);
    R_FlushState();

    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 7; col++) {
            /* 0x4C6A5C: quad corners are [row][col], [row][col+1],
             * [row+1][col+1], [row+1][col] */
            const struct IntVert *src[4] = {
                &buf[row][col],
                &buf[row][col + 1],
                &buf[row + 1][col + 1],
                &buf[row + 1][col]
            };

            RenderVertex rv[4];
            for (int k = 0; k < 4; k++) {
                rv[k].sx = (float)src[k]->sx;
                rv[k].sy = (float)src[k]->sy;
                rv[k].sz = 0.5f;
                rv[k].rhw = 1.0f;
                rv[k].color = VERTEX_WHITE;
                rv[k].specular = 0;
                rv[k].u = g_uvLUT256[(src[k]->uvU >> 16) & 0xFF];
                rv[k].v = g_uvLUT256[(src[k]->uvV >> 16) & 0xFF];
            }
            R_DrawQuad(rv);
        }
    }

    R_PopState();
}
