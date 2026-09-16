/* sndadx.c — ADX music backend for the DC (opt-in, `make DC_MUSIC_ADX=1`).
 *
 * Drop-in sibling of sndwav.c: implements the exact same wav_* API from
 * sndwav.h, so sfx_dc.c (adx_wav_init) and music_dc.c call it unchanged. The
 * Makefile compiles EITHER sndwav.c (default) OR this file, never both.
 *
 * Difference from sndwav.c: .adp streams raw to snd_stream_start_adpcm and the
 * AICA decodes in hardware (zero CPU). ADX is not an AICA codec, so here the
 * SH4 decodes ADX -> 16-bit PCM (via the shared adx.c) in the stream callback
 * and feeds snd_stream_start (PCM mode). This trades SH4 cycles for the
 * convenience of one music format across SDL and DC — the whole point of the
 * experiment is to measure that cost against the .adp hardware path.
 *
 * The refill buffer is bumped 8192 -> 32768 because PCM is ~4x the byte rate of
 * the ADPCM payload; 32768 PCM bytes buffers the same ~186ms the .adp path did.
 */
#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <kos/thread.h>
#include <dc/sound/stream.h>

#include "sndwav.h"
#include "sound/adx.h"   /* shared decoder lives in sdl/src/sound (-I../sdl/src) */

#define STREAM_BUF_SIZE (32768)

/* Driver state */
#define SNDDRV_STATUS_NULL  0x00
#define SNDDRV_STATUS_READY 0x01
#define SNDDRV_STATUS_DONE  0x02

/* Decoder state */
#define SNDDEC_STATUS_NULL      0x00
#define SNDDEC_STATUS_READY     0x01
#define SNDDEC_STATUS_STREAMING 0x02
#define SNDDEC_STATUS_PAUSING   0x03
#define SNDDEC_STATUS_STOPPING  0x04
#define SNDDEC_STATUS_RESUMING  0x05

typedef struct {
    snd_stream_hnd_t shnd;
    AdxDecoder       dec;            /* ADX decode source (holds its own file) */
    int              dec_open;       /* 1 if dec currently holds an open file */
    uint8_t         *drv_buf;        /* 32-byte aligned PCM refill buffer */
    volatile int     status;
    uint32_t         loop;
    uint32_t         vol;            /* 0-255 */
    uint32_t         channels;       /* 1=Mono / 2=Stereo */
    uint32_t         sample_rate;
} snddrv_hnd;

/* shnd MUST be initialised explicitly — SND_STREAM_INVALID is -1, so a
 * zero-initialised handle is 0, a legal stream index. See sndwav.c. */
static snddrv_hnd      stream = { .shnd = SND_STREAM_INVALID };
static volatile int    sndwav_status = SNDDRV_STATUS_NULL;
static kthread_t      *audio_thread;
static kthread_attr_t  audio_attr;
static mutex_t         stream_mutex;

/* Guarded stream calls — mirror of sndwav.c. Refuses calls on a dead
 * handle and names the caller, rather than letting KOS assert (debug) or
 * command the AICA from uninitialised memory (release). Not used inside
 * adx_wav_init, which legitimately sets volume before status goes READY. */
static int stream_live(const char *who);
static void stream_volume(int vol, const char *who);
static void stream_stop(const char *who);
static void stream_poll(const char *who);

static void *sndadx_thread(void *param);

static int stream_live(const char *who)
{
    if (sndwav_status != SNDDRV_STATUS_READY ||
        stream.shnd == SND_STREAM_INVALID) {
        printf("[SND] adx %s: call on dead stream (shnd=%d status=%d)\n",
               who, (int)stream.shnd, sndwav_status);
        return 0;
    }
    return 1;
}

static void stream_volume(int vol, const char *who)
{
    if (stream_live(who)) {
        snd_stream_volume(stream.shnd, vol);
    }
}

static void stream_stop(const char *who)
{
    if (stream_live(who)) {
        snd_stream_stop(stream.shnd);
    }
}

static void stream_poll(const char *who)
{
    if (stream_live(who)) {
        snd_stream_poll(stream.shnd);
    }
}
static void *adx_file_callback(snd_stream_hnd_t hnd, int req, int *done);

/* Default music volume — engine calls adx_wav_volume to override.
 * Matches MUSIC_VOL_NORMAL in music_dc.c so the level is the same before
 * the first duck as it is after the restore. */
static int s_music_vol = 255;

int adx_wav_init(void)
{
    if (sndwav_status == SNDDRV_STATUS_READY)
        return sndwav_status;

    dbglog(DBG_INFO, "adx_wav_init(adx): enter\n");
    if (snd_stream_init() < 0) {
        stream.shnd = SND_STREAM_INVALID;
        return 0;
    }

    if (mutex_init(&stream_mutex, MUTEX_TYPE_NORMAL) < 0) {
        stream.shnd = SND_STREAM_INVALID;
        return 0;
    }

    /* Allocate the stream + refill buffer ONCE, up front, before SFX grabs the
     * remaining AICA channels. Reused for every track for the program's life. */
    stream.shnd = snd_stream_alloc(adx_file_callback, STREAM_BUF_SIZE);
    if (stream.shnd == SND_STREAM_INVALID)
        return 0;
    stream.drv_buf = memalign(32, STREAM_BUF_SIZE);
    if (stream.drv_buf == NULL) {
        snd_stream_destroy(stream.shnd);
        stream.shnd = SND_STREAM_INVALID;
        return 0;
    }

    stream.dec_open    = 0;
    stream.vol         = s_music_vol;
    stream.status      = SNDDEC_STATUS_NULL;
    stream.channels    = 2;
    stream.sample_rate = 44100;
    snd_stream_volume(stream.shnd, stream.vol);

    audio_attr.create_detached = 0;
    audio_attr.stack_size      = 32768;
    audio_attr.stack_ptr       = NULL;
    audio_attr.prio            = PRIO_DEFAULT;
    audio_attr.label           = "MusicPlayer";

    audio_thread = thd_create_ex(&audio_attr, sndadx_thread, NULL);
    if (audio_thread == NULL) {
        free(stream.drv_buf);
        stream.drv_buf = NULL;
        snd_stream_destroy(stream.shnd);
        stream.shnd = SND_STREAM_INVALID;
        return 0;
    }
    sndwav_status = SNDDRV_STATUS_READY;
    return sndwav_status;
}

void adx_wav_shutdown(void)
{
    sndwav_status = SNDDRV_STATUS_DONE;
    if (audio_thread) {
        thd_join(audio_thread, NULL);
        audio_thread = NULL;
    }
    mutex_lock(&stream_mutex);
    if (stream.shnd != SND_STREAM_INVALID) {
        snd_stream_destroy(stream.shnd);
        stream.shnd = SND_STREAM_INVALID;
    }
    if (stream.drv_buf) {
        free(stream.drv_buf);
        stream.drv_buf = NULL;
    }
    if (stream.dec_open) {
        Adx_Close(&stream.dec);
        stream.dec_open = 0;
    }
    stream.status = SNDDEC_STATUS_NULL;
    mutex_unlock(&stream_mutex);
}

/* Close the current track's decoder but leave the stream allocated for reuse. */
void adx_wav_destroy(void)
{
    if (sndwav_status != SNDDRV_STATUS_READY) return;

    mutex_lock(&stream_mutex);
    if (stream.status == SNDDEC_STATUS_STREAMING ||
        stream.status == SNDDEC_STATUS_RESUMING ||
        stream.status == SNDDEC_STATUS_PAUSING) {
        stream_stop(__func__);
    }
    stream.status = SNDDEC_STATUS_NULL;

    if (stream.dec_open) {
        Adx_Close(&stream.dec);
        stream.dec_open = 0;
    }
    mutex_unlock(&stream_mutex);
}

wav_stream_hnd_t adx_wav_create(const char *filename, int loop)
{
    if (sndwav_status != SNDDRV_STATUS_READY) return SND_STREAM_INVALID;
    if (filename == NULL)                      return SND_STREAM_INVALID;

    mutex_lock(&stream_mutex);

    if (stream.status == SNDDEC_STATUS_STREAMING ||
        stream.status == SNDDEC_STATUS_RESUMING) {
        stream_stop(__func__);
    }
    if (stream.dec_open) {
        Adx_Close(&stream.dec);
        stream.dec_open = 0;
    }

    if (Adx_Open(&stream.dec, filename) != 0) {
        mutex_unlock(&stream_mutex);
        return SND_STREAM_INVALID;
    }
    stream.dec_open    = 1;
    stream.loop        = loop;
    stream.channels    = stream.dec.channels;
    stream.sample_rate = stream.dec.sampleRate;
    stream.vol         = s_music_vol;

    stream_volume(stream.vol, __func__);
    stream.status = SNDDEC_STATUS_READY;
    mutex_unlock(&stream_mutex);

    return stream.shnd;
}

void adx_wav_play(void)
{
    stream_volume(stream.vol, __func__);
    if (stream.status == SNDDEC_STATUS_STREAMING) return;
    stream.status = SNDDEC_STATUS_RESUMING;

    int waited_ms = 0;
    while (stream.status == SNDDEC_STATUS_RESUMING && waited_ms < 2500) {
        thd_sleep(10);
        waited_ms += 10;
    }
}

void adx_wav_pause(void)
{
    stream_volume(0, __func__);
    if (stream.status == SNDDEC_STATUS_READY || stream.status == SNDDEC_STATUS_PAUSING) return;
    stream.status = SNDDEC_STATUS_PAUSING;
}

void adx_wav_stop(void)
{
    stream_volume(0, __func__);
    if (stream.status == SNDDEC_STATUS_READY || stream.status == SNDDEC_STATUS_STOPPING) return;
    stream.status = SNDDEC_STATUS_STOPPING;

    int waited_ms = 0;
    while (stream.status == SNDDEC_STATUS_STOPPING && waited_ms < 2500) {
        thd_sleep(10);
        waited_ms += 10;
    }
}

void adx_wav_volume(int vol)
{
    if (vol > 255) vol = 255;
    if (vol < 0)   vol = 0;
    s_music_vol = vol;
    if (stream.shnd == SND_STREAM_INVALID) return;
    stream.vol = vol;
    stream_volume(stream.vol, __func__);
}

int adx_wav_is_playing(void)
{
    return stream.status == SNDDEC_STATUS_STREAMING ||
           stream.status == SNDDEC_STATUS_RESUMING  ||
           stream.status == SNDDEC_STATUS_PAUSING;
}

static void *sndadx_thread(void *param)
{
    (void)param;
    while (sndwav_status != SNDDRV_STATUS_DONE) {
        mutex_lock(&stream_mutex);
        switch (stream.status) {
        case SNDDEC_STATUS_RESUMING:
            stream_volume(stream.vol, __func__);
            /* PCM mode (not _adpcm): the callback hands over decoded 16-bit. */
            snd_stream_start(stream.shnd, stream.sample_rate,
                             stream.channels - 1);
            stream_volume(stream.vol, __func__);
            stream.status = SNDDEC_STATUS_STREAMING;
            break;
        case SNDDEC_STATUS_PAUSING:
            stream_stop(__func__);
            stream.status = SNDDEC_STATUS_READY;
            break;
        case SNDDEC_STATUS_STOPPING:
            stream_stop(__func__);
            if (stream.dec_open)
                Adx_Rewind(&stream.dec);
            stream.status = SNDDEC_STATUS_READY;
            break;
        case SNDDEC_STATUS_STREAMING:
            stream_poll(__func__);
            break;
        case SNDDEC_STATUS_READY:
        default:
            break;
        }
        mutex_unlock(&stream_mutex);
        thd_sleep(20);
    }
    return NULL;
}

static void *adx_file_callback(snd_stream_hnd_t hnd, int req, int *done)
{
    (void)hnd;
    size_t got = Adx_Read(&stream.dec, stream.drv_buf, (size_t)req);
    if (got != (size_t)req) {
        /* End of stream. Rewind and, if looping, re-fill the whole request
         * from the top (mirrors sndwav.c's loop handling — a tiny seam at the
         * wrap, same as the .adp path). */
        Adx_Rewind(&stream.dec);
        if (stream.loop) {
            size_t got2 = Adx_Read(&stream.dec, stream.drv_buf, (size_t)req);
            if (got2 != (size_t)req) {
                stream_stop(__func__);
                stream.status = SNDDEC_STATUS_READY;
                return NULL;
            }
        } else {
            stream_stop(__func__);
            stream.status = SNDDEC_STATUS_READY;
            return NULL;
        }
    }
    *done = req;
    return stream.drv_buf;
}
