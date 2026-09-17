/**
 * music_sdl.c — MP3/WAV music playback via SDL_mixer
 *
 * Replaces music_SDL.m (AVFoundation) with pure C SDL_mixer calls.
 * WAV files live in MUSIC/ directory, named track2.wav through track21.wav
 * matching the original CD track numbering.
 */

#include <SDL.h>
#include <SDL_mixer.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#define sys_mkdir(p) _mkdir(p)
#else
#include <unistd.h>
#define sys_mkdir(p) mkdir(p, 0755)
#endif
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "sonicr_paths.h"
#include "music_rwops.h"

static Mix_Music *s_musicTrack = NULL;
static int s_currentTrack = 0;
static int s_musicReady = 0;
static int s_currentTrackIsFanfare = 0;  /* don't auto-replay finished fanfares */

/* Per-track durations in ms — ROM table at 0x5041AC, indexed by CD track.
 * GetLogicalCDTrack times against these rather than asking whether audio is
 * still playing, so a ripped track whose padding differs from the redbook
 * original doesn't shift medley timing. */
static const int s_trackDurationMs[23] = {
         0,      0,  43210,   6993,  10170,  55580, 305305, 283904,
    270474, 237737, 295113, 241929, 240949, 164269, 208959, 210448,
    202700, 210360, 238142,   3921,   6451,   5910, 5448657
};

static int   s_logicalTrack = 0;   /* 0x6DA2A4 */
static DWORD s_trackStartMs = 0;   /* 0x6DA2A8 */

/* Music level, full scale to match dc/src/music_dc.c now that both platforms
 * put the SFX slider on the same even 1/8 steps. DUCKED is what the replay
 * commentary drops it to (see SFX_DuckMusic in sound.c). PlayCD reads
 * s_musicDucked so a track starting mid-duck comes in at the ducked level
 * instead of overriding it. */
#define MUSIC_VOL_NORMAL MIX_MAX_VOLUME
#define MUSIC_VOL_DUCKED (MUSIC_VOL_NORMAL / 2)
static int s_musicDucked = 0;

/* Music Volume slider level, 0-8 (mirrors g_optMusicVolume). Held locally so
 * the mixer level can be recomputed without either caller below knowing the
 * other's state. */
static int s_musicLevel = 8;
static float s_masterMusicVolume = 0.8f;

/* Slider and duck compose — the duck halves whatever the slider is set to. */
static int Music_EffectiveVolume(void)
{
    int base = s_musicDucked ? MUSIC_VOL_DUCKED : MUSIC_VOL_NORMAL;
    int vol = (int)(base * (s_musicLevel / 8.0f) * s_masterMusicVolume);
    if (vol < 0) vol = 0;
    if (vol > MIX_MAX_VOLUME) vol = MIX_MAX_VOLUME;
    return vol;
}

void Music_SetMasterVolume(float vol)
{
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    s_masterMusicVolume = vol;
    g_musicEnabled = (vol > 0.001f);
    if (vol <= 0.001f) {
        Mix_VolumeMusic(0);
    } else {
        Mix_VolumeMusic(Music_EffectiveVolume());
    }
}

float Music_GetMasterVolume(void)
{
    return s_masterMusicVolume;
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
    Mix_VolumeMusic(Music_EffectiveVolume());
}

void Music_SetDucked(int ducked)
{
    s_musicDucked = ducked ? 1 : 0;
    Mix_VolumeMusic(Music_EffectiveVolume());
}

void StopCD(void);

/**
 * OpenCDDevice — replaces MCI cdaudio open.
 * Just marks the music system as ready.
 */
void Music_TestTrackResolution(void);

int OpenCDDevice(void)
{
    DebugLog("OpenCDDevice (SDL_mixer mode)\n");
    s_musicReady = 1;
    g_mciDeviceId = 1;  /* non-zero = device ready */
    Music_TestTrackResolution();
    return 1;
}

/**
 * CloseCDDevice — replaces MCI close.
 */
void CloseCDDevice(void)
{
    if (s_musicTrack != NULL) {
        Mix_HaltMusic();
        Mix_FreeMusic(s_musicTrack);
        s_musicTrack = NULL;
    }
    s_currentTrack = 0;
    s_musicReady = 0;
    g_mciDeviceId = 0;
}

/* =====================================================================
 * Format Loaders & Candidate Path Resolution
 * ===================================================================== */

typedef Mix_Music *(*TrackLoaderFn)(const char *path);

static Mix_Music *loader_son(const char *path)
{
    SDL_RWops *rw = MusicRW_OpenSon(path, 44100, 2, 16);
    if (!rw) return NULL;
    return Mix_LoadMUS_RW(rw, 1);
}

static Mix_Music *loader_adx(const char *path)
{
    SDL_RWops *rw = MusicRW_OpenAdx(path);
    if (!rw) return NULL;
    return Mix_LoadMUS_RW(rw, 1);
}

static Mix_Music *loader_adp(const char *path)
{
    SDL_RWops *rw = MusicRW_OpenAdp(path, 44100, 2);
    if (!rw) return NULL;
    return Mix_LoadMUS_RW(rw, 1);
}

static Mix_Music *loader_mixer(const char *path)
{
    FILE *tf = fopen(path, "rb");
    if (!tf) return NULL;
    fclose(tf);
    Mix_Music *mm = Mix_LoadMUS(path);
    if (!mm) {
        SDL_Log("Mix_LoadMUS failed for '%s': %s", path, Mix_GetError());
    }
    return mm;
}

/**
 * try_load_candidates — systematically probe combinations of:
 *   - directory case (MUSIC, music, Music)
 *   - prefix case (TRACK, Track, track)
 *   - track number formatting (%d and %02d when trackNum < 10)
 *   - extensions provided by caller
 * Returns first successfully loaded Mix_Music*, or NULL if none found.
 */
static Mix_Music *try_load_candidates(int trackNum,
                                      const char *const exts[], size_t numExts,
                                      TrackLoaderFn loader,
                                      char *outResolvedPath, size_t maxPathLen)
{
    static const char *const dirs[] = { "MUSIC", "music", "Music" };
    static const char *const prefixes[] = { "TRACK", "Track", "track" };

    char numStrs[2][16];
    int numCount = 1;
    snprintf(numStrs[0], sizeof(numStrs[0]), "%d", trackNum);
    if (trackNum < 10) {
        snprintf(numStrs[1], sizeof(numStrs[1]), "%02d", trackNum);
        numCount = 2;
    }

    char path[512];
    for (size_t e = 0; e < numExts; e++) {
        for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
            for (size_t p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) {
                for (int n = 0; n < numCount; n++) {
                    snprintf(path, sizeof(path), DATA_DIR "/%s/%s%s.%s",
                             dirs[d], prefixes[p], numStrs[n], exts[e]);
                    Mix_Music *mm = loader(path);
                    if (mm) {
                        if (outResolvedPath && maxPathLen > 0) {
                            snprintf(outResolvedPath, maxPathLen, "%s", path);
                        }
                        return mm;
                    }
                }
            }
        }
    }
    return NULL;
}

/**
 * load_track — find and load the music file for a CD track number.
 *
 * Probes candidate paths in preference order, first existing loadable file wins:
 *   1. .son  raw headerless PCM (44100/16/stereo) — streamed via music_rwops shim
 *   2. .adx  CRI ADX ADPCM                         — decoded on demand via shim
 *   3. .adp  AICA ADPCM                            — decoded on demand via shim
 *   4. .ogg/.mp3/.flac/.wav                        — handed straight to SDL_mixer
 */
static Mix_Music *load_track(int trackNum)
{
    char resolvedPath[512] = {0};

    static const char *const son_exts[] = { "SON", "son" };
    static const char *const adx_exts[] = { "ADX", "adx" };
    static const char *const adp_exts[] = { "ADP", "adp" };
    static const char *const mixer_exts[] = {
        "ogg", "OGG",
        "mp3", "MP3",
        "flac", "FLAC",
        "wav", "WAV"
    };

    Mix_Music *mm = NULL;

    /* 1. Raw headerless PCM (.son) */
    mm = try_load_candidates(trackNum, son_exts, sizeof(son_exts) / sizeof(son_exts[0]),
                             loader_son, resolvedPath, sizeof(resolvedPath));
    if (mm) {
        SDL_Log("load_track(%d) loaded .son: '%s'", trackNum, resolvedPath);
        return mm;
    }

    /* 2. CRI ADX ADPCM (.adx) */
    mm = try_load_candidates(trackNum, adx_exts, sizeof(adx_exts) / sizeof(adx_exts[0]),
                             loader_adx, resolvedPath, sizeof(resolvedPath));
    if (mm) {
        SDL_Log("load_track(%d) loaded .adx: '%s'", trackNum, resolvedPath);
        return mm;
    }

    /* 3. AICA ADPCM (.adp) */
    mm = try_load_candidates(trackNum, adp_exts, sizeof(adp_exts) / sizeof(adp_exts[0]),
                             loader_adp, resolvedPath, sizeof(resolvedPath));
    if (mm) {
        SDL_Log("load_track(%d) loaded .adp: '%s'", trackNum, resolvedPath);
        return mm;
    }

    /* 4. Native SDL_mixer formats (.ogg, .mp3, .flac, .wav) */
    mm = try_load_candidates(trackNum, mixer_exts, sizeof(mixer_exts) / sizeof(mixer_exts[0]),
                             loader_mixer, resolvedPath, sizeof(resolvedPath));
    if (mm) {
        SDL_Log("load_track(%d) loaded audio: '%s'", trackNum, resolvedPath);
        return mm;
    }

    return NULL;
}

/* =====================================================================
 * Music Track Resolution Unit Testing
 * ===================================================================== */

static Mix_Music *test_file_probe_loader(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fclose(fp);
    return (Mix_Music *)(uintptr_t)1;
}

static void test_create_file(const char *path, int *created)
{
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        *created = 0;
        return;
    }
    f = fopen(path, "wb");
    if (f) {
        fputc(0, f);
        fclose(f);
        *created = 1;
    } else {
        *created = 0;
    }
}

void Music_TestTrackResolution(void)
{
    static int s_testRan = 0;
    if (s_testRan) return;
    s_testRan = 1;

    SDL_Log("MusicTrackResolution: Running automated track resolution verification...");

    /* Ensure test directories exist */
    sys_mkdir(DATA_DIR "/music");
    sys_mkdir(DATA_DIR "/MUSIC");

    struct TestCase {
        int trackNum;
        const char *createdPath;
        const char *const *exts;
        size_t numExts;
        const char *desc;
    };

    static const char *const ogg_exts[] = { "ogg", "OGG" };
    static const char *const mp3_exts[] = { "mp3", "MP3" };
    static const char *const wav_exts[] = { "wav", "WAV" };
    static const char *const flac_exts[] = { "flac", "FLAC" };
    static const char *const son_exts[] = { "SON", "son" };
    static const char *const adx_exts[] = { "ADX", "adx" };
    static const char *const adp_exts[] = { "ADP", "adp" };

    struct TestCase tests[] = {
        { 2, DATA_DIR "/music/track02.ogg", ogg_exts, 2, "zero-padded lowercase in music/" },
        { 3, DATA_DIR "/MUSIC/Track03.mp3", mp3_exts, 2, "zero-padded TitleCase in MUSIC/" },
        { 4, DATA_DIR "/music/TRACK4.wav",  wav_exts, 2, "non-padded uppercase prefix in music/" },
        { 5, DATA_DIR "/MUSIC/Track5.flac", flac_exts, 2, "non-padded TitleCase prefix in MUSIC/" },
        { 1, DATA_DIR "/music/Track01.son", son_exts, 2, "zero-padded TitleCase .son in music/" },
        { 7, DATA_DIR "/MUSIC/track07.adx", adx_exts, 2, "zero-padded lowercase .adx in MUSIC/" },
        { 8, DATA_DIR "/music/TRACK08.adp", adp_exts, 2, "zero-padded uppercase .adp in music/" }
    };

    int allPassed = 1;
    int numTests = (int)(sizeof(tests) / sizeof(tests[0]));

    for (int i = 0; i < numTests; i++) {
        int created = 0;
        test_create_file(tests[i].createdPath, &created);

        char resolved[512] = {0};
        Mix_Music *res = try_load_candidates(tests[i].trackNum, tests[i].exts, tests[i].numExts,
                                             test_file_probe_loader, resolved, sizeof(resolved));

        if (res && strcmp(resolved, tests[i].createdPath) == 0) {
            SDL_Log("MusicTrackResolution: [PASS] Case %d (%s) -> '%s'",
                    i + 1, tests[i].desc, resolved);
        } else {
            SDL_Log("MusicTrackResolution: [FAIL] Case %d (%s): expected '%s', got '%s'",
                    i + 1, tests[i].desc, tests[i].createdPath, resolved);
            allPassed = 0;
        }

        if (created) {
            remove(tests[i].createdPath);
        }
    }

    if (allPassed) {
        SDL_Log("MusicTrackResolution: [PASS] All 7 track naming variants (zero-padded, directory case, prefix case) resolved successfully!");
    } else {
        SDL_Log("MusicTrackResolution: [FAIL] One or more track naming variants failed resolution!");
    }
}

/**
 * PlayCD — replaces MCI play.
 * trackNum = CD track number from binary (2-21).
 * WAV files ripped from CD use matching track numbers: MUSIC/track2.wav, etc.
 */
void PlayCD(int trackNum)
{
    if (trackNum < 2 || trackNum > 21) {
        return;
    }
    if (g_musicEnabled == 0) {
        StopCD();
        return;
    }

    /* Already playing this track — don't restart */
    if (s_currentTrack == trackNum && s_musicTrack != NULL) {
        if (Mix_PlayingMusic()) {
            /* Re-baseline the medley clock. A looping track outlives its table
             * entry; the original re-asserted the redbook track at that point,
             * we let the mixer keep looping seamlessly and just restart the
             * timer so the logical position stays on this track. */
            s_logicalTrack = trackNum;
            s_trackStartMs = timeGetTime();
            return;             /* still in flight */
        }
        if (s_currentTrackIsFanfare) {
            return;        /* one-shot finished — don't replay */
        }
    }

    //DebugLog("PlayCD(%i) track%d.wav\n", trackNum, trackNum);

    /* Stop current music */
    if (s_musicTrack != NULL) {
        Mix_HaltMusic();
        Mix_FreeMusic(s_musicTrack);
        s_musicTrack = NULL;
    }
    s_currentTrack = 0;

    /* Find and load the track: .son/.adx stream via RWops shims, native
     * formats go straight to SDL_mixer. First existing MUSIC/track<N>.* wins. */
    s_musicTrack = load_track(trackNum);
    if (s_musicTrack == NULL) {
        DebugLog("PlayCD(%i) — no loadable MUSIC/track%d.* found: %s\n",
                 trackNum, trackNum, Mix_GetError());
        return;
    }

    int isFanfare = (trackNum == 2 || trackNum == 3 || trackNum == 4 ||
                     trackNum == 0x13 || trackNum == 0x14 || trackNum == 0x15);
    int loops = isFanfare ? 0 : -1;  /* 0 = play once, -1 = loop forever */

    Mix_VolumeMusic(Music_EffectiveVolume());
    if (Mix_PlayMusic(s_musicTrack, loops) != 0) {
        DebugLog("Mix_PlayMusic failed for track %d: %s\n", trackNum, Mix_GetError());
    } else {
        DebugLog("PlayCD(%d) playing (loops=%d, vol=%d)\n", trackNum, loops, Music_EffectiveVolume());
    }
    s_currentTrack = trackNum;
    s_currentTrackIsFanfare = isFanfare;
    s_logicalTrack = trackNum;              /* 0x4D01AC sets [0x6DA2A4] */
    s_trackStartMs = timeGetTime();         /* and [0x6DA2A8] */
}

/**
 * StopCD - 0x004D0264 — 42 bytes
 * Stops CD audio playback (MCI_STOP = 0x808).
 * Called between races and on shutdown.
 */
void StopCD(void)
{
    if (s_musicTrack != NULL) {
        Mix_HaltMusic();
        Mix_FreeMusic(s_musicTrack);
        s_musicTrack = NULL;
    }
    s_currentTrack = 0;
    s_currentTrackIsFanfare = 0;
    s_logicalTrack = 0;
    s_trackStartMs = 0;
    /* Every path that abandons a replay stops the music, so clearing the duck
     * here covers the exits the race loop's per-frame tick can't reach. */
    SFX_DuckStop();
}

/**
 * PauseCD — pause active music playback.
 */
void PauseCD(void)
{
    if (s_currentTrack == 0 || s_musicTrack == NULL) {
        return;
    }
    if (Mix_PlayingMusic() && !Mix_PausedMusic()) {
        Mix_PauseMusic();
    }
}

/**
 * ResumeCD — resume paused music playback.
 */
void ResumeCD(void)
{
    if (s_currentTrack == 0 || s_musicTrack == NULL) {
        return;
    }
    if (Mix_PausedMusic()) {
        Mix_ResumeMusic();
    }
}

/**
 * GetLogicalCDTrack — 0x004D0100
 * The binary computes "which track of the medley is playing now" from elapsed
 * time since PlayCD against a per-track duration table (0x5041ac). Redbook CD
 * had no reliable "is this track still playing" query, so it pre-baked track
 * durations to know when a non-looping track ended and the medley advanced.
 * We stream per track and have Mix_PlayingMusic() — the exact signal that table
 * was faking. So return the current track while it plays, or the next logical
 * track (current+1) once a non-looping track has finished. Callers use
 * `if (GetLogicalCDTrack() != N) PlayCD(N)` to keep track N asserted.
 */
int GetLogicalCDTrack(void)
{
    if (!s_musicReady || s_logicalTrack == 0) {
        return 0;
    }

    int elapsed = (int)(timeGetTime() - s_trackStartMs);   /* 0x4D0109 */
    if (elapsed < 0) {
        elapsed = -elapsed;                                /* 0x4D010F: cdq/xor/sub */
    }

    if (s_logicalTrack > 0 && s_logicalTrack < 23 &&
        elapsed > s_trackDurationMs[s_logicalTrack]) {
        s_logicalTrack++;                                  /* 0x4D0123 */
    }

    return s_logicalTrack;                                 /* 0x4D012C */
}

/**
 * UpdateCDPlayback — 0x004D01AC
 * In the original, this IS PlayCD — same function, same address.
 */
void UpdateCDPlayback(int trackNum)
{
    /* Binary 0x4d01ba: mov ecx,[0x8fd4a0]; test ecx,ecx; je (return). When
     * music is disabled (Music Volume = off), UpdateCDPlayback plays nothing —
     * this gate keeps music off across screen/track transitions. Dropped in
     * translation, so "Music Volume off" had no effect. */
    if (g_musicEnabled == 0) {
        return;
    }

    PlayCD(trackNum);
}
