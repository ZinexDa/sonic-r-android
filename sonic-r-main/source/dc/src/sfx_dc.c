/**
 * sfx_dc.c — Sound-effect playback via KOS snd_sfx.
 *
 * DC counterpart to sdl/src/sound/sfx_sdl.c. Uses the same slot table and
 * dispatch logic; only the load/play/stop primitives change to KOS calls.
 *
 * Looping sounds (engine, water) get a dedicated allocated channel via
 * snd_sfx_chn_alloc + snd_sfx_play_ex(loop=1); one-shots use snd_sfx_play.
 * A running loop's volume and pitch are updated in place on its AICA channel
 * (sfx_update_vol / sfx_update_freq) without restarting the sample, so e.g.
 * Amy's speed-driven surface pitch tracks smoothly instead of retriggering.
 */

#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/aica_comm.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "fileio.h"

#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "sonicr_paths.h"
#include "sndwav.h"

extern void *g_soundBuffers[64];    /* 0x006D9AEC */
extern int   g_soundActive[64];    /* 0x006DA080 */
extern int   g_initFeatureB;       /* 0x006D9AE8 */
extern int   g_volumeBase;         /* 0x005041A8 = -2500 */

#define SFX_MAX_SLOTS 64

/* The replay commentary plays at full volume: PlaySoundSimple (0x4d02d4) sets
 * no volume, and SetAllSoundVolumes skips the announcer slot during
 * demo/replay (0x4d07a6), so it stays at max and cuts through the music. The
 * clip is split across consecutive slots from REPLAY_VOICE_SLOT_FIRST so each
 * piece fits the 65534-frame sample cap, and the exemption covers the whole
 * range or the line would change level partway through.
 * See sound/replay_voice.c. */
#include "sound/replay_voice.h"

static sfxhnd_t s_handles[SFX_MAX_SLOTS];
static int      s_channel[SFX_MAX_SLOTS];   /* persistent per-slot channel (snd_sfx_chn_alloc'd at load) */
static int      s_isLooping[SFX_MAX_SLOTS]; /* 1 if currently looping on s_channel[slot] */
static int      s_loopVol[SFX_MAX_SLOTS];   /* current loop volume — re-trigger only if it changes */
static int      s_loopFreq[SFX_MAX_SLOTS];  /* last freq applied to the live loop; -1 = unset */
static int      s_durationMs[SFX_MAX_SLOTS];/* clip length from the WAV header, ms (music duck) */
static int      s_ready;

/* Slot-to-filename mapping — extracted from FUN_004d07d4 in SONICR.EXE
 * (same table as sfx_sdl.c). */
static const struct { int slot; const char *filename; } s_sfxTable[] = {
    { 0x00, "PAUSE.WAV"    },
    { 0x01, "CHOOSE.WAV"   },
    { 0x02, "SELECT.WAV"   },
    { 0x03, "RUNLEFT.WAV"  },
    { 0x04, "RUNRIGHT.WAV" },
    { 0x05, "AMY.WAV"      },
    { 0x06, "JET.WAV"      },
    { 0x07, "JUMP.WAV"     },
    { 0x08, "SPIN.WAV"     },
    { 0x09, "SPINGO.WAV"   },
    { 0x0A, "SPINREV.WAV"  },
    { 0x0B, "TAILS.WAV"    },
    { 0x0D, "JUMP.WAV"     },
    { 0x0E, "FIRE.WAV"     },
    { 0x0F, "EXPLODE.WAV"  },
    { 0x10, "AMYSKID.WAV"  },
    { 0x11, "AMYWATER.WAV" },
    { 0x12, "WATERRUN.WAV" },
    { 0x13, "WATERRUN.WAV" },
    { 0x14, "BUBBLE.WAV"   },
    { 0x15, "SPLASH.WAV"   },
    { 0x16, "POP.WAV"      },
    { 0x18, "HITCHAR.WAV"  },
    { 0x1A, "BONUS.WAV"    },
    { 0x1B, "GETTOKEN.WAV" },
    { 0x1C, "GETCHAOS.WAV" },
    { 0x1D, "RING1.WAV"    },
    { 0x1E, "RING1.WAV"    },
    { 0x1F, "WARP.WAV"     },
    { 0x20, "SKID1.WAV"    },
    { 0x21, "DOOR.WAV"     },
    { 0x22, "RECORD.WAV"   },
    { 0x23, "GOTALL.WAV"   },
    { 0x24, "BONUS.WAV"    },
    { 0x27, "TAG.WAV"      },
    { 0x2D, "THUNDER.WAV"  },
    { 0x32, "SPRING.WAV"   },
    { 0x33, "BUMPER1.WAV"  },
    { 0x34, "BUMPER2.WAV"  },
    { 0x35, "READY.WAV"    },
    { 0x36, "SET.WAV"      },
    { 0x37, "GO.WAV"       },
    { -1,   NULL           }
};

/* g_masterVolume → linear 0..1.
 *
 * SetAllSoundVolumes produces g_volumeBase(-2500)..0, one step per SFX volume
 * setting, so normalising across that span gives an even 1/8 per step. */
static float ds_volume_to_linear(int dsVolume)
{
    if (dsVolume <= g_volumeBase) return 0.0f;
    if (dsVolume >= 0)            return 1.0f;
    return (float)(dsVolume - g_volumeBase) / (float)(-g_volumeBase);
}

/* Load a WAV file from disk, fix the 8-bit unsigned→signed mismatch on
 * the way to AICA, and hand the buffer to snd_sfx_load_buf.
 *
 * Why: WAV 8-bit PCM is unsigned (silence = 0x80). AICA's SM_8BIT format
 * expects signed (silence = 0x00). KOS's snd_sfx_load blits the raw bytes
 * to AICA RAM unchanged, which makes any 8-bit WAV play with massive DC
 * offset and inverted polarity — sounds like loud distortion. We XOR 0x80
 * across the sample data before AICA upload, which converts unsigned →
 * signed in-place. 16-bit and ADPCM WAVs pass through untouched. */
static char *read_wav_file(const char *path, long *out_size)
{
    FILE *fp = fOpen(path, "rb");
    if (!fp) return NULL;
    fSeek(fp, 0, SEEK_END);
    long sz = fTell(fp);
    fSeek(fp, 0, SEEK_SET);
    if (sz <= 44) { fClose(fp); return NULL; }
    char *buf = (char *)malloc(sz);
    if (!buf) { fClose(fp); return NULL; }
    if (fRead(buf, 1, sz, fp) != (size_t)sz) {
        free(buf); fClose(fp); return NULL;
    }
    fClose(fp);
    *out_size = sz;
    return buf;
}

/* Walk the chunk list to find fmt and data spans. Returns 1 on success. */
static int find_wav_chunks(const char *buf, long size,
                           int *out_bits, long *out_data_off, long *out_data_len,
                           int *out_fmt, int *out_channels, int *out_rate)
{
    if (size < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
        return 0;

    long off = 12;
    int  haveFmt = 0;
    int  haveData = 0;
    while (off + 8 <= size) {
        const char *id = buf + off;
        uint32_t csz;
        memcpy(&csz, buf + off + 4, 4);
        long body = off + 8;

        if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= size) {
            uint16_t bits, tag, ch;
            uint32_t rate;
            memcpy(&tag,  buf + body + 0,  2);   /* format tag */
            memcpy(&ch,   buf + body + 2,  2);   /* channels */
            memcpy(&rate, buf + body + 4,  4);   /* sample rate */
            memcpy(&bits, buf + body + 14, 2);   /* offset 14 in fmt = bits/sample */
            *out_bits = bits;
            if (out_fmt)      *out_fmt = tag;
            if (out_channels) *out_channels = ch;
            if (out_rate)     *out_rate = (int)rate;
            haveFmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            *out_data_off = body;
            *out_data_len = (long)csz;
            haveData = 1;
        }
        if (haveFmt && haveData) return 1;
        off = body + ((csz + 1) & ~1u);  /* chunks word-aligned */
    }
    return haveFmt && haveData;
}

static void load_wav_into_slot(int slot, const char *filename)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) {
        dbglog(DBG_ERROR, "SFX: load slot %d out of range\n", slot);
        return;
    }

    /* Release whatever the slot already holds. snd_sfx_chn_alloc draws from a
     * fixed 64-channel pool with no recycling and the startup table claims 42,
     * so a slot reloaded once per replay would exhaust the pool and strand the
     * sample in AICA memory. Stop first: freeing a channel only clears the
     * in-use bit, it does not silence a voice still sounding on it. */
    if (s_handles[slot] != SFXHND_INVALID) {
        if (s_channel[slot] >= 0) {
            snd_sfx_stop(s_channel[slot]);
            snd_sfx_chn_free(s_channel[slot]);
            s_channel[slot] = -1;
        }
        snd_sfx_unload(s_handles[slot]);
        s_handles[slot]   = SFXHND_INVALID;
        s_isLooping[slot] = 0;
        s_loopVol[slot]   = -1;
        s_loopFreq[slot]  = -1;
        s_durationMs[slot] = 0;
        g_soundBuffers[slot] = NULL;
        g_soundActive[slot]  = 0;
    }

    char path[512];
    snprintf(path, sizeof(path), DATA_DIR "/SOUND/DCSFX/%s", filename);
    long size = 0;
    char *buf = read_wav_file(path, &size);
    if (!buf) {
        dbglog(DBG_ERROR, "SFX: open %s failed for slot 0x%02X\n", path, slot);
        return;
    }

    int  bits = 16;
    long dataOff = 0;
    long dataLen = 0;
    int  fmt = 1, channels = 1, rate = 0;
    if (find_wav_chunks(buf, size, &bits, &dataOff, &dataLen, &fmt, &channels, &rate)) {
        if (bits == 8) {
            unsigned char *p = (unsigned char *)buf + dataOff;
            for (long i = 0; i < dataLen; i++) p[i] ^= 0x80;
        }
        /* Clip length for the music duck. Yamaha ADPCM (fmt 0x14) packs two
         * 4-bit samples per byte; PCM is dataLen / (channels * bytes). */
        if (rate > 0 && channels > 0) {
            long samples = (fmt == 0x14 || fmt == 0x11)
                         ? (dataLen * 2) / channels
                         : dataLen / (channels * (bits / 8 ? bits / 8 : 1));
            s_durationMs[slot] = (int)((samples * 1000) / rate);
        }
    }

    sfxhnd_t h = snd_sfx_load_buf(buf);
    free(buf);
    if (h == SFXHND_INVALID) {
        dbglog(DBG_ERROR, "SFX: snd_sfx_load_buf(%s) failed for slot 0x%02X\n", path, slot);
        return;
    }
    int chn = snd_sfx_chn_alloc();
    if (chn < 0) {
        dbglog(DBG_ERROR, "SFX: snd_sfx_chn_alloc failed for slot 0x%02X (%s)\n", slot, path);
        snd_sfx_unload(h);
        return;
    }
    s_handles[slot]      = h;
    s_channel[slot]      = chn;
    g_soundBuffers[slot] = (void *)(intptr_t)1;
    g_soundActive[slot]  = 1;
    //dbglog(DBG_INFO, "SFX: loaded slot 0x%02X (%s) -> chn %d\n", slot, filename, chn);
}

void InitDirectSound(void)
{
    if (s_ready) {
        dbglog(DBG_INFO, "InitDirectSound: already initialized\n");
        return;
    }

    if (snd_init() < 0) {
        dbglog(DBG_ERROR, "InitDirectSound: snd_init failed\n");
        return;
    }
    dbglog(DBG_INFO, "InitDirectSound: snd_init OK\n");

    /* Init the music stream BEFORE allocating per-slot SFX channels.
     * snd_sfx_chn_alloc grabs from the same channel pool the streamer
     * uses, so streaming has to claim its channels first. */
    wav_init();

    for (int i = 0; i < SFX_MAX_SLOTS; i++) {
        s_handles[i]   = SFXHND_INVALID;
        s_channel[i]   = -1;
        s_isLooping[i] = 0;
        s_loopVol[i]   = -1;
        s_loopFreq[i]  = -1;
    }

    int loaded = 0;
    for (int i = 0; s_sfxTable[i].slot >= 0; i++) {
        load_wav_into_slot(s_sfxTable[i].slot, s_sfxTable[i].filename);
        if (s_handles[s_sfxTable[i].slot] != SFXHND_INVALID) loaded++;
    }
    //dbglog(DBG_INFO, "SFX: loaded %d effects\n", loaded);

    s_ready = 1;
    g_lpDirectSound = (void *)(intptr_t)1;
    g_initFeatureB  = 1;

    extern void SetAllSoundVolumes(void);   /* 0x4D0760 */
    SetAllSoundVolumes();
}

void CloseDirectSound(void)
{
    s_ready = 0;
    snd_sfx_stop_all();
    for (int i = 0; i < SFX_MAX_SLOTS; i++) {
        if (s_channel[i] >= 0) {
            snd_sfx_chn_free(s_channel[i]);
            s_channel[i] = -1;
        }
        if (s_handles[i] != SFXHND_INVALID) {
            snd_sfx_unload(s_handles[i]);
            s_handles[i] = SFXHND_INVALID;
        }
        g_soundBuffers[i] = NULL;
        g_soundActive[i]  = 0;
        s_isLooping[i]    = 0;
        s_loopVol[i]      = -1;
        s_loopFreq[i]     = -1;
    }
    g_lpDirectSound = NULL;
}

/* SFX_* — internal API the dispatchers use. KOS volumes are 0..255, pan
 * 0..255 (128 = center). */

/* Each slot owns one persistent KOS channel (snd_sfx_chn_alloc at load).
 * Playing always targets that channel via snd_sfx_play_chn / play_ex,
 * which auto-replaces whatever was playing on it — same semantic as
 * SDL_mixer's "channel = slot" so frequent triggers don't stack into
 * a wall of overlapping voices. */

extern int snd_sh4_to_aica(void *packet, uint32_t size);
struct snd_effect;
typedef struct snd_effect
{
	uint32_t locl, locr;
	uint32_t len;
	uint32_t rate;
	uint32_t used;
	uint32_t fmt;
	uint16_t stereo;

	LIST_ENTRY(snd_effect)
	list;
} snd_effect_t;

/* Center the AICA pitch quantization. KOS encodes pitch as OCT (4-bit octave)
 * + FNS (10-bit fractional mantissa) = 1024 steps/octave, and appears to
 * TRUNCATE the FNS, so a requested Hz realizes a hair LOW — Amy's engine reads
 * slightly flat on hardware vs the SDL resampler (which uses an exact ratio and
 * is our pitch reference). Nudge the request up ~half an FNS step so the floor
 * rounds to nearest. Half a step is +0.5/1024 of the octave mantissa (1+FNS/1024
 * in [1,2)), i.e. a +0.024%..+0.049% multiplicative bump depending on octave
 * position; the ~+0.037% midpoint is a reasonable single value.
 * TUNE against hardware (A/B vs the SDL pitch); 1.0 disables. */
#ifndef AICA_FNS_HALF_STEP
#define AICA_FNS_HALF_STEP  1.000366f
#endif
static int aica_center_freq(int freq)
{
    if (freq <= 0) return freq;
    return (int)((float)freq * AICA_FNS_HALF_STEP + 0.5f);
}

/* Live volume update on an already-playing channel — no restart.
 * Derived from snd_sfx_play_ex; only AICA_CH_UPDATE_SET_VOL is applied, so
 * only data->vol takes effect (freq/pan fields are ignored by the driver). */
static void sfx_update_vol(sfx_play_data_t *data)
{
    int size;
    snd_effect_t *t = (snd_effect_t *)data->idx;
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    size = t->len;
    if (size >= 65535)
        size = 65534;

    cmd->cmd       = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size      = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id    = data->chn;

    chan->cmd       = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_VOL;
    chan->base      = t->locl;
    chan->type      = t->fmt;
    chan->length    = size;
    chan->loop      = data->loop;
    chan->loopstart = data->loopstart;
    chan->loopend   = data->loopend ? data->loopend : size;
    chan->freq      = data->freq > 0 ? aica_center_freq(data->freq) : t->rate;
    chan->vol       = data->vol;
    chan->pan       = data->pan;

    snd_sh4_to_aica(tmp, cmd->size);
}

/* Live frequency (pitch) update on an already-playing channel — no restart.
 * Only AICA_CH_UPDATE_SET_FREQ is applied, so only data->freq takes effect. */
static void sfx_update_freq(sfx_play_data_t *data)
{
    int size;
    snd_effect_t *t = (snd_effect_t *)data->idx;
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);

    size = t->len;
    if (size >= 65535)
        size = 65534;

    cmd->cmd       = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size      = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id    = data->chn;

    chan->cmd       = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_FREQ;
    chan->base      = t->locl;
    chan->type      = t->fmt;
    chan->length    = size;
    chan->loop      = data->loop;
    chan->loopstart = data->loopstart;
    chan->loopend   = data->loopend ? data->loopend : size;
    chan->freq      = data->freq > 0 ? aica_center_freq(data->freq) : t->rate;
    chan->vol       = data->vol;
    chan->pan       = data->pan;

    snd_sh4_to_aica(tmp, cmd->size);
}

static void start_loop(int slot, int vol, int freq)
{
    sfx_play_data_t data = {0};
    data.chn       = s_channel[slot];
    data.idx       = s_handles[slot];
    data.vol       = vol;
    data.pan       = 128;
    if (freq)
        data.freq = aica_center_freq(freq);
    data.loop      = 1;
    data.loopstart = 0;
    snd_sfx_play_ex(&data);
    s_isLooping[slot] = 1;
    s_loopVol[slot]   = vol;
    s_loopFreq[slot]  = freq;
}

void SFX_Play(int slot, int loop, int freq)
{
    if (!s_ready)                          return;
    if (slot < 0 || slot >= SFX_MAX_SLOTS) return;
    if (s_handles[slot] == SFXHND_INVALID) return;
    if (s_channel[slot] < 0)               return;

    int vol = (int)(ds_volume_to_linear(g_masterVolume) * 255.0f);
    if (vol < 0) vol = 0; if (vol > 255) vol = 255;

    if (loop) {
        /* Start the loop once. On every subsequent per-tick call, update the
         * LIVE voice in place — never stop/replay, or a continuously-varying
         * pitch (e.g. Amy's speed-driven surface sound) machine-guns the
         * sample from sample 0 each frame. This mirrors the original calling
         * SetFrequency/SetVolume on the persistent DirectSound buffer. */
        if (!s_isLooping[slot]) {
            snd_sfx_stop(s_channel[slot]);
            start_loop(slot, vol, freq);
            return;
        }
        /* Pitch only — volume is owned by SFX_SetVolume (called right before
         * this each frame with the distance-attenuated value); writing vol
         * here would clobber that attenuation with the master volume. */
        if (freq && freq != s_loopFreq[slot]) {
            sfx_play_data_t data = {0};
            data.chn       = s_channel[slot];
            data.idx       = s_handles[slot];
            data.freq      = freq;
            data.vol       = s_loopVol[slot];
            data.pan       = 128;
            data.loop      = 1;
            data.loopstart = 0;
            sfx_update_freq(&data);
            s_loopFreq[slot] = freq;
        }
        return;
    }
    else {
        /* One-shot — replaces any previous play on this channel. */
        int playVol = IS_REPLAY_VOICE_SLOT(slot) ? 255 : vol;  /* binary exempts the announcer slot from attenuation (0x4d07a6) */
        snd_sfx_stop(s_channel[slot]);
        s_isLooping[slot] = 0;
        s_loopVol[slot]   = -1;
        s_loopFreq[slot]  = -1;
        sfx_play_data_t data = {0};
        data.chn = s_channel[slot];
        data.idx = s_handles[slot];
        data.vol = playVol;
        data.pan = 128;
        if (freq)
            data.freq = freq;
        if (slot == 0xD)
            data.freq = 22050 + 5512;
        if (data.freq)
            data.freq = aica_center_freq(data.freq);
        snd_sfx_play_ex(&data);
    }
}

void SFX_Stop(int slot)
{
    if (!s_ready)
        return;
    if (slot < 0 || slot >= SFX_MAX_SLOTS)
        return;
    if (s_channel[slot] < 0)
        return;
    snd_sfx_stop(s_channel[slot]);
    s_isLooping[slot] = 0;
    s_loopVol[slot] = -1;
    s_loopFreq[slot] = -1;
}

void SFX_StopAll(void)
{
    if (!s_ready)
        return;
    snd_sfx_stop_all();
    for (int i = 0; i < SFX_MAX_SLOTS; i++)
    {
        s_isLooping[i] = 0;
        s_loopVol[i] = -1;
        s_loopFreq[i] = -1;
    }
}

/* Per-slot mix trim, 256 = unity. DELIBERATE DIVERGENCE from the 1998 mix.
 *
 * The aggregate loops play as PlaySoundEffect(0x1000B, minDist, pitch), and
 * 0x4D0396 clamps any distance below 0x40 straight to 0xFF — so a loop that
 * belongs to the player themselves runs at full SFX volume for the entire
 * race with only its pitch moving. Faithful, and fatiguing on slot 0x0B.
 *
 * Applied to the linear gain rather than the DirectSound value: the latter is
 * an attenuation across g_volumeBase..0, and scaling it would bend the curve
 * instead of the level. Slots not listed stay at unity.
 *
 * Only reaches SFX_SetVolume, which owns the volume of looping voices — the
 * case this exists for. Plain one-shots take their gain from SFX_Play and are
 * NOT trimmed; adding an entry for one would silently do nothing. */
#define SFX_TRIM_UNITY 256
static const short s_sfxSlotTrim[SFX_MAX_SLOTS] = {
    [0x0B] = 154,   /* TAILS.WAV — rotor loop, 60% */
};

static float sfx_slot_trim(int slot)
{
    int t = s_sfxSlotTrim[slot];
    if (t <= 0) {
        return 1.0f;
    }
    return (float)t / (float)SFX_TRIM_UNITY;
}

void SFX_SetVolume(int slot, int dsVolume)
{
    if (!s_ready)
        return;
    if (slot < 0 || slot >= SFX_MAX_SLOTS)
        return;
    if (s_handles[slot] == SFXHND_INVALID)
        return;
    if (s_channel[slot] < 0)
        return;

    int vol = (int)(ds_volume_to_linear(dsVolume) * 255.0f * sfx_slot_trim(slot));
    if (vol < 0)
        vol = 0;
    if (vol > 255)
        vol = 255;

    /* Update the live loop's volume in place — no restart. Only when the
     * volume actually changed (engine calls this per-tick). Restarting here
     * would machine-gun the sample as distance attenuation varies, and would
     * reset the pitch that SFX_Play maintains via s_loopFreq. */
    if (s_isLooping[slot] && s_loopVol[slot] != vol)
    {
        sfx_play_data_t data = {0};
        data.chn = s_channel[slot];
        data.idx = s_handles[slot];
        data.vol = vol;
        data.freq = s_loopFreq[slot] > 0 ? s_loopFreq[slot] : 0;
        data.pan = 128;
        data.loop = 1;
        data.loopstart = 0;
        sfx_update_vol(&data);
        s_loopVol[slot] = vol;
    }
}

void SFX_SetPan(int slot, int dsPan)   { (void)slot; (void)dsPan; }
void SFX_SetPosition(int slot, int p)  { (void)slot; (void)p; }

/* ---- Binary-named wrappers used by game code ---- */

static int PlaySoundSimple(int slot)                    /* 0x4d02d4 */
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS)
        return 0;
 
    if (g_soundActive[slot] == 0)
        return 0;

    if (g_optSfxVolume == 0)
        return 1;  /* sound disabled */

    SFX_Play(slot, 0, 0);
    return 1;
}

static int PlaySoundWithParams(int slot, int distance, int freqParam)
                                                        /* 0x4d0330 */
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS)
        return 0;
    if (g_soundActive[slot] == 0)
        return 0;
    if (g_optSfxVolume == 0)                return 1;  /* sound disabled */

    if (distance != 0x100) {
        int d = distance;
        if (d < 0x40) d = 0xFF;
        else d = 0xFF - d;

        int volDiff = g_masterVolume - g_volumeBase;
        if (volDiff < 0) volDiff = -volDiff;

        extern int g_demoMode;
        int divisor = (g_demoMode == 2) ? 0x12c : 0xff;

        int attenVol = (d * volDiff) / divisor + g_volumeBase;
        SFX_SetVolume(slot, attenVol);
    }

    if (freqParam != 0) {
        freqParam = (freqParam * 99900) / 255 + 100;
    }

    SFX_Play(slot, 1, freqParam);  /* matches SDL: loop after optional vol set */
    return 1;
}

static int g_ringAlternate;                             /* 0x00901CBC */

void PlaySoundEffect(int soundCmd, int distance, int freqParam)
{                                                       /* 0x482280 */
    int slot = soundCmd & 0xFFFF;

    if (soundCmd & 0xFFFF0000) {
        PlaySoundWithParams(slot, distance, freqParam);
    } else {
        PlaySoundSimple(slot);
    }

    extern int g_demoMode;
    if (g_demoMode == DEMO_REPLAY) return;

    if ((soundCmd & 0xFFFF) == 0x1D) {
        slot = 0x1D + g_ringAlternate;
        g_ringAlternate = (g_ringAlternate + 1) & 1;
    }
}

/* =====================================================================
 * LoadSoundEffect — 0x004D064C — LoadWAV(EAX=filename, EDX=slot)
 * Loads an arbitrary WAV file into the given slot at runtime. Delegates to
 * load_wav_into_slot, which prepends SOUND/DCSFX/. The AICA addresses samples
 * with a 16-bit offset, so snd_sfx tops out at 65534 samples; the replay voice
 * clips are 11025 Hz (REPLAY4 is 7350 Hz) and peak at 62870, so they fit.
 * ===================================================================== */
void LoadSoundEffect(const char *filename, int slot)
{
    load_wav_into_slot(slot, filename);
}

/* Clip length recorded from the WAV header at load time (see load_wav_into_slot). */
int SFX_ClipDurationMs(int slot)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) {
        return 0;
    }
    return s_durationMs[slot];
}
