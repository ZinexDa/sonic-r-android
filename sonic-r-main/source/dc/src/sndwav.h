#ifndef SNDWAV_H
#define SNDWAV_H

#include <sys/cdefs.h>
__BEGIN_DECLS

#include <kos/fs.h>

/* Streamed playback for raw 44.1kHz stereo Yamaha ADPCM files (no header).
 * Adapted from a battle-tested cross-project music player; despite the
 * "wav_" prefix it's hard-coded to the ADPCM-only fast path.
 *
 * One stream at a time: wav_create returns a handle but everything else
 * is implicit on the global stream. */

#define WAVE_FORMAT_YAMAHA_ADPCM    0x0020
typedef struct {
    uint32_t format;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t sample_size;
    uint32_t data_offset;
    uint32_t data_length;
} WavFileInfo;

typedef int wav_stream_hnd_t;

/* Public API — dispatches to whichever backend wav_init() selected. */
int  wav_init(void);
void wav_shutdown(void);
void wav_destroy(void);

wav_stream_hnd_t wav_create(const char *filename, int loop);

void wav_play(void);
void wav_pause(void);
void wav_stop(void);
void wav_volume(int vol);
int  wav_is_playing(void);

/* File extension of the selected backend: "adp" or "adx". Valid after
 * wav_init(); reports the default before that. */
const char *wav_music_ext(void);

/* ---- Backends ----------------------------------------------------------
 * Both are linked in; exactly ONE is ever initialised, chosen at wav_init()
 * from which music files the disc actually carries. They cannot share a
 * stream: .adp hands ADPCM straight to AICA via snd_stream_start_adpcm with
 * an 8K buffer, .adx is decoded to PCM on the SH4 and needs 32K. Whichever
 * is not chosen allocates nothing.
 *
 * snd_stream_alloc must happen before SFX claims the remaining AICA
 * channels, which is why the choice is made at init, not at wav_create. */
int  adp_wav_init(void);
void adp_wav_shutdown(void);
void adp_wav_destroy(void);
wav_stream_hnd_t adp_wav_create(const char *filename, int loop);
void adp_wav_play(void);
void adp_wav_pause(void);
void adp_wav_stop(void);
void adp_wav_volume(int vol);
int  adp_wav_is_playing(void);

int  adx_wav_init(void);
void adx_wav_shutdown(void);
void adx_wav_destroy(void);
wav_stream_hnd_t adx_wav_create(const char *filename, int loop);
void adx_wav_play(void);
void adx_wav_pause(void);
void adx_wav_stop(void);
void adx_wav_volume(int vol);
int  adx_wav_is_playing(void);

__END_DECLS

#endif
