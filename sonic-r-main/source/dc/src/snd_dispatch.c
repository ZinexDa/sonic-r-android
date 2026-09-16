/* snd_dispatch.c — runtime music-backend selection for the DC.
 *
 * Two streaming backends are linked in: sndwav.c (.adp, handed straight to
 * AICA hardware ADPCM, zero SH4 cost) and sndadx.c (.adx, decoded to PCM on
 * the SH4). They implement the same nine calls under adp_/adx_ prefixes;
 * this file exports the unprefixed wav_* API the rest of the DC build uses
 * and forwards to whichever one is live.
 *
 * The disc carries one music set or the other, and it cannot change while
 * the program runs, so the choice is made once at wav_init() by probing for
 * a known track. That timing is forced anyway: each backend's init calls
 * snd_stream_alloc, which has to happen before SFX loading claims the
 * remaining AICA channels. The backend that isn't selected never allocates
 * its stream or its refill buffer.
 */

#include <stdio.h>

#include "sndwav.h"
#include "sonicr_paths.h"

/* Probe track — 2 is "Super Sonic Racing" and is present in every music set. */
#define PROBE_TRACK 2

enum { BACKEND_ADP = 0, BACKEND_ADX = 1 };

static int s_backend = BACKEND_ADP;
static int s_resolved = 0;

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

/* Pick a backend from what's actually on the disc. .adp wins when both are
 * present — it's the hardware path and costs no SH4 time. */
static void resolve_backend(void)
{
    char path[256];

    if (s_resolved) {
        return;
    }
    s_resolved = 1;

    snprintf(path, sizeof(path), "%s/MUSIC/track%d.adp", DATA_DIR, PROBE_TRACK);
    if (file_exists(path)) {
        s_backend = BACKEND_ADP;
        return;
    }

    snprintf(path, sizeof(path), "%s/MUSIC/track%d.adx", DATA_DIR, PROBE_TRACK);
    if (file_exists(path)) {
        s_backend = BACKEND_ADX;
        return;
    }

    /* Neither found — keep the default so wav_create just fails per track
     * rather than the sound system refusing to start. */
    s_backend = BACKEND_ADP;
}

const char *wav_music_ext(void)
{
    return (s_backend == BACKEND_ADX) ? "adx" : "adp";
}

int wav_init(void)
{
    resolve_backend();
    return (s_backend == BACKEND_ADX) ? adx_wav_init() : adp_wav_init();
}

void wav_shutdown(void)
{
    if (s_backend == BACKEND_ADX) {
        adx_wav_shutdown();
    }
    else {
        adp_wav_shutdown();
    }
}

void wav_destroy(void)
{
    if (s_backend == BACKEND_ADX) {
        adx_wav_destroy();
    }
    else {
        adp_wav_destroy();
    }
}

wav_stream_hnd_t wav_create(const char *filename, int loop)
{
    return (s_backend == BACKEND_ADX) ? adx_wav_create(filename, loop)
                                      : adp_wav_create(filename, loop);
}

void wav_play(void)
{
    if (s_backend == BACKEND_ADX) {
        adx_wav_play();
    }
    else {
        adp_wav_play();
    }
}

void wav_pause(void)
{
    if (s_backend == BACKEND_ADX) {
        adx_wav_pause();
    }
    else {
        adp_wav_pause();
    }
}

void wav_stop(void)
{
    if (s_backend == BACKEND_ADX) {
        adx_wav_stop();
    }
    else {
        adp_wav_stop();
    }
}

void wav_volume(int vol)
{
    if (s_backend == BACKEND_ADX) {
        adx_wav_volume(vol);
    }
    else {
        adp_wav_volume(vol);
    }
}

int wav_is_playing(void)
{
    return (s_backend == BACKEND_ADX) ? adx_wav_is_playing()
                                      : adp_wav_is_playing();
}
