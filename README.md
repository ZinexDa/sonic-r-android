# Sonic R — Android Edition

An Android port and enhancement of the [Sonic R decompilation by jnmartin84](https://github.com/jnmartin84/sonic-r), featuring an OpenGL ES 2.0 rendering backend, SDL2 integration, custom touch controls, and modern Android launcher architecture.

## Features

- **GLES 2.0 Pipeline**: Custom programmable shader pipeline replacing desktop OpenGL 1.x immediate mode.
- **Customizable Touch Controls**: Fully draggable and resizable on-screen D-Pad and action buttons with real-time visual preview.
- **Audio & SFX Controls**: Independent volume balancing and persistence via SDL2_mixer.
- **Dedicated Asset Installer**: Guided launcher UI allowing users to select their legal retail Sonic R PC folder.
- **Multiplayer Architecture**: Planned cross-platform network play powered by a Rust transport sidecar and signaling hub.

## Credits & Attribution

- **Decompilation Core**: Original reverse-engineering and C translation by [jnmartin84](https://github.com/jnmartin84/sonic-r) and contributors.
- **Android Port & GLES2 Backend**: Developed as an open-source extension of the decompilation project.
- **Game & Assets**: *Sonic R* is © SEGA / Traveller's Tales (1997/1998). This project is an unofficial, non-commercial fan preservation effort and contains no proprietary Sega code or retail assets.

## Legal Notice & Game Data Requirement

> [!IMPORTANT]
> **You must supply your own game data.** This repository contains **only the game engine and Android wrapper** — it does **not** distribute copyrighted retail tracks, textures, videos, or music. A legally purchased copy of *Sonic R* for PC is required.

## Building

Prerequisites:
- Android Studio / Android SDK (API 34, NDK 28.2+)
- JDK 21

```bash
cd app
.\gradlew.bat assembleDebug
```

Output APK will be at `app/build/outputs/apk/debug/SonicR-debug.apk`.

## License

This project is derived from the Sonic R decompilation by [jnmartin84](https://github.com/jnmartin84/sonic-r) and is distributed under the original author's custom non-commercial license terms:

- **Free to Download, Build, and Modify**: You may freely download, build, and modify this code.
- **Strict No-Sale Restriction**: You have no rights to sell this code in any form whatsoever — as source text, as an executable compiled for any platform, for profit or not for profit, as a "bonus disc" or any other packaging, with or without game data. Sale is not permitted under any circumstance.

See [LICENSE](LICENSE) for the full text.
