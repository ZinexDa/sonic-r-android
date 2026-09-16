# Playing Sonic R on Sega Dreamcast

![Resort Island Dreamcast Screenshot](./sonic_r_dreamcast.png)

*Looking for Windows / macOS / Linux instead? See [desktop.md](desktop.md). Back to the [README](../README.md).*

> **You supply your own game data.** This port is only the game engine — it does
> not include any of Sonic R's assets. You must own a copy of the game to provide
> them. See [Game data you supply](#game-data-you-supply).

---

## Build a disc with Colab (no toolchain needed)

The Dreamcast runs from `sonicr.cdi`, a self-contained disc image that bundles the
engine and your game data. The easiest way to make one needs no toolchain — build
it in your browser:

> **Build a CDI in your browser.** A Google Colab notebook generates a Dreamcast
> `.cdi` for you.
> [Open the notebook.](https://colab.research.google.com/drive/1Ss1Dzn1lDjZirpeqZ1F9pikTpFTlNXHA?usp=sharing)

You supply your own **Sonic R data** (see [Game data you supply](#game-data-you-supply))
plus a **WAV soundtrack**, which Colab encodes to the efficient ADP format for
you. Copy the repo's bundled `DATA/` extras into your data folder **before** you
zip and upload.

Burn the resulting `.cdi` to a CD-R or copy it to your ODE.
Saves are written to the VMU, and up to four controllers are supported for split-screen play.

Prefer to build from a local KallistiOS toolchain instead? See
[Building from source](#building-from-source).

---

## Game data you supply

This port contains **only the game engine**. It does **not** include Sonic R game
data. To play, you must supply the data files from a copy of the game you own.

### Required data

You need these folders from an installed copy of the Sonic R PC release:

| Folder     | Contents                                             |
|------------|------------------------------------------------------|
| `GENERAL`  | Shared textures, character sheets, weather sprites   |
| `ISLAND`   | Resort Island track + textures                       |
| `CITY`     | Radical City track + textures                        |
| `RUIN`     | Regal Ruin track + textures                          |
| `FACTORY`  | Reactive Factory track + textures                    |
| `EMERALD`  | Radiant Emerald track + textures                     |
| `AI`       | Opponent pathfinding data                            |
| `BIN`      | Models, menus, titles, demos, environment maps       |
| `SOUND`    | Sound effects                                        |

> **Filenames must be UPPERCASE.** The original PC data ships in mixed case, and
> inside a Dreamcast disc image the names must be uppercased or the game will fail
> to find its assets. `dreamcast/tools/uppercase_tree.sh` will do this for a copy
> of the data tree (and clean up any junk macOS leaves behind).

### Bundled extras

A few assets the original PC release doesn't include (or doesn't include in a
usable Dreamcast form) ship in this repo under `DATA/`:

- `BIN/OPTION/NET01.RAW` — the network-screen platform icons.
- `GENERAL/SONICR.ICO` — the icon shown on your VMU saves in the Dreamcast BIOS.
  (Without it, saves still work — they just appear iconless.)
- `SOUND/DCSFX/` — the **complete set of Dreamcast-encoded sound effects**. The
  Colab disc builder encodes your *music* but not the sound effects, so shipping
  them pre-encoded here is what gives browser-built discs full sound.

**Copy the contents of `DATA/` into your data folder before you build** — for
Colab, do this before you zip and upload:

```sh
cp -R DATA/. /path/to/gamedata/
```

(A from-source build does this for you automatically when it packs the disc.)

### Music

The Dreamcast plays only **ADX** or **ADP**. Put your tracks in `MUSIC/`, named by
CD track number — `MUSIC/track2.<ext>` … through `track21`.

- **WAV → ADP (recommended).** Supply `MUSIC/track2.wav` … (44.1 kHz / 16-bit /
  stereo) and the builder encodes it to ADP for you — the **Colab builder requires
  WAV** and does this automatically. ADP streams straight to the sound hardware
  with zero CPU cost.
- **ADP** — `MUSIC/track2.adp`, … raw 44.1 kHz stereo AICA ADPCM, used as-is.
- **ADX** — `MUSIC/track2.adx`, … what the *Sonic R Updater* installs. It's
  decoded on the SH4 CPU at playback, which can cost a little performance in heavy
  scenes, so ADP is preferred where you have the choice.

---

## Controls

Each player uses their own Dreamcast controller. Steering is on the D-pad or the
left analog stick; the rest of the defaults are below and can be remapped from
Options → Controls. Plug in up to four controllers for 3- and 4-player
split-screen.

| Action              | Dreamcast controller           |
|---------------------|--------------------------------|
| Steer               | D-pad **or** left analog stick |
| Accelerate          | **B** or **X**                 |
| Jump / ability      | **A**                          |
| Sharp turn left     | **L** trigger                  |
| Sharp turn right    | **R** trigger                  |
| Look back (camera)  | **Y**                          |
| Start / pause       | **Start**                      |

In menus, **A** or **Start** confirms and **B** / **X** backs out.

### Network lobby (no keyboard needed)

The network screens were built for a keyboard and still show their original
**function-key prompts** on screen — `F1`, `F2`, `F3`, `F5`, `F6`, `F7`, `F8`. You
do **not** need a keyboard: a standard controller drives every one of them. Match
the on-screen prompt to the button below.

| On-screen prompt | What it does                     | Controller button |
|------------------|----------------------------------|--------------------|
| `F1`             | Host a game / start the race     | **A** or **Start** |
| `F2`             | Join a game                      | **R** trigger      |
| `F3`             | Skip sign-in / LAN-only play     | **Y**              |
| `F6`             | Change character                 | **D-pad ← / →**    |
| `F7`             | Change game mode                 | **L** trigger      |
| `F8`             | Change track                     | **D-pad ↑ / ↓**    |
| *(leave)*        | Back out to the previous menu    | **B** or **X**     |

`F5` is inert in this build — no controller button maps to it. A keyboard still
works if you plug one in and prefer the F-keys directly.

---

## Game modes

All original game modes are supported, including split-screen multiplayer.

---

## Network play

Online and LAN play use UDP on port **7847**. Everyone in a session must be
running the **same build and the same game data**.

Network play on Dreamcast requires a **Broadband Adapter (BBA)** or **W5500**. The
network stack comes up the first time you enter a network game. Dial-up **modem
play is not supported** at this time.

The entire network lobby — host, join, and character / track / mode selection — is
driven by the controller, so no keyboard is needed (see
[Network lobby](#network-lobby-no-keyboard-needed)).

**The matchmaker is the recommended way to play online.** It lets you find
internet games without swapping IPs — and signing in gives you your own username.
Skip sign-in (or play LAN-only) and you simply appear to others as **"Dreamcast"**.

**Full cross-play with desktop.** A Dreamcast speaks the identical protocol, so it
plays with PC / Mac / Linux players in any combination — a Dreamcast hosting a
desktop client, a desktop hosting a Dreamcast client, or Dreamcast-to-Dreamcast —
as long as everyone is on the same version and data.

### Signing in and out

Signing in to the matchmaker saves a small token (your credential plus your
username) as a VMU save named `SONICR_NET`, so you stay signed in on later
launches. To **sign out**, or if the matchmaker misbehaves (a stale token, the
wrong name, a sign-in that won't take), delete `SONICR_NET` from the VMU using the
Dreamcast BIOS file manager (boot with no disc in the drive). The next successful
sign-in writes a fresh token.

---

## Save data

Everything the game saves is a file on the VMU. Delete any of them from the
Dreamcast BIOS file manager (boot with no disc in the drive).

| What                  | VMU file      |
|-----------------------|---------------|
| Progress / records    | `SONICR_SAVE` |
| Options               | `SONICR_OPT`  |
| Gamepad configuration | `SONICR_PAD`  |
| Keyboard bindings     | `SONICR_KEY`  |
| Time Attack ghosts    | `SONICR_GHO`  |
| Matchmaker sign-in    | `SONICR_NET`  |

To reset progress, delete `SONICR_SAVE`. To reset controls, delete `SONICR_PAD`
(gamepad) or `SONICR_KEY` (keyboard). To sign out of the matchmaker, delete
`SONICR_NET`.

---

## Troubleshooting

| Symptom                               | Fix                                                        |
|---------------------------------------|------------------------------------------------------------|
| Black screen / missing textures       | Data folders missing or wrong case — uppercase them before building. |
| Crash right after the logos           | Incomplete data set — make sure all folders are present.   |
| No music                              | Add a `MUSIC` folder with your tracks (WAV for Colab).     |
| Controls feel wrong                   | Reset in Options, or delete `SONICR_PAD` / `SONICR_KEY` from the VMU. |
| Matchmaker won't sign in / wrong name | Delete `SONICR_NET` from the VMU and retry.                |
| No network                            | Confirm the BBA / W5500 adapter is seated and detected.    |

---

## Building from source

Only needed if you'd rather build locally than use Colab, or to modify the game.

**Dependencies:** a working [KallistiOS](https://github.com/KallistiOS/KallistiOS)
toolchain and [`mkdcdisc`](https://gitlab.com/simulant/mkdcdisc) for building the
disc image.

```sh
source /opt/toolchains/dc/kos/environ.sh              # required — sets KOS_BASE
cd source/dc
make                                                  # builds sonicr.elf
make GAMEDATA=/path/to/gamedata cdi                   # wraps ELF + your data into sonicr.cdi
```

`GAMEDATA` is the path to your extracted Sonic R data folder — the directory that
contains `BIN`, `GENERAL`, `ISLAND`, and so on. The `cdi` target packs your data
folders into `build/sonicr.cdi`. Running `make cdi` without `GAMEDATA` set stops
with an error telling you to provide it.

**Bundled assets are added automatically.** Anything the original PC data set
lacks — the `BIN/OPTION/NET01.RAW` network icons and the pre-encoded sound effects
(`SOUND/DCSFX/*.WAV`) — is copied in from the repo's `DATA/` folder if it isn't
already present, so you don't have to place it yourself.

**Music.** Put your tracks in `MUSIC/`, named by CD track number. Supply WAV
(44.1 kHz / 16-bit / stereo) and the build encodes it to ADP for you with
`wav2adpcm` from your KOS toolchain — the zero-CPU soundtrack. Pre-made `.adp`/
`.adx` are used as-is. Keep source WAVs outside the data folder with
`WAV_DIR=/path/to/wavs`. Only `.adp`/`.adx` are packed onto the disc, so a stray
`.wav` left in the data folder can't sneak in and oversize the image.

If `mkdcdisc` isn't installed at the default location (`~/mkdcdisc/...`), point the
build at it too:

```sh
make MKDCDISC=/path/to/mkdcdisc GAMEDATA=/path/to/gamedata cdi
```

> **Note:** the Dreamcast Makefile does not track header dependencies. After
> editing any `.h`, run `make clean && make` so the change is picked up.
