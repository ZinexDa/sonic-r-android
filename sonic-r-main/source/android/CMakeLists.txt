cmake_minimum_required(VERSION 3.22.1)
project(sonicr C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# SDL2 dependency
set(SDL_SHARED ON CACHE BOOL "Build SDL2 as shared library" FORCE)
set(SDL_STATIC OFF CACHE BOOL "Build SDL2 as static library" FORCE)
set(SDL_TEST OFF CACHE BOOL "Build SDL2 test programs" FORCE)
add_subdirectory(SDL2 SDL2_build EXCLUDE_FROM_ALL)

# SDL2_mixer dependency (static, WAV-only)
set(SDL2MIXER_VENDORED OFF CACHE BOOL "Use vendored third-party libraries" FORCE)
set(SDL2MIXER_CMD OFF CACHE BOOL "Support an external music player" FORCE)
set(SDL2MIXER_FLAC OFF CACHE BOOL "Enable FLAC music" FORCE)
set(SDL2MIXER_GME OFF CACHE BOOL "Support loading GME music via game-music-emu" FORCE)
set(SDL2MIXER_MOD OFF CACHE BOOL "Support loading MOD music" FORCE)
set(SDL2MIXER_MP3 OFF CACHE BOOL "Enable MP3 music" FORCE)
set(SDL2MIXER_MIDI OFF CACHE BOOL "Enable MIDI music" FORCE)
set(SDL2MIXER_OPUS OFF CACHE BOOL "Enable Opus music" FORCE)
set(SDL2MIXER_VORBIS OFF CACHE BOOL "Enable OGG Vorbis music" FORCE)
set(SDL2MIXER_WAVE ON CACHE BOOL "Enable streaming WAVE music" FORCE)
set(SDL2MIXER_WAVPACK OFF CACHE BOOL "Enable WavPack music" FORCE)
set(SDL2MIXER_SAMPLES OFF CACHE BOOL "Build the SDL2_mixer sample program(s)" FORCE)
set(SDL2MIXER_INSTALL OFF CACHE BOOL "Enable SDL2mixer install target" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build the library as a static library" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT TARGET SDL2::SDL2-static)
    add_library(SDL2::SDL2-static ALIAS SDL2)
endif()

add_subdirectory(SDL2_mixer SDL2_mixer_build EXCLUDE_FROM_ALL)

# Collect shared game sources from source/sdl/src
set(SONICR_SDL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdl")
file(GLOB_RECURSE ALL_SDL_C "${SONICR_SDL_DIR}/src/*.c")

# Exclude desktop GL and platform implementations
set(SONICR_SOURCES "")
foreach(F IN LISTS ALL_SDL_C)
    get_filename_component(FNAME ${F} NAME)
    if(FNAME STREQUAL "r_gl_backend.c" OR
       FNAME STREQUAL "render_gl.c" OR
       FNAME STREQUAL "r_soft_backend.c" OR
       FNAME STREQUAL "platform_sdl.c")
        # Excluded from Android build
    else()
        list(APPEND SONICR_SOURCES ${F})
    endif()
endforeach()

# Add Android-specific platform, rendering, and touch overlay implementations
list(APPEND SONICR_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform_android.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/r_gles_backend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/touch_overlay.c"
)

# Build libmain.so
add_library(main SHARED ${SONICR_SOURCES})

target_include_directories(main PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SONICR_SDL_DIR}/src
    ${SONICR_SDL_DIR}/src/sound
    ${SONICR_SDL_DIR}/src/net
    ${SONICR_SDL_DIR}/src/net/bearssl
    ${CMAKE_CURRENT_SOURCE_DIR}/SDL2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/SDL2_mixer/include
)

target_compile_definitions(main PRIVATE
    SONICR_SDL
    SONICR_ANDROID
    SONICR_DEBUG_LOG
    SONICR_FLOAT32
    SONICR_MATCHMAKER
    _GNU_SOURCE
    main=SDL_main
)

target_compile_options(main PRIVATE
    -Os
    -fno-common
    -Wall
    -Wextra
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-unused-result
    -Wno-strict-aliasing
    -Wno-misleading-indentation
    -Wno-array-bounds
    -Wno-parentheses
    -Wno-type-limits
    -Wno-unused-but-set-variable
    -Wno-implicit-fallthrough
    -Wno-void-pointer-to-int-cast
    -Wno-int-to-void-pointer-cast
    -Wno-pointer-to-int-cast
    -Wno-int-to-pointer-cast
)

target_link_libraries(main PRIVATE
    SDL2
    SDL2_mixer
    GLESv2
    log
    m
)
