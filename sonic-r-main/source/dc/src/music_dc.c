/**
 * music_dc.c - Music playback for the Dreamcast build, via the streamed
 * Yamaha ADPCM player in sndwav.c. Music files live at
 *   {DATA_DIR}/MUSIC/trackN.adp
 * where N is the original CD track number (2..21), so calls like
 * UpdateCDPlayback(2) play track "Super Sonic Racing" from "track2.adp".
 * Same directory layout as the SDL build's music_sdl.c (DATA_DIR/MUSIC/trackN.wav). */

#include <stdint.h>
#include <stdio.h>

#include "sndwav.h"
#include "sonicr_paths.h"

/* The music format is whatever the disc carries. Both backends are linked in
 * and snd_dispatch.c picks one at wav_init() by probing for a known track;
 * wav_music_ext() reports which, so paths are built to match. */

extern int g_mciDeviceId;
extern int g_musicEnabled; /* 0x008FD4A0 - Music Volume on/off; gates UpdateCDPlayback */
extern void SFX_DuckStop(void); /* sound.c - clear the replay-commentary music duck */

static int s_currentTrack = 0;
static int s_currentTrackLoops = 0;     /* 1 if current track is looping */
static int s_currentTrackIsFanfare = 0; /* don't auto-replay finished fanfares */
static int s_paused = 0;

/* Music level. DUCKED is what the replay commentary drops it to (SFX_DuckMusic
 * in sound.c). wav_volume holds the value inside the streamer, so a track
 * starting mid-duck keeps the ducked level without play_track knowing. */
#define MUSIC_VOL_NORMAL 255
#define MUSIC_VOL_DUCKED (MUSIC_VOL_NORMAL / 2)

/* Music Volume slider level, 0-8 (mirrors g_optMusicVolume). Held locally so
 * the streamer level can be recomputed without either caller below knowing
 * the other's state. */
static int s_musicDucked = 0;
static int s_musicLevel = 8;

/* Slider and duck compose - the duck halves whatever the slider is set to. */
static void Music_ApplyVolume(void)
{
    int base = s_musicDucked ? MUSIC_VOL_DUCKED : MUSIC_VOL_NORMAL;
    wav_volume(base * s_musicLevel / 8);
}

void Music_SetVolume(int level)
{
    if (level < 0) {
        level = 0;
    }
    if (level > 8) {
        level = 8;
    }
    s_musicLevel = level;
    Music_ApplyVolume();
}

void Music_SetDucked(int ducked)
{
    s_musicDucked = ducked ? 1 : 0;
    Music_ApplyVolume();
}

/* Tracks 2, 3, 4, 0x13, 0x14, 0x15 are fanfares in the original game -
 * they play once. Everything else loops. Same set the SDL build keys off.
 * 0x13/0x14 are the post-race emerald-unlock-screen jingles. */
static int track_loops(int track)
{
    return !(track == 2 || track == 3 || track == 4 ||
             track == 0x13 || track == 0x14 || track == 0x15);
}

static void play_track(int track)
{
    if (track < 2 || track > 21) {
        return;
    }

    /* Don't auto-restart the same track. If it's still in flight we keep
     * playing it; if it's a finished fanfare we leave it silent so the
     * caller's per-frame poll doesn't re-fire it. */
    if (track == s_currentTrack) {
        if (wav_is_playing() || s_currentTrackIsFanfare) {
            return;
        }
    }

    /* Don't clobber a non-loop fanfare in flight with a different track -
     * the engine fires the next track immediately after the fanfare and
     * polls again later. Let the fanfare end naturally; the next request
     * will succeed once it does. Exception: a looping track (race music)
     * is allowed to cut the fanfare short so it's streaming by GO. */
    if (s_currentTrack != 2 && s_currentTrack != 0 && !s_currentTrackLoops && wav_is_playing()) {
        if (!track_loops(track)) {
            return;
        }
    }

    /* Tear down the previous stream before starting a new one - single
     * stream slot. */
    if (s_currentTrack != 0) {
        wav_stop();
        wav_destroy();
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/MUSIC/track%d.%s",
             DATA_DIR, track, wav_music_ext());

    int loops = track_loops(track);

    wav_stream_hnd_t h = wav_create(path, loops);
    if (h < 0) {
        s_currentTrack = 0;
        return;
    }

    wav_play();

    s_currentTrack = track;
    s_currentTrackLoops = loops;
    s_currentTrackIsFanfare = !loops;
    s_paused = 0;
}

static void stop_music(void)
{
    if (s_currentTrack == 0) {
        return;
    }

    wav_stop();
    wav_destroy();

    s_currentTrack = 0;
    s_currentTrackLoops = 0;
    s_paused = 0;
    s_currentTrackIsFanfare = 0;
}

/* GetLogicalCDTrack / OpenCDDevice / CloseCDDevice / StopCD /
 * UpdateCDPlayback - the binary's main music control. */

/* GetLogicalCDTrack - 0x004D0100. The binary derives "which medley track is
 * playing now" from elapsed time vs a per-track duration table; that table only
 * existed because redbook CD couldn't be asked "is this track still playing".
 * We have wav_is_playing(), so return the current track while it plays or the
 * next logical track (current+1) once a non-looping track has finished. */
int GetLogicalCDTrack(void)
{
    if (s_currentTrack == 0) {
        return 0;
    }

    if (wav_is_playing()) {
        return s_currentTrack;
    }

    return s_currentTrack + 1;  /* non-looping track finished → advanced */
}

int OpenCDDevice(void)
{
    g_mciDeviceId = 1;
    return 1;
}

void CloseCDDevice(void)
{
    wav_shutdown();
}

void PauseCD(void)
{
    if (s_currentTrack == 0) {
        return;
    }

    if (s_currentTrackIsFanfare && !wav_is_playing()) {
        return;
    }

    wav_pause();
    s_paused = 1;
}

void ResumeCD(void)
{
    if (s_paused) {
        wav_play();
        s_paused = 0;
    }
}

void StopCD(void)
{
    stop_music();
    /* Every path that abandons a replay stops the music, so clearing the duck
     * here covers the exits the race loop's per-frame tick can't reach. */
    SFX_DuckStop();
}

void UpdateCDPlayback(int track)
{
    /* Binary 0x4d01ba: mov ecx,[0x8fd4a0]; test ecx,ecx; je (return). When
     * music is disabled (Music Volume = off), UpdateCDPlayback plays nothing -
     * this gate keeps music off across screen/track transitions. Dropped in
     * translation, so "Music Volume off" had no effect. */
    if (g_musicEnabled == 0) {
        return;
    }
    play_track(track);
}
