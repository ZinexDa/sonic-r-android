# Sonic R Dreamcast build (KallistiOS)
#
# Source `/opt/toolchains/dc/kos/environ.sh` before running this.
# Game logic is shared with the SDL build (../sdl/src), with DC-specific
# replacements in dc/src for the renderer, platform, sound, and entry point.

ifeq ($(KOS_BASE),)
$(error KOS_BASE not set. source /opt/toolchains/dc/kos/environ.sh first)
endif

DC_SRCDIR  := src
SDL_SRCDIR := ../sdl/src
BUILDDIR   := build
TARGET     := sonicr.elf

# Files in sdl/src that DC replaces with its own version. These are excluded
# from the SDL source list so the DC equivalents (in dc/src) win.
# main.c stays in the build - its SDL/Win32-specific blocks are guarded by
# #ifdef SONICR_DC, so the DC bootstrap path is reached without forking the
# 2000+ lines of screen-flow logic into a parallel main_dc.c.
SDL_REPLACED := \
    $(SDL_SRCDIR)/render_track_sdl.c \
    $(SDL_SRCDIR)/r_gl_backend.c \
    $(SDL_SRCDIR)/render_gl.c \
    $(SDL_SRCDIR)/platform/platform_sdl.c \
    $(SDL_SRCDIR)/sound/music_sdl.c \
    $(SDL_SRCDIR)/sound/sfx_sdl.c \
    $(SDL_SRCDIR)/sound/music_rwops.c \
    $(SDL_SRCDIR)/sound/adp.c \
    $(SDL_SRCDIR)/sound/adx.c \
    $(SDL_SRCDIR)/gles2/r_gles2_backend.c

# Discover all SDL sources, then strip the replaced ones. music_rwops.c pulls in
# <SDL.h>/SDL_mixer and adp.c is the SDL-side software ADPCM decoder - neither
# belongs in the DC build. adx.c is added back below: dc/src/sndadx.c uses it.
SDL_ALL_C  := $(shell find $(SDL_SRCDIR) -name '*.c')
SDL_C      := $(filter-out $(SDL_REPLACED), $(SDL_ALL_C))

# DC-specific sources
DC_ALL_C   := $(shell find $(DC_SRCDIR) -name '*.c')

# Both music backends are compiled in: sndwav.c (.adp streamed direct-to-AICA,
# zero CPU) and sndadx.c (.adx CPU-decoded to PCM via the shared adx.c). They
# expose adp_wav_*/adx_wav_*; snd_dispatch.c exports the wav_* API sfx_dc.c and
# music_dc.c call, and picks a backend at wav_init() by probing the disc. Only
# the selected one allocates a stream, so carrying both costs code size only.
SDL_C += $(SDL_SRCDIR)/sound/adx.c
DC_C  := $(DC_ALL_C)

CSRC       := $(SDL_C) $(DC_C)

# Mirror the source tree under build/: SDL sources land at build/sdl/src/...,
# DC sources at build/dc/src/...  The leading '../' is stripped from SDL paths
# (so '../sdl/src/X.c' -> 'build/sdl/src/X.o'); DC paths get a 'dc/' prefix
# (so 'src/Y.c' -> 'build/dc/src/Y.o').
SDL_OBJS := $(patsubst ../%.c,$(BUILDDIR)/%.o,$(SDL_C))
DC_OBJS  := $(patsubst %.c,$(BUILDDIR)/dc/%.o,$(DC_C))
OBJS     := $(SDL_OBJS) $(DC_OBJS)

# Debug logging: on by default for dev builds; ship with `make DEBUG_LOG=0`
# so DebugLog/DebugLogAlt compile back to a no-op (see sdl/src/util.c).
DEBUG_LOG ?= 0
ifeq ($(DEBUG_LOG),1)
  DEBUG_LOG_CFLAGS = -DSONICR_DEBUG_LOG
else
  DEBUG_LOG_CFLAGS =
endif


CFLAGS = -I$(DC_SRCDIR) -I$(SDL_SRCDIR) \
         -std=gnu11 -O3 -g -fno-common -Wall -Wno-unused -Wno-parentheses \
         -Wno-strict-aliasing -Wno-unused-result -Wno-misleading-indentation \
         -Wno-array-bounds -Wno-attribute-warning -Wno-maybe-uninitialized \
         -Wno-type-limits -Wno-stringop-overflow -Wno-unused-but-set-variable \
         -Wno-implicit-fallthrough -Wno-pointer-sign -Wno-stringop-truncation \
         -DSONICR_SDL -DSONICR_DC -DSONICR_FLOAT32 -DSONICR_MATCHMAKER -DBLURRY \
         -DDATA_DIR='"/cd"' $(DEBUG_LOG_CFLAGS)

# -DSONICR_DC_240P

LDFLAGS = -lppp -lm

.PHONY: all clean cdi audit-dead

all: $(BUILDDIR)/$(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/$(TARGET): $(OBJS) | $(BUILDDIR)
	kos-cc -o $@ $(OBJS) $(LDFLAGS)

# Pattern rules: object paths mirror source paths under build/.
$(BUILDDIR)/sdl/%.o: ../sdl/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	kos-cc $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/dc/%.o: %.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	kos-cc $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)

# Whole-program reachability sweep: reports every function the linker could not
# reach from the entry point. More trustworthy than grepping for callers — it
# follows relocations, so a function reached only through a pointer or a
# dispatch table still counts as live.
#
# KOS_CFLAGS/KOS_LDFLAGS already supply -ffunction-sections -fdata-sections and
# -Wl,--gc-sections, so every normal build is ALREADY dead-stripped; the only
# thing this target adds is --print-gc-sections to make the linker say what it
# dropped. Both gc flags are repeated below so the target keeps working if those
# KOS defaults ever change.
#
# -fno-lto is REQUIRED, not an optimisation. KOS builds with -flto=auto, so a
# normal link recompiles everything into /var/folders/.../ccXXXX.debug.temp.o
# and the linker reports those temp paths — our own object names never appear
# and the report silently comes back empty. -ffat-lto-objects means each .o
# also carries real machine code, so -fno-lto links that instead and object
# identity survives. Verified with a deliberately-dead probe function: without
# -fno-lto it is not reported, with it, it is.
#
# Output is filtered to our own translation units; unreachable newlib/KOS/BearSSL
# sections are expected and uninteresting. Exits nonzero if anything is found.
audit-dead: $(OBJS) | $(BUILDDIR)
	@kos-cc -fno-lto -o $(BUILDDIR)/audit.elf $(OBJS) $(LDFLAGS) \
	     -Wl,--gc-sections -Wl,--print-gc-sections 2>&1 \
	  | grep -E "in file .*build/(sdl|dc)/" \
	  | grep -v "bearssl" \
	  | sed -E "s|.*section [']([^']*)['] in file [']([^']*)['].*|\2  \1|" \
	  | grep "[.]text[.]" | sed "s|[.]text[.]||" \
	  | sort -k2 > $(BUILDDIR)/dead.txt; \
	  n=$$(wc -l < $(BUILDDIR)/dead.txt | tr -d ' '); \
	  echo "unreferenced functions in our sources: $$n"; \
	  cat $(BUILDDIR)/dead.txt; \
	  test "$$n" -eq 0

# Disc image (CDI). Builds the ELF first, then wraps it with the game data.
#
# Point the build at your extracted Sonic R data folder with GAMEDATA:
#     make GAMEDATA=/path/to/gamedata cdi
# GAMEDATA is the HOST path to the folder holding the game's data subdirectories
# (BIN, GENERAL, ISLAND, ...). It is unrelated to the on-disc "/cd" path baked
# into the ELF via -DDATA_DIR above — that is where the game reads data at
# runtime; this is where mkdcdisc reads it from at build time.
#
# Override MKDCDISC if your build is somewhere else, or trim DATA_DIRS for
# a faster turnaround when you don't need the assets.
MKDCDISC  ?= $(HOME)/mkdcdisc/builddir/mkdcdisc
GAMEDATA  ?=
# MUSIC is packed separately, from a staging dir (MUSIC_STAGE) that holds only
# .adp/.adx — so a stray .wav in the data folder can never get onto the disc and
# oversize the image. Hence MUSIC is NOT in DATA_DIRS.
#
# GHOST is deliberately NOT shipped either: the disc is read-only, so a baked-in
# ghost can never be replaced, and a zero/garbage .gho reads as an unbeatable
# 0:00 record on every track. Ghosts are recorded at play time (to the VMU on
# hardware) — the disc must ship with none.
DATA_DIRS ?= AI BIN CITY EMERALD FACTORY GENERAL ISLAND RUIN SAVE SOUND
MUSIC_STAGE := $(BUILDDIR)/MUSIC
DATA_FLAGS = $(foreach d,$(DATA_DIRS),-d $(GAMEDATA)/$(d)) -d $(MUSIC_STAGE)
CDI       := $(BUILDDIR)/sonicr.cdi

# Some assets aren't part of the original PC data set: the network-screen icons
# (BIN/OPTION/NET01.RAW), the VMU save icon (GENERAL/SONICR.ICO), and the full
# set of Dreamcast-encoded sound effects (SOUND/DCSFX/). They live in the bundled
# DATA/ tree and are copied into the data folder below if absent. The desktop-only
# PCM replay voice under DATA/SOUND/SFX is excluded from the seed and the image.
DATA_SEED := ../../DATA

# Dreamcast music must be .adp (raw headerless 44.1 kHz stereo AICA ADPCM) or
# .adx; the SDL_mixer formats (.wav/.ogg/...) are desktop-only. Every KOS install
# ships wav2adpcm — the same toolchain you built the ELF with — so any track*.wav
# is encoded to .adp at pack time. Pre-encoded and freshly-encoded tracks are
# gathered into MUSIC_STAGE (above) and only that dir is packed, so a leftover
# .wav in the data folder can neither reach nor oversize the image.
# Override WAV2ADPCM if yours lives elsewhere; if it's missing we warn and skip.
WAV2ADPCM ?= $(KOS_BASE)/utils/wav2adpcm/wav2adpcm

# Where the source track*.wav live. Defaults to the data folder's own MUSIC dir
# (so `make GAMEDATA=... cdi` just works); point WAV_DIR at a separate music
# folder to keep your WAVs out of the data dir entirely.
#   make GAMEDATA=/path/to/gamedata WAV_DIR=/path/to/wavs cdi
WAV_DIR ?= $(GAMEDATA)/MUSIC

cdi: $(CDI)

$(CDI): $(BUILDDIR)/$(TARGET)
	@if [ -z "$(GAMEDATA)" ]; then \
	    echo "error: GAMEDATA is not set."; \
	    echo "   point it at your extracted Sonic R data folder, e.g.:"; \
	    echo "   make GAMEDATA=/path/to/gamedata cdi"; \
	    exit 1; \
	fi
	@if [ ! -d "$(GAMEDATA)" ]; then \
	    echo "error: GAMEDATA '$(GAMEDATA)' does not exist or is not a directory."; \
	    exit 1; \
	fi
	@if [ -d "$(DATA_SEED)" ]; then \
	    for f in $$(cd $(DATA_SEED) && find . -type f ! -path './SOUND/SFX/*' | sed 's|^\./||'); do \
	        if [ ! -f "$(GAMEDATA)/$$f" ]; then \
	            echo "seeding $$f from DATA/"; \
	            mkdir -p "$(GAMEDATA)/$$(dirname "$$f")"; \
	            cp "$(DATA_SEED)/$$f" "$(GAMEDATA)/$$f"; \
	        fi; \
	    done; \
	fi
	@# Assemble the on-disc MUSIC dir under build/: copy in any already-encoded
	@# tracks, then encode WAVs that still lack one. Only this staged dir is
	@# packed (see DATA_FLAGS), so a .wav in the data folder never reaches the disc.
	@rm -rf "$(MUSIC_STAGE)"
	@mkdir -p "$(MUSIC_STAGE)"
	@if [ -d "$(GAMEDATA)/MUSIC" ]; then \
	    for a in "$(GAMEDATA)"/MUSIC/track*.adp "$(GAMEDATA)"/MUSIC/track*.adx; do \
	        if [ -f "$$a" ]; then cp "$$a" "$(MUSIC_STAGE)/"; fi; \
	    done; \
	fi
	@if [ -x "$(WAV2ADPCM)" ]; then \
	    for w in "$(WAV_DIR)"/track*.wav "$(WAV_DIR)"/track*.WAV; do \
	        [ -f "$$w" ] || continue; \
	        n=$$(basename "$$w"); n=$${n%.*}; \
	        if [ ! -f "$(MUSIC_STAGE)/$$n.adp" ] && [ ! -f "$(MUSIC_STAGE)/$$n.adx" ]; then \
	            echo "encoding $$n.wav -> MUSIC/$$n.adp (Dreamcast)"; \
	            "$(WAV2ADPCM)" -n -i -t "$$w" "$(MUSIC_STAGE)/$$n.adp"; \
	        fi; \
	    done; \
	elif ls "$(WAV_DIR)"/track*.wav "$(WAV_DIR)"/track*.WAV >/dev/null 2>&1; then \
	    echo "note: wav2adpcm not found at $(WAV2ADPCM) —"; \
	    echo "      track*.wav will be SILENT on Dreamcast (it needs .adp or .adx)."; \
	    echo "      set WAV2ADPCM=/path/to/wav2adpcm, or add track*.adp/.adx yourself."; \
	fi
	$(MKDCDISC) -s "IND-696969" -e $< -o $@ -n "Sonic R" -N $(DATA_FLAGS)
