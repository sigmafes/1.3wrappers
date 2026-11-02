cmake_minimum_required(VERSION 3.16)
project(gdhost C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Traductor ARM32 -> x86-64 / ARM64
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
if(WIN32)
  # Las cabeceras de MinGW no tienen los campos nuevos de topología de CPU que usa xbyak.
  add_compile_definitions(XBYAK_CPU_CACHE=0)
endif()
add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

set(GDHOST_SOURCES
  src/main.cpp
  src/memory.cpp
  src/cpu.cpp
  src/elf_loader.cpp
  src/thunks.cpp
  src/libc.cpp
  src/pthread.cpp
  src/jni.cpp
  src/gl.cpp
  src/zlib.cpp
  src/misc.cpp
  src/apk.cpp
  src/audio.cpp
  src/net.cpp
  src/text.cpp
  src/platform.cpp
  src/songs.cpp
)
if(ANDROID)
  # Android: el programa es libmain.so, que carga SDLActivity (SDL2 y SDL2_mixer vienen del proyecto Android).
  add_library(gdhost SHARED ${GDHOST_SOURCES})
  set_target_properties(gdhost PROPERTIES OUTPUT_NAME main)
else()
  add_executable(gdhost ${GDHOST_SOURCES})
endif()
target_link_libraries(gdhost PRIVATE dynarmic)

if(ANDROID)
  target_link_libraries(gdhost PRIVATE SDL2::SDL2 SDL2_mixer::SDL2_mixer GLESv2 EGL z log android)
elseif(WIN32)
  # Dependencias de Windows (ver tools/preparar_windows.sh): SDL2, SDL2_mixer y zlib para MinGW.
  set(WIN_DEPS "${CMAKE_SOURCE_DIR}/external/win" CACHE PATH "Dependencias de Windows")
  set(SDL2_ROOT "${WIN_DEPS}/SDL2-2.30.9/x86_64-w64-mingw32")
  set(MIXER_ROOT "${WIN_DEPS}/SDL2_mixer-2.8.0/x86_64-w64-mingw32")
  set(ZLIB_ROOT "${WIN_DEPS}/zlib-win")
  target_include_directories(gdhost PRIVATE "${SDL2_ROOT}/include" "${SDL2_ROOT}/include/SDL2" "${MIXER_ROOT}/include" "${ZLIB_ROOT}/include")
  target_link_libraries(gdhost PRIVATE
    "${MIXER_ROOT}/lib/libSDL2_mixer.dll.a"
    "${SDL2_ROOT}/lib/libSDL2.dll.a"
    "${ZLIB_ROOT}/lib/libzlibstatic.a"
    ws2_32 opengl32 winhttp winmm)
  target_link_options(gdhost PRIVATE -mwindows -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
  # Carpeta portable: gdhost.exe + DLLs de SDL
  add_custom_command(TARGET gdhost POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDL2_ROOT}/bin/SDL2.dll" "${MIXER_ROOT}/bin/SDL2_mixer.dll" $<TARGET_FILE_DIR:gdhost>)
else()
  find_package(SDL2 REQUIRED)
  find_package(ZLIB REQUIRED)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2_MIXER REQUIRED IMPORTED_TARGET SDL2_mixer)
  target_link_libraries(gdhost PRIVATE SDL2::SDL2 PkgConfig::SDL2_MIXER ZLIB::ZLIB EGL)
endif()
