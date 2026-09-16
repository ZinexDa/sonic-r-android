/* =====================================================================
 * save_vmu.c — Dreamcast VMU save persistence + fileio shim.
 *
 * The cross-platform game writes three kinds of save file via fOpen/fRead/
 * fWrite/fClose (fileio.h):
 *   - SONICR.INF      (160 B  — game options/settings)
 *   - JOYSTICK.INF    (~1128 B — pad config)
 *   - SAVE/R01..R10.SAV (2216 B each — the 10 progress slots)
 *
 * On DC there is no writable POSIX filesystem on real hardware, so those
 * paths are routed here to the VMU instead. Everything else (track BINs,
 * textures, the icon) falls straight through to fopen() with the original
 * /cd -> /pc dcload fallback.
 *
 * Layout on the card (3 BIOS-visible saves, vmu_pkg-wrapped):
 *   SONICR_SAVE — the 10 slots bundled: [10 used-flags][10 x 2216]
 *   SONICR_OPT  — SONICR.INF payload
 *   SONICR_PAD  — JOYSTICK.INF payload
 *
 * Any single-slot save rewrites the whole SONICR_SAVE file (VMU files are
 * not randomly writable mid-file); all 10 slots live in a RAM cache so the
 * rewrite is always complete. See fileio.h for the macro wiring.
 * ===================================================================== */

#include <kos.h>
#include <dc/vmu_pkg.h>
#include <dc/maple.h>
#include <dc/vmufs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sonicr_paths.h"

/* LZFX save compression. Bigger hash table than the header's default (DC has
 * plenty of RAM); 2^13 entries -> ~32 KB transient table, much better ratio.
 * lzfx.h must be included by exactly ONE translation unit (its compressor
 * functions + htab have external linkage). */
#define LZFX_HLOG 13
#include "lzfx.h"

extern mutex_t io_lock;   /* defined in platform_dc.c */

/* Music pause/resume (music_dc.c). Maple-bus VMU I/O is slow enough to
 * starve the streamed-audio refill, so we pause around the card access —
 * the same convention the menus use around CD/PC texture loads. */
extern void PauseCD(void);
extern void ResumeCD(void);

#define SLOT_SIZE     2216          /* 554 * 4 — one R0N.SAV slot */
#define SLOT_COUNT    10
#define BUNDLE_PAYLOAD (SLOT_COUNT + SLOT_COUNT * SLOT_SIZE) /* flags + slots */

#define VMU_BUNDLE_FN "SONICR_SAVE"
#define VMU_OPT_FN    "SONICR_OPT"
#define VMU_PAD_FN    "SONICR_PAD"
#define VMU_NET_FN    "SONICR_NET"
#define VMU_KEY_FN    "SONICR_KEY"
#define VMU_GHO_FN    "SONICR_GHO"

/* DC ghost persistence. ghost.c collapses the character axis so there is one
 * ghost per (lapConfig, track) = 10 slots; all 10 live in SONICR_GHO as one
 * variable-length bundle, LZFX-compressed like the save bundle. Capacity is
 * gated once at boot (capacity_ensure): reserve 12 blocks for the core saves
 * (save 4 + opt 2 + pad 2 + net 2 + keys 2) and 40 blocks (~20 KB, ~10 compressed
 * ghosts) for the ghost bundle; the card is 200 blocks, 1-block icon per file. */
#define GHOST_SLOTS          10
#define MAX_GHOST_BYTES      11276   /* 12 + 2*GHOST_BUFFER_FRAMES(0x1600) */
#define VMU_BLOCK            512
#define CORE_RESERVE_BLOCKS  12
#define GHOST_RESERVE_BLOCKS 40
#define GHOST_BUNDLE_MAX     (8 + GHOST_SLOTS*8 + GHOST_SLOTS*MAX_GHOST_BYTES)
#define GHOST_MAGIC          0x4f484731u   /* "1GHO" */

/* Startup VMU capacity mode. */
enum { VMU_NORMAL = 0, VMU_NO_SAVE, VMU_RAM_GHOSTS, VMU_NO_CARD };

#define ICON_PATH     PATH_GENERAL "SONICR.ICO"   /* /cd/GENERAL/SONICR.ICO */

/* Every VMU payload is prefixed with this header (read/written via memcpy so
 * the u32 fields stay SH4 alignment-safe):
 *   [u32 origLen][u32 mode]  mode: VMODE_RAW or VMODE_LZFX, then the body.
 * Large files (the 22 KB bundle) compress; small INF/NET files stay raw. */
#define VMU_HDR       8
#define VMODE_RAW     0u
#define VMODE_LZFX    1u
#define VMU_COMPRESS_MIN 1024   /* don't bother compressing below this */

/* ---- virtual file handles ---- */
enum {
    K_BUNDLE,
    K_OPT,
    K_PAD,
    K_ONLINE,
    K_GHOST,
    K_KEYS
};

/* VMU file name / BIOS description for the whole-file (non-bundle) kinds. */
static const char *kind_fn(int kind)
{
    switch (kind) {
        case K_OPT:
            return VMU_OPT_FN;
        case K_PAD:
            return VMU_PAD_FN;
        case K_ONLINE:
            return VMU_NET_FN;
        case K_KEYS:
            return VMU_KEY_FN;
        default:
            return NULL;
    }
}
static const char *kind_desc(int kind)
{
    switch (kind) {
        case K_OPT:
            return "Sonic R Options";
        case K_PAD:
            return "Sonic R Pad Config";
        case K_ONLINE:
            return "Sonic R Online";
        case K_KEYS:
            return "Sonic R Keys";
        default:
            return "Sonic R";
    }
}

typedef struct {
    int kind;           /* K_BUNDLE / K_OPT / K_PAD */
    int slot;           /* bundle slot 0..9 */
    int writing;        /* opened for write */
    unsigned char *buf; /* data buffer */
    size_t cap;         /* buffer capacity */
    size_t pos;         /* read/write cursor */
    size_t len;         /* valid bytes (read) / bytes written */
} sr_vfile;

/* Active virtual handles — the game keeps at most one save file open at a
 * time (the load loop closes each slot before opening the next). */
#define MAX_VFILE 4
static sr_vfile *s_open[MAX_VFILE];

/* ---- the 10-slot bundle cache ---- */
static unsigned char s_bundle[SLOT_COUNT][SLOT_SIZE];
static unsigned char s_slotUsed[SLOT_COUNT];
static int s_bundleLoaded;   /* 0=not yet pulled from VMU this session */

/* Capacity mode, decided once (capacity_ensure). */
static int s_vmuMode = VMU_NORMAL;
static int s_vmuChecked;

/* The 10-slot ghost bundle (RAM). Each used slot is a malloc'd .gho payload;
 * seq gives LRU order for eviction when the 40-block budget is exceeded. */
static unsigned char *s_ghost[GHOST_SLOTS];
static unsigned int   s_ghostLen[GHOST_SLOTS];
static unsigned int   s_ghostSeq[GHOST_SLOTS];
static unsigned int   s_ghostNextSeq = 1;
static int            s_ghostLoaded;

/* =====================================================================
 * VMU device + raw file IO (the proven wipeout/doom64 recipe)
 * ===================================================================== */

static maple_device_t *vmu_dev(void)
{
    return maple_enum_type(0, MAPLE_FUNC_MEMCARD);
}

static void vmu_path(maple_device_t *d, const char *fn, char *out, size_t n)
{
    snprintf(out, n, "/vmu/%c%d/%s", 'a' + d->port, d->unit, fn);
}

/* Read a vmu_pkg file's payload into dst (<= dstmax). Returns payload length,
 * or -1 if absent / unreadable / parse failure. */
static int vmu_read(const char *fn, unsigned char *dst, int dstmax)
{
    maple_device_t *d = vmu_dev();
    if (!d) {
        return -1;
    }

    char path[40];
    vmu_path(d, fn, path, sizeof(path));

    PauseCD();   /* stop the music stream while the maple bus is busy */
    file_t f = fs_open(path, O_RDONLY | O_META);
    if (f == FILEHND_INVALID) {
        ResumeCD();
        return -1;
    }

    ssize_t size = fs_total(f);
    if (size <= 0) {
        fs_close(f);
        ResumeCD();
        return -1;
    }

    unsigned char *raw = malloc(size);
    if (!raw) {
        fs_close(f);
        ResumeCD();
        return -1;
    }

    ssize_t total = 0, r;
    while (total < size) {
        r = fs_read(f, raw + total, size - total);
        if (r < 0) {
            free(raw);
            fs_close(f);
            ResumeCD();
            return -1;
        }
        if (r == 0) {
            break;
        }
        total += r;
    }
    fs_close(f);
    ResumeCD();   /* maple I/O done — resume music; parse/decompress is CPU */

    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    if (vmu_pkg_parse(raw, total, &pkg) < 0) {
        free(raw);
        return -1;
    }

    /* Strip the [u32 origLen][u32 mode] header, then raw-copy or decompress. */
    if (pkg.data_len < VMU_HDR) {
        free(raw);
        return -1;
    }

    unsigned int origLen = 0;
    unsigned int mode = VMODE_RAW;
    memcpy(&origLen, pkg.data + 0, 4);
    memcpy(&mode, pkg.data + 4, 4);
    const unsigned char *body = pkg.data + VMU_HDR;
    unsigned int bodyLen = (unsigned int)(pkg.data_len - VMU_HDR);

    int out;
    if (mode == VMODE_LZFX) {
        unsigned int ulen = (unsigned int)dstmax;
        if (lzfx_decompress(body, bodyLen, dst, &ulen) < 0) {
            free(raw);
            return -1;
        }
        if (ulen != origLen) {
            free(raw);
            return -1;
        }   /* size mismatch -> corrupt */
        out = (int)ulen;
    } else {
        out = (bodyLen > (unsigned int)dstmax) ? dstmax : (int)bodyLen;
        memcpy(dst, body, (size_t)out);
    }
    free(raw);
    return out;
}

/* Build a vmu_pkg image (icon + [u32 origLen][u32 mode] + LZFX/raw body) into
 * *out (malloc'd; caller frees). Lets a caller size the packed image before
 * committing it. Returns 0 on success, -1 on failure. */
static int vmu_build(const char *desc, const unsigned char *data, int len,
                     unsigned char **out, int *outsz)
{
    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    strncpy(pkg.desc_short, "Sonic R", sizeof(pkg.desc_short) - 1);
    strncpy(pkg.desc_long, desc, sizeof(pkg.desc_long) - 1);
    strncpy(pkg.app_id, "SONICR", sizeof(pkg.app_id) - 1);

    /* Stored payload: [u32 origLen][u32 mode] + body. Body is LZFX-compressed
     * when it's large enough and actually shrinks; otherwise stored raw. */
    unsigned int origLen   = (unsigned int)len;
    unsigned int mode      = VMODE_RAW;
    unsigned int storedLen = (unsigned int)len;

    /* Worst case: incompressible input adds ~1 control byte per 32 literals. */
    unsigned int payloadCap = VMU_HDR + origLen + (origLen >> 5) + 64;
    unsigned char *payload = malloc(payloadCap);
    if (!payload) return -1;

    if (len > VMU_COMPRESS_MIN) {
        unsigned int clen = payloadCap - VMU_HDR;   /* available output space */
        if (lzfx_compress(data, origLen, payload + VMU_HDR, &clen) >= 0
            && clen < origLen) {
            mode = VMODE_LZFX;
            storedLen = clen;
        }
    }
    if (mode == VMODE_RAW) {
        memcpy(payload + VMU_HDR, data, (size_t)len);
        storedLen = origLen;
    }
    memcpy(payload + 0, &origLen, 4);
    memcpy(payload + 4, &mode,    4);

    pkg.data_len = (int)(VMU_HDR + storedLen);
    pkg.data = payload;

    /* Icon from the data dir; iconless if it can't be loaded (e.g. dcload).
     * vmu_pkg_load_icon does NOT allocate — icon_cnt and icon_data (512 bytes
     * per 32x32 4bpp frame) must be set first. It fills icon_data + icon_pal
     * and clamps icon_cnt to the .ico's frame count. */
    unsigned char icon_buf[512];
    pkg.icon_cnt = 1;
    pkg.icon_data = icon_buf;
    pkg.icon_anim_speed = 0;
    if (vmu_pkg_load_icon(&pkg, ICON_PATH) != 0) {
        pkg.icon_cnt = 0;
        pkg.icon_data = NULL;
    }

    *out = NULL; *outsz = 0;
    int built = vmu_pkg_build(&pkg, out, outsz);
    free(payload);                 /* vmu_pkg_build copied pkg.data into out */
    if (built < 0 || !*out || *outsz <= 0) {
        if (*out) { free(*out); *out = NULL; }
        return -1;
    }
    return 0;
}

/* Commit a prebuilt image to the card. Returns 0 on success, -1 on failure. */
static int vmu_commit(const char *fn, const unsigned char *out, int outsz)
{
    maple_device_t *d = vmu_dev();
    if (!d) return -1;

    char path[40];
    vmu_path(d, fn, path, sizeof(path));

    PauseCD();   /* stop the music stream while the maple bus is busy */
    file_t f = fs_open(path, O_RDWR | O_CREAT | O_META);
    if (f == FILEHND_INVALID) { ResumeCD(); return -1; }

    ssize_t total = 0, w;
    int rv = 0;
    while (total < outsz) {
        w = fs_write(f, out + total, outsz - total);
        if (w < 0) { rv = -1; break; }   /* card full / write error */
        total += w;
    }
    fs_close(f);
    ResumeCD();   /* maple I/O done — resume music */
    return rv;
}

/* Wrap payload in a vmu_pkg (with the extracted SONICR icon) and write it.
 * Returns 0 on success, -1 on failure. */
static int vmu_write(const char *fn, const char *desc,
                     const unsigned char *data, int len)
{
    unsigned char *out;
    int outsz;
    if (vmu_build(desc, data, len, &out, &outsz) < 0) return -1;
    int rv = vmu_commit(fn, out, outsz);
    free(out);
    return rv;
}

/* Pull the 10-slot bundle from the card once per session. */
static void bundle_ensure(void)
{
    if (s_bundleLoaded) return;
    s_bundleLoaded = 1;
    memset(s_bundle, 0, sizeof(s_bundle));
    memset(s_slotUsed, 0, sizeof(s_slotUsed));

    unsigned char tmp[BUNDLE_PAYLOAD];
    int len = vmu_read(VMU_BUNDLE_FN, tmp, sizeof(tmp));
    if (len < SLOT_COUNT) return;                 /* absent / too small */

    memcpy(s_slotUsed, tmp, SLOT_COUNT);
    int avail = (len - SLOT_COUNT) / SLOT_SIZE;
    if (avail > SLOT_COUNT) avail = SLOT_COUNT;
    memcpy(s_bundle, tmp + SLOT_COUNT, (size_t)avail * SLOT_SIZE);
}

static int bundle_flush(void)
{
    unsigned char tmp[BUNDLE_PAYLOAD];
    memcpy(tmp, s_slotUsed, SLOT_COUNT);
    memcpy(tmp + SLOT_COUNT, s_bundle, sizeof(s_bundle));
    return vmu_write(VMU_BUNDLE_FN, "Sonic R Save Data", tmp, BUNDLE_PAYLOAD);
}

/* =====================================================================
 * Capacity gating — decided once, at first VMU access.
 * ===================================================================== */

static int vmu_free_blocks_now(maple_device_t *d)
{
    PauseCD();
    int fb = vmufs_free_blocks(d);
    ResumeCD();
    return fb;
}

static int vmu_file_exists(const char *fn)
{
    maple_device_t *d = vmu_dev();
    if (!d) return 0;
    char path[40];
    vmu_path(d, fn, path, sizeof(path));
    PauseCD();
    file_t f = fs_open(path, O_RDONLY | O_META);
    int ok = (f != FILEHND_INVALID);
    if (ok) fs_close(f);
    ResumeCD();
    return ok;
}

/* Two gates: (1) fresh card with no room for the core saves -> NO_SAVE, all VMU
 * I/O fails. (2) core present but no ghost file and no room for the ghost
 * budget -> RAM_GHOSTS, only ghost I/O diverts to RAM. */
static void capacity_ensure(void)
{
    if (s_vmuChecked) return;
    s_vmuChecked = 1;

    maple_device_t *d = vmu_dev();
    if (!d) { s_vmuMode = VMU_NO_CARD; return; }   /* no card inserted at all */

    int freeb = vmu_free_blocks_now(d);
    int haveCore = vmu_file_exists(VMU_BUNDLE_FN) || vmu_file_exists(VMU_OPT_FN)
                || vmu_file_exists(VMU_PAD_FN)   || vmu_file_exists(VMU_NET_FN)
                || vmu_file_exists(VMU_KEY_FN);

    if (!haveCore) {                                /* fresh card */
        if (freeb < CORE_RESERVE_BLOCKS) { s_vmuMode = VMU_NO_SAVE; return; }
        freeb -= CORE_RESERVE_BLOCKS;               /* core consumes this on first save */
    }
    if (!vmu_file_exists(VMU_GHO_FN) && freeb < GHOST_RESERVE_BLOCKS) {
        s_vmuMode = VMU_RAM_GHOSTS; return;
    }
    s_vmuMode = VMU_NORMAL;
}

/* Front-end query: 0 = normal, 1 = no save at all, 2 = ghosts are RAM-only.
 * Used to raise the "free up VMU space" message once at startup. */
int SaveVMU_CapacityMode(void)
{
    mutex_lock(&io_lock);
    capacity_ensure();
    int m = s_vmuMode;
    mutex_unlock(&io_lock);
    return m;
}

/* Boot warning text for the current capacity mode, or NULL if all is well.
 * The block counts come straight from the reserve constants so they can't
 * drift from what the gates actually require. The front-end (platform_dc)
 * draws this once at startup. */
const char *SaveVMU_CapacityWarning(void)
{
    static char msg[256];
    switch (SaveVMU_CapacityMode()) {
        case VMU_NO_SAVE:
            snprintf(msg, sizeof(msg),
                     "Not enough free blocks.\n"
                     "To save your game, you need a VMU\n"
                     "with %d free blocks.\n"
                     "Press START to continue.", CORE_RESERVE_BLOCKS);
            return msg;
        case VMU_RAM_GHOSTS:
            snprintf(msg, sizeof(msg),
                     "Not enough free blocks.\n"
                     "To save ghost data, you need a VMU\n"
                     "with %d free blocks.\n"
                     "Press START to continue.", GHOST_RESERVE_BLOCKS);
            return msg;
        case VMU_NO_CARD:
            return "No VMU inserted.\n"
                   "Game cannot be saved.\n"
                   "Press START to continue.";
        default:
            return NULL;
    }
}

/* =====================================================================
 * Ghost bundle — 10 variable-length slots in SONICR_GHO.
 * ===================================================================== */

/* Pull SONICR_GHO into the RAM slots once per session (NORMAL mode only). */
static void ghost_ensure(void)
{
    if (s_ghostLoaded) return;
    s_ghostLoaded = 1;
    if (s_vmuMode != VMU_NORMAL) return;           /* RAM-only: never read card */

    unsigned char *raw = malloc(GHOST_BUNDLE_MAX);
    if (!raw) return;
    int len = vmu_read(VMU_GHO_FN, raw, GHOST_BUNDLE_MAX);
    if (len >= (int)(8 + GHOST_SLOTS * 8)) {
        unsigned int magic = 0, nextSeq = 0;
        memcpy(&magic,   raw + 0, 4);
        memcpy(&nextSeq, raw + 4, 4);
        if (magic == GHOST_MAGIC) {
            unsigned int off = 8 + GHOST_SLOTS * 8;
            for (int i = 0; i < GHOST_SLOTS; i++) {
                unsigned int l = 0, sq = 0;
                memcpy(&l,  raw + 8 + i * 8 + 0, 4);
                memcpy(&sq, raw + 8 + i * 8 + 4, 4);
                if (l > 0 && l <= MAX_GHOST_BYTES && off + l <= (unsigned int)len) {
                    s_ghost[i] = malloc(l);
                    if (s_ghost[i]) {
                        memcpy(s_ghost[i], raw + off, l);
                        s_ghostLen[i] = l;
                        s_ghostSeq[i] = sq;
                    }
                    off += l;
                }
            }
            if (nextSeq > s_ghostNextSeq) s_ghostNextSeq = nextSeq;
        }
    }
    free(raw);
}

/* Copy a just-written ghost into its RAM slot and stamp it newest. */
static void ghost_slot_put(int slot, const unsigned char *buf, unsigned int len)
{
    if (slot < 0 || slot >= GHOST_SLOTS) return;
    if (len > MAX_GHOST_BYTES) len = MAX_GHOST_BYTES;
    unsigned char *nb = realloc(s_ghost[slot], len ? len : 1);
    if (!nb) return;                               /* keep old slot on OOM */
    memcpy(nb, buf, len);
    s_ghost[slot] = nb;
    s_ghostLen[slot] = len;
    s_ghostSeq[slot] = s_ghostNextSeq++;
}

/* Serialize the RAM bundle: [u32 magic][u32 nextSeq][ (u32 len,u32 seq) x10 ]
 * then the used bodies in slot order. */
static int ghost_serialize(unsigned char **out, int *outlen)
{
    int total = 8 + GHOST_SLOTS * 8;
    for (int i = 0; i < GHOST_SLOTS; i++)
        if (s_ghost[i]) total += (int)s_ghostLen[i];

    unsigned char *b = malloc(total);
    if (!b) return -1;

    unsigned int magic = GHOST_MAGIC, nextSeq = s_ghostNextSeq;
    memcpy(b + 0, &magic,   4);
    memcpy(b + 4, &nextSeq, 4);
    unsigned int off = 8 + GHOST_SLOTS * 8;
    for (int i = 0; i < GHOST_SLOTS; i++) {
        unsigned int l  = s_ghost[i] ? s_ghostLen[i] : 0;
        unsigned int sq = s_ghostSeq[i];
        memcpy(b + 8 + i * 8 + 0, &l,  4);
        memcpy(b + 8 + i * 8 + 4, &sq, 4);
        if (l) { memcpy(b + off, s_ghost[i], l); off += l; }
    }
    *out = b; *outlen = total;
    return 0;
}

/* Pack the bundle and evict the oldest slot until it fits GHOST_RESERVE_BLOCKS,
 * then commit. A single ghost is far under budget, so this terminates. */
static int ghost_flush(void)
{
    for (;;) {
        unsigned char *raw; int rawlen;
        if (ghost_serialize(&raw, &rawlen) < 0) return -1;

        unsigned char *img; int imgsz;
        int r = vmu_build("Sonic R Ghosts", raw, rawlen, &img, &imgsz);
        free(raw);
        if (r < 0) return -1;

        int blocks = (imgsz + VMU_BLOCK - 1) / VMU_BLOCK;
        if (blocks <= GHOST_RESERVE_BLOCKS) {
            r = vmu_commit(VMU_GHO_FN, img, imgsz);
            free(img);
            return r;
        }
        free(img);

        int victim = -1;
        unsigned int lo = 0xFFFFFFFFu;
        for (int i = 0; i < GHOST_SLOTS; i++)
            if (s_ghost[i] && s_ghostSeq[i] < lo) { lo = s_ghostSeq[i]; victim = i; }
        if (victim < 0) return -1;                 /* nothing left to evict */
        free(s_ghost[victim]);
        s_ghost[victim] = NULL;
        s_ghostLen[victim] = 0;
        s_ghostSeq[victim] = 0;
    }
}

/* =====================================================================
 * Path classification
 * ===================================================================== */

/* Returns kind (K_*), sets *slot for K_BUNDLE; -1 if not a save path. */
static int classify(const char *path, int *slot)
{
    if (strstr(path, "SONICR.INF")) return K_OPT;
    if (strstr(path, "JOYSTICK.INF")) return K_PAD;
    if (strstr(path, "ONLINE.DAT")) return K_ONLINE;
    if (strstr(path, "KEYS.BIN")) return K_KEYS;

    /* GHOST/G<NN>.GHO — the DC-collapsed ghost slot (ghost.c BuildGhostPath).
     * Parse the 1-2 digits between 'G' and ".GHO" into the slot index. */
    {
        const char *g = strstr(path, ".GHO");
        if (!g) g = strstr(path, ".gho");
        if (g && g - path >= 2) {
            int n = 0, mul = 1, digits = 0;
            const char *p = g - 1;
            while (p > path && *p >= '0' && *p <= '9' && digits < 2) {
                n += (*p - '0') * mul; mul *= 10; digits++; p--;
            }
            if (digits >= 1 && (*p == 'G' || *p == 'g')
                && n >= 0 && n < GHOST_SLOTS) { *slot = n; return K_GHOST; }
        }
    }

    /* SAVE/R<NN>.SAV — pull the two digits after the last 'R' before ".SAV" */
    const char *sav = strstr(path, ".SAV");
    if (sav && sav - path >= 3) {
        const char *p = sav - 1;
        int n = 0, mul = 1, digits = 0;
        while (p > path && *p >= '0' && *p <= '9') {
            n += (*p - '0') * mul; mul *= 10; digits++; p--;
        }
        if (digits >= 1 && *p == 'R') {
            int s = n - 1;                         /* R01 -> 0 */
            if (s >= 0 && s < SLOT_COUNT) { *slot = s; return K_BUNDLE; }
        }
    }
    return -1;
}

/* =====================================================================
 * Virtual-handle registry
 * ===================================================================== */

static int reg_add(sr_vfile *v)
{
    for (int i = 0; i < MAX_VFILE; i++)
        if (!s_open[i]) { s_open[i] = v; return 0; }
    return -1;
}

static int is_vfile(FILE *f)
{
    for (int i = 0; i < MAX_VFILE; i++)
        if (s_open[i] == (sr_vfile *)f) return 1;
    return 0;
}

static void reg_del(sr_vfile *v)
{
    for (int i = 0; i < MAX_VFILE; i++)
        if (s_open[i] == v) { s_open[i] = NULL; return; }
}

/* =====================================================================
 * Real-fopen fallback (the original fileio.h DC macro behaviour:
 * try the path, then retry under /pc for dcload).
 * ===================================================================== */

static FILE *real_open(const char *path, const char *mode)
{
    FILE *fp = fopen(path, mode);
    if (fp) return fp;

    char pcPath[256];
    int r;
    if (strlen(path) > 3 && path[1] == 'c' && path[2] == 'd') {
        r = snprintf(pcPath, sizeof(pcPath), "%s", path);
        if (r < 0 || (size_t)r >= sizeof(pcPath)) return NULL;
        pcPath[1] = 'p'; pcPath[2] = 'c';
    } else if (strlen(path) > 3 && path[1] == 'p' && path[2] == 'c') {
        return NULL;
    } else {
        r = snprintf(pcPath, sizeof(pcPath), "/pc/%s", path);
        if (r < 0 || (size_t)r >= sizeof(pcPath)) return NULL;
    }
    return fopen(pcPath, mode);
}

/* =====================================================================
 * The fileio shim — fOpen/fRead/fWrite/fClose/fSeek/fTell/fError on DC.
 * ===================================================================== */

FILE *sr_fOpen(const char *path, const char *mode)
{
    int slot = 0;
    int kind = classify(path, &slot);

    mutex_lock(&io_lock);

    if (kind < 0) {                                /* game data — real fopen */
        FILE *fp = real_open(path, mode);
        mutex_unlock(&io_lock);
        return fp;
    }

    capacity_ensure();
    if (s_vmuMode == VMU_NO_SAVE || s_vmuMode == VMU_NO_CARD) {  /* nothing persists */
        mutex_unlock(&io_lock);
        return NULL;                                /* every VMU read+write fails */
    }

    int writing = (mode[0] == 'w' || mode[0] == 'a' ||
                   (mode[0] && mode[1] == '+'));

    sr_vfile *v = calloc(1, sizeof(*v));
    if (!v) { mutex_unlock(&io_lock); return NULL; }
    v->kind = kind;
    v->slot = slot;
    v->writing = writing;

    if (kind == K_BUNDLE) {
        bundle_ensure();
        if (writing) {
            v->cap = SLOT_SIZE;
            v->buf = calloc(1, SLOT_SIZE);
        } else {
            if (!s_slotUsed[slot]) {               /* empty slot -> "no file" */
                free(v); mutex_unlock(&io_lock); return NULL;
            }
            v->buf = s_bundle[slot];               /* read in place */
            v->cap = v->len = SLOT_SIZE;
        }
    } else if (kind == K_GHOST) {
        ghost_ensure();                            /* RAM-backed in every mode */
        if (writing) {
            v->cap = MAX_GHOST_BYTES;
            v->buf = calloc(1, v->cap);
        } else {
            if (!s_ghost[slot]) {                  /* no ghost recorded yet */
                free(v); mutex_unlock(&io_lock); return NULL;
            }
            v->buf = malloc(s_ghostLen[slot]);
            if (v->buf) {
                memcpy(v->buf, s_ghost[slot], s_ghostLen[slot]);
                v->cap = v->len = s_ghostLen[slot];
            }
        }
    } else {
        const char *fn = kind_fn(kind);
        if (writing) {
            v->cap = 2048;                         /* INF files are tiny */
            v->buf = calloc(1, v->cap);
        } else {
            unsigned char *b = malloc(2048);
            int len = b ? vmu_read(fn, b, 2048) : -1;
            if (len < 0) { free(b); free(v); mutex_unlock(&io_lock); return NULL; }
            v->buf = b; v->cap = v->len = len;
        }
    }

    if (!v->buf || reg_add(v) < 0) {
        if (v->buf && v->buf != s_bundle[slot]) free(v->buf);
        free(v);
        mutex_unlock(&io_lock);
        return NULL;
    }
    mutex_unlock(&io_lock);
    return (FILE *)v;
}

size_t sr_fRead(void *buf, size_t sz, size_t n, FILE *f)
{
    mutex_lock(&io_lock);
    if (!is_vfile(f)) { size_t r = fread(buf, sz, n, f); mutex_unlock(&io_lock); return r; }
    sr_vfile *v = (sr_vfile *)f;
    size_t want = sz * n;
    size_t avail = (v->pos < v->len) ? v->len - v->pos : 0;
    if (want > avail) want = avail;
    memcpy(buf, v->buf + v->pos, want);
    v->pos += want;
    mutex_unlock(&io_lock);
    return sz ? want / sz : 0;
}

size_t sr_fWrite(const void *buf, size_t sz, size_t n, FILE *f)
{
    mutex_lock(&io_lock);
    if (!is_vfile(f)) { size_t r = fwrite(buf, sz, n, f); mutex_unlock(&io_lock); return r; }
    sr_vfile *v = (sr_vfile *)f;
    size_t want = sz * n;
    if (v->pos + want > v->cap) {                  /* grow (INF only) */
        size_t ncap = v->pos + want;
        unsigned char *nb = realloc(v->buf, ncap);
        if (!nb) { mutex_unlock(&io_lock); return 0; }
        v->buf = nb; v->cap = ncap;
    }
    memcpy(v->buf + v->pos, buf, want);
    v->pos += want;
    if (v->pos > v->len) v->len = v->pos;
    mutex_unlock(&io_lock);
    return n;
}

int sr_fClose(FILE *f)
{
    mutex_lock(&io_lock);
    if (!is_vfile(f)) { int r = fclose(f); mutex_unlock(&io_lock); return r; }
    sr_vfile *v = (sr_vfile *)f;

    if (v->writing) {
        if (v->kind == K_BUNDLE) {
            memcpy(s_bundle[v->slot], v->buf, SLOT_SIZE);
            s_slotUsed[v->slot] = 1;
            bundle_flush();
        } else if (v->kind == K_GHOST) {
            ghost_slot_put(v->slot, v->buf, v->len);
            if (s_vmuMode == VMU_NORMAL) {
                /* Our 40-block budget bounds only our own use; the card can
                 * still be globally full. On write failure, keep the RAM ghost
                 * and drop to RAM-only for the rest of the session. */
                if (ghost_flush() < 0) s_vmuMode = VMU_RAM_GHOSTS;
            }
        } else {
            vmu_write(kind_fn(v->kind), kind_desc(v->kind), v->buf, v->len);
        }
    }

    reg_del(v);
    if (v->buf && v->buf != s_bundle[v->slot]) free(v->buf);
    free(v);
    mutex_unlock(&io_lock);
    return 0;
}

int sr_fSeek(FILE *f, long off, int whence)
{
    mutex_lock(&io_lock);
    if (!is_vfile(f)) { int r = fseek(f, off, whence); mutex_unlock(&io_lock); return r; }
    sr_vfile *v = (sr_vfile *)f;
    long base = (whence == SEEK_CUR) ? (long)v->pos
              : (whence == SEEK_END) ? (long)v->len : 0;
    long np = base + off;
    if (np < 0) np = 0;
    v->pos = (size_t)np;
    mutex_unlock(&io_lock);
    return 0;
}

long sr_fTell(FILE *f)
{
    mutex_lock(&io_lock);
    if (!is_vfile(f)) { long r = ftell(f); mutex_unlock(&io_lock); return r; }
    long p = (long)((sr_vfile *)f)->pos;
    mutex_unlock(&io_lock);
    return p;
}

int sr_fError(FILE *f)
{
    mutex_lock(&io_lock);
    if (!is_vfile(f)) { int r = ferror(f); mutex_unlock(&io_lock); return r; }
    mutex_unlock(&io_lock);
    return 0;
}
