/* sndwav.c — streamed Yamaha ADPCM playback for the DC.
 *
 * Hard-coded for raw 44.1kHz stereo ADPCM files with no header (so .adp
 * input is just the ADPCM payload). Single-stream: only one track plays
 * at a time. Background polling thread refills the AICA buffer.
 *
 * Adapted from the user's cross-project player (Doom 64 etc.). Doom-only
 * dependencies (doomdef.h, I_Error, menu_settings) removed; music volume
 * is a static driven through adp_wav_volume / set_music_volume from the
 * engine's volume control. */

#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include <kos/thread.h>
#include <dc/sound/stream.h>

#include "sndwav.h"
#include "fileio.h"

#define STREAM_BUF_SIZE (8192)

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

typedef void *(*snddrv_cb)(snd_stream_hnd_t, int, int *);

typedef struct {
    snd_stream_hnd_t shnd;
    FILE           *wave_file;
    uint8_t         *drv_buf;        /* 32-byte aligned AICA refill buffer */
    volatile int     status;
    snddrv_cb        callback;
    uint32_t         loop;
    uint32_t         vol;            /* 0-255 */
    uint32_t         format;
    uint32_t         channels;       /* 1=Mono / 2=Stereo */
    uint32_t         sample_rate;
    uint32_t         sample_size;
    uint32_t         data_offset;
    uint32_t         data_length;
    uint32_t         buf_offset;     /* unused; kept for symmetry */
} snddrv_hnd;

/* shnd MUST be initialised explicitly. SND_STREAM_INVALID is -1, so a
 * zero-initialised handle is 0 — a perfectly legal stream index — and every
 * "is there a stream?" test would pass while pointing at a slot this backend
 * never allocated. Both backends are linked in and only the one the disc
 * selects ever allocates, so the other one sits on that bad default. */
static snddrv_hnd      stream = { .shnd = SND_STREAM_INVALID };
static volatile int    sndwav_status = SNDDRV_STATUS_NULL;
static kthread_t      *audio_thread;
static kthread_attr_t  audio_attr;
static mutex_t         stream_mutex;

static void *sndwav_thread(void *param);
static void *wav_file_callback(snd_stream_hnd_t hnd, int req, int *done);

/* Guarded stream calls.
 *
 * KOS's snd_stream_* entry points assert the handle is live, and those
 * asserts compile out in a release build — where the call then indexes
 * streams[hnd] anyway and pushes a command built from uninitialised memory
 * at the AICA. Route everything through here so a dead handle is refused
 * and, more usefully, names the caller instead of aborting the system with
 * a stack trace that dies inside KOS's own assert handler.
 *
 * Not used by adp_wav_init: it calls snd_stream_volume once after a
 * successful alloc but before sndwav_status goes READY, so the guard would
 * reject a call that is actually fine. */
static int stream_live(const char *who)
{
    if (sndwav_status != SNDDRV_STATUS_READY ||
        stream.shnd == SND_STREAM_INVALID) {
        printf("[SND] adp %s: call on dead stream (shnd=%d status=%d)\n",
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

/* Default music volume — engine calls adp_wav_volume to override.
 * Matches MUSIC_VOL_NORMAL in music_dc.c so the level is the same before
 * the first duck as it is after the restore. */
static int s_music_vol = 255;

int adp_wav_init(void)
{
    /* Idempotent — calling twice is fine, returns the existing state. */
    if (sndwav_status == SNDDRV_STATUS_READY) {
        //dbglog(DBG_INFO, "adp_wav_init: already initialized\n");
        return sndwav_status;
    }

    //dbglog(DBG_INFO, "adp_wav_init: enter\n");
    if (snd_stream_init() < 0) {
        dbglog(DBG_ERROR, "adp_wav_init: snd_stream_init failed\n");
        stream.shnd = SND_STREAM_INVALID;
        return 0;
    }

    if (mutex_init(&stream_mutex, MUTEX_TYPE_NORMAL) < 0) {
        dbglog(DBG_ERROR, "adp_wav_init: mutex_init failed\n");
        stream.shnd = SND_STREAM_INVALID;
        return 0;
    }

    /* Allocate the stream and its refill buffer ONCE here, before SFX
     * loading grabs the rest of the AICA channels. The stream is reused
     * for every track played for the rest of the program — adp_wav_create
     * just opens a new file, never reallocates the stream. */
    stream.shnd = snd_stream_alloc(wav_file_callback, STREAM_BUF_SIZE);
    if (stream.shnd == SND_STREAM_INVALID) {
        dbglog(DBG_ERROR, "adp_wav_init: snd_stream_alloc failed\n");
        return 0;
    }
    stream.drv_buf = memalign(32, STREAM_BUF_SIZE);
    if (stream.drv_buf == NULL) {
        dbglog(DBG_ERROR, "adp_wav_init: memalign(8192) for drv_buf failed\n");
        snd_stream_destroy(stream.shnd);
        stream.shnd = SND_STREAM_INVALID;
        return 0;
    }

    stream.wave_file = NULL;
    stream.vol       = s_music_vol;
    stream.status    = SNDDEC_STATUS_NULL;
    stream.callback  = wav_file_callback;
    snd_stream_volume(stream.shnd, stream.vol);

    audio_attr.create_detached = 0;
    audio_attr.stack_size      = 32768;
    audio_attr.stack_ptr       = NULL;
    audio_attr.prio            = PRIO_DEFAULT;// - 1;
    audio_attr.label           = "MusicPlayer";

    audio_thread = thd_create_ex(&audio_attr, sndwav_thread, NULL);
    if (audio_thread == NULL) {
        dbglog(DBG_ERROR, "adp_wav_init: thd_create_ex failed\n");
        free(stream.drv_buf);
        stream.drv_buf = NULL;
        snd_stream_destroy(stream.shnd);
        stream.shnd = SND_STREAM_INVALID;
        return 0;
    }
    sndwav_status = SNDDRV_STATUS_READY;
    //dbglog(DBG_INFO, "adp_wav_init: ready (stream alloc'd up front, reused for life of program)\n");
    return sndwav_status;
}

void adp_wav_shutdown(void)
{
    /* Real teardown — only call this at program exit. */
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
    if (stream.wave_file != NULL) {
        fClose(stream.wave_file);
        stream.wave_file = NULL;
    }
    stream.status = SNDDEC_STATUS_NULL;
    mutex_unlock(&stream_mutex);
}

/* adp_wav_destroy — close the current track's file but leave the stream
 * allocated for reuse. Engine calls this between tracks. The stream itself
 * stays alive for the entire program (per channel-allocation requirement —
 * destroying and re-creating mid-game would race against SFX channels). */
void adp_wav_destroy(void)
{
    if (sndwav_status != SNDDRV_STATUS_READY) return;

    mutex_lock(&stream_mutex);

    /* Stop playback if active. */
    if (stream.status == SNDDEC_STATUS_STREAMING ||
        stream.status == SNDDEC_STATUS_RESUMING ||
        stream.status == SNDDEC_STATUS_PAUSING) {
        stream_stop(__func__);
    }
    stream.status = SNDDEC_STATUS_NULL;

    if (stream.wave_file != NULL) {
        fClose(stream.wave_file);
        stream.wave_file = NULL;
    }

    mutex_unlock(&stream_mutex);
}

static int wav_get_info_adpcm(FILE *file, WavFileInfo *result)
{
    result->format      = WAVE_FORMAT_YAMAHA_ADPCM;
    result->channels    = 2;
    result->sample_rate = 44100;
    result->sample_size = 4;
    if (fSeek(file, 0, SEEK_END) != 0) {
        fClose(file);
        return -1;
    }
    result->data_length = fTell(file);
    if (fSeek(file, 0, SEEK_SET) != 0) {
        fClose(file);
        return -1;
    }
    result->data_offset = 0;
    return 1;
}

wav_stream_hnd_t adp_wav_create(const char *filename, int loop)
{
    FILE      *file;
    WavFileInfo info;

    if (sndwav_status != SNDDRV_STATUS_READY) {
        //dbglog(DBG_ERROR, "adp_wav_create: adp_wav_init not called yet\n");
        return SND_STREAM_INVALID;
    }
    if (filename == NULL) {
        //dbglog(DBG_ERROR, "adp_wav_create: filename is NULL\n");
        return SND_STREAM_INVALID;
    }
    //dbglog(DBG_INFO, "adp_wav_create: opening %s (loop=%d)\n", filename, loop);

    file = fOpen(filename, "r");
    if (file == NULL) {
        //dbglog(DBG_ERROR, "adp_wav_create: fs_open(%s) failed\n", filename);
        return SND_STREAM_INVALID;
    }

    wav_get_info_adpcm(file, &info);
    /* dbglog(DBG_INFO, "adp_wav_create: %s data_length=%lu\n",
           filename, (unsigned long)info.data_length); */

    /* Stop the previous track and close its file (if any) — but DON'T
     * destroy the stream itself. */
    mutex_lock(&stream_mutex);
    if (stream.status == SNDDEC_STATUS_STREAMING ||
        stream.status == SNDDEC_STATUS_RESUMING) {
        stream_stop(__func__);
    }
    if (stream.wave_file != NULL) {
        fClose(stream.wave_file);
    }

    stream.wave_file   = file;
    stream.loop        = loop;
    stream.format      = info.format;
    stream.channels    = info.channels;
    stream.sample_rate = info.sample_rate;
    stream.sample_size = info.sample_size;
    stream.data_length = info.data_length;
    stream.data_offset = info.data_offset;
    stream.vol         = s_music_vol;

    fSeek(stream.wave_file, stream.data_offset, SEEK_SET);
    stream_volume(stream.vol, __func__);
    stream.status = SNDDEC_STATUS_READY;
    mutex_unlock(&stream_mutex);

    return stream.shnd;
}

void adp_wav_play(void)
{
    stream_volume(stream.vol, __func__);
    //printf("adp_wav_play\n");
    if (stream.status == SNDDEC_STATUS_STREAMING) return;
    //printf("adp_wav_play RESUMING\n");
    stream.status = SNDDEC_STATUS_RESUMING;

    /* Block until the polling thread has actually transitioned to
     * STREAMING so callers see consistent state immediately after
     * adp_wav_play returns. The thread polls every 50ms; cap at 500ms
     * so a broken thread can't deadlock us. */
    int waited_ms = 0;
    while (stream.status == SNDDEC_STATUS_RESUMING && waited_ms < 2500) {
        thd_sleep(10);
        waited_ms += 10;
    }
/*     if (stream.status != SNDDEC_STATUS_STREAMING) {
        dbglog(DBG_WARNING, "adp_wav_play: stream did not reach STREAMING (status=%d) after %dms\n",
               stream.status, waited_ms);
    } */
}

void adp_wav_pause(void)
{
    stream_volume(0, __func__);
    if (stream.status == SNDDEC_STATUS_READY || stream.status == SNDDEC_STATUS_PAUSING) return;
    stream.status = SNDDEC_STATUS_PAUSING;
}

void adp_wav_stop(void)
{
    stream_volume(0, __func__);
    if (stream.status == SNDDEC_STATUS_READY || stream.status == SNDDEC_STATUS_STOPPING) return;
    stream.status = SNDDEC_STATUS_STOPPING;

    /* Block until the polling thread has actually transitioned to
     * READY so callers see consistent state immediately after
     * adp_wav_stop returns. The thread polls every 50ms; cap at 500ms
     * so a broken thread can't deadlock us. */
    int waited_ms = 0;
    while (stream.status == SNDDEC_STATUS_STOPPING && waited_ms < 2500) {
        thd_sleep(10);
        waited_ms += 10;
    }
    /* if (stream.status != SNDDEC_STATUS_READY) {
        dbglog(DBG_WARNING, "adp_wav_stop: stream did not reach READY (status=%d) after %dms\n",
               stream.status, waited_ms);
    } */
}

void adp_wav_volume(int vol)
{
    if (vol > 255) vol = 255;
    if (vol < 0)   vol = 0;
    s_music_vol = vol;
    if (stream.shnd == SND_STREAM_INVALID) return;
    stream.vol = vol;
    stream_volume(stream.vol, __func__);
}

int adp_wav_is_playing(void)
{
    /* "Playing" includes RESUMING (the polling thread takes ~50ms to
     * transition RESUMING → STREAMING; in the gap, callers like the
     * engine's CheckCDStatus would otherwise see "not playing" and
     * stomp the just-requested track with the next one). PAUSING also
     * counts since the user-visible state is "still playing this track,
     * just paused." */
    return stream.status == SNDDEC_STATUS_STREAMING ||
           stream.status == SNDDEC_STATUS_RESUMING  ||
           stream.status == SNDDEC_STATUS_PAUSING;
}

static void *sndwav_thread(void *param)
{
    (void)param;
    while (sndwav_status != SNDDRV_STATUS_DONE) {
        mutex_lock(&stream_mutex);
        switch (stream.status) {
        case SNDDEC_STATUS_RESUMING:
            stream_volume(stream.vol, __func__);
            snd_stream_start_adpcm(stream.shnd, stream.sample_rate,
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
            if (stream.wave_file != NULL)
                fSeek(stream.wave_file, stream.data_offset, SEEK_SET);
            else
                stream.buf_offset = stream.data_offset;
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

static void *wav_file_callback(snd_stream_hnd_t hnd, int req, int *done)
{
    (void)hnd;
    size_t read = fRead(stream.drv_buf, 1, req, stream.wave_file);
#if 0
    if (read == -1) {
        //dbglog(DBG_ERROR, "wav_file_callback: fs_read failed (req=%d)\n", req);
        stream_stop(__func__);
        stream.status = SNDDEC_STATUS_READY;
        return NULL;
    }
#endif
    if (read != req) {
        if (fSeek(stream.wave_file, stream.data_offset, SEEK_SET) != 0) {
            //dbglog(DBG_ERROR, "wav_file_callback: fs_seek to data_offset failed\n");
            stream_stop(__func__);
            stream.status = SNDDEC_STATUS_READY;
            return NULL;
        }
        if (stream.loop) {
            size_t read2 = fRead(stream.drv_buf, 1, req, stream.wave_file);
            if (read2 != req) {
                //dbglog(DBG_ERROR, "wav_file_callback: loop fs_read failed (req=%d)\n", req);
                stream_stop(__func__);
                stream.status = SNDDEC_STATUS_READY;
                return NULL;
            }
        } else {
            //dbglog(DBG_INFO, "wav_file_callback: stream end (non-loop), stopping\n");
            stream_stop(__func__);
            stream.status = SNDDEC_STATUS_READY;
            return NULL;
        }
    }

    *done = req;
    return stream.drv_buf;
}
