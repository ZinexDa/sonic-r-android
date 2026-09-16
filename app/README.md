# Sonic R Android Port

Android port of the Sonic R decompilation, powered by SDL2, OpenGL ES 2.0 shaders, and CMake/NDK.

## Architecture

- **`app/`**: Standalone Gradle project configuring Android Gradle Plugin (AGP 8.12.3) and CMake 3.22.1 external native build.
  - Target SDK: 34, Min SDK: 21.
  - Native architectures: `arm64-v8a` and `x86_64`.
  - Android activities: `org.sonicr.android.LauncherActivity` (native UI launcher screen) and `org.sonicr.android.GameActivity` (SDL2 game engine host).
  - `tools/pack_assets.py`: Asset packager creating a case-tolerant `.tar.gz` archive with uppercase paths and lowercase symlinks.
- **`sonic-r-main/source/android/`**: Platform isolation layer for Android.
  - `SDL2/`: Bundled SDL 2.30.12 source distribution.
  - `src/platform_android.c`: Platform implementation of `platform.h` (SDL2 window, GLES 2.0 context, event polling, timing).
  - `src/r_gles_backend.c`: GLES2 geometry rendering backend replacing desktop OpenGL 1.x immediate mode. Implements `r_draw.h`, `r_state.h`, `r_texture.h` via dynamic VBO vertex submission, MVP orthographic shader matrix with perspective divide, GLES2 render state tracker, and 1x1 placeholder texture.
  - `src/sound_android_stub.c`: Audio stubs replacing SDL_mixer dependencies for this milestone.
  - `CMakeLists.txt`: Builds `libSDL2.so` and `libmain.so`.

## Prerequisites

- **JDK 21** (e.g. Eclipse Adoptium OpenJDK 21)
- **Android SDK** with:
  - Platforms: `android-34`
  - Build-tools: `35.0.0` or newer
  - NDK: `28.2.13676358` (or configured version)
  - CMake: `3.22.1`

## Building

From the `app/` directory:

```bash
# Windows
.\gradlew.bat assembleDebug

# Linux / macOS
./gradlew assembleDebug
```

Output APK:
`app/build/outputs/apk/debug/SonicR-debug.apk`

## Deploying Retail Game Assets

Sonic R requires retail PC game data (`GENERAL/`, `BIN/`, `ISLAND/`, etc.). Because Linux/Android is case-sensitive and `sonicr_paths.h` uses uppercase paths, use `pack_assets.py` to prepare an archive with uppercase primary files and lowercase symlinks:

```bash
# Pack PC assets from retail game folder (e.g. E:\Games\ssr)
python app/tools/pack_assets.py "E:\Games\ssr" sonicr_assets.tar.gz

# Push archive to Android device
adb push sonicr_assets.tar.gz /data/local/tmp/

# Extract directly into app internal storage (/data/user/0/org.sonicr.android/files/)
adb shell "run-as org.sonicr.android sh -c 'cd files && tar -xzf /data/local/tmp/sonicr_assets.tar.gz'"

# Clean up temporary archive
adb shell rm /data/local/tmp/sonicr_assets.tar.gz
```

## Running on Device or Emulator

1. Connect device or launch emulator (`emulator -avd <avd_name>`).
2. Install the APK:
   ```bash
   adb install -r app/build/outputs/apk/debug/SonicR-debug.apk
   ```
3. Start the application:
   ```bash
   adb shell am start -n org.sonicr.android/.LauncherActivity
   ```
4. Verification:
   - Screen launches in fullscreen landscape with letterbox/pillarbox maintaining a 4:3 active aspect ratio.
   - Transitions smoothly through SEGA logo, Traveller's Tales logo, Title screen, and initiates the attract demo race loop on Resort Island.
   - Renders 3D track and player geometry with flat per-vertex colors and Gouraud shading via GLES2 shaders.
