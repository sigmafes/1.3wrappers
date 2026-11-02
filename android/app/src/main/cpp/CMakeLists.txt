cmake_minimum_required(VERSION 3.18)
project(gdps13_android C CXX)

# Android 15+: librerías alineadas a páginas de 16 KB
add_link_options("-Wl,-z,max-page-size=16384")

# SDL2 y SDL2_mixer desde su código fuente (descargado por el workflow en SDL/ y SDL_mixer/).
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL)

set(SDL2MIXER_VENDORED OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_OPUS OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MOD OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MIDI OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_WAVPACK OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_FLAC OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_GME OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MP3 ON CACHE BOOL "" FORCE)
set(SDL2MIXER_MP3_MINIMP3 ON CACHE BOOL "" FORCE)
set(SDL2MIXER_MP3_MPG123 OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_VORBIS STB CACHE STRING "" FORCE)
add_subdirectory(SDL_mixer)

# El código incluye <SDL2/SDL.h> y <SDL2/SDL_mixer.h>: carpeta puente con ese prefijo.
set(SHIM ${CMAKE_BINARY_DIR}/shim)
file(GLOB SDL_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/SDL/include/*.h)
file(COPY ${SDL_HEADERS} ${CMAKE_CURRENT_SOURCE_DIR}/SDL_mixer/include/SDL_mixer.h DESTINATION ${SHIM}/SDL2)
include_directories(${SHIM} ${SHIM}/SDL2)

# Boost (solo cabeceras) para dynarmic
set(Boost_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/boost-inc CACHE PATH "" FORCE)
set(Boost_NO_SYSTEM_PATHS ON)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)

# El núcleo (el mismo que en PC) compilado como libmain.so
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../host gdhost)
