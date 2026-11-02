# 13GDPS

Port de **1.3 GDPS** para Windows y Android (64 bits).

El programa carga las librerías ARM del APK original (`libgame.so`, `libfmodL.so` y `libascella.so`) y las ejecuta con un recompilador (dynarmic). Lo que el juego le pide a Android (archivos, OpenGL ES, audio, red, Java) lo resuelve el propio programa. El APK del juego no se incluye; cada jugador usa el suyo.

## Usarlo en Windows

1. Descomprime `13GDPS.zip`.
2. Pon el APK de 1.3 GDPS en la carpeta `13GDPS`, junto a `13GDPS.exe`.
3. Abre `13GDPS.exe`.

La partida se guarda en `data`.

Controles:
- Ratón: tocar.
- Espacio o flecha arriba: saltar.
- Esc: atrás.
- F11 o Alt+Enter: pantalla completa.

## Compilar

Requisitos: CMake, Ninja y un compilador de C++20.

- **Linux:**
  ```
  cmake -S host -B host/build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build host/build
  ./host/build/gdhost ruta/al/juego.apk
  ```
- **Windows** (compilación cruzada desde Linux con MinGW):
  ```
  host/tools/preparar_windows.sh
  host/tools/empaquetar_windows.sh
  ```
  El resultado queda en `host/dist/13GDPS.zip`.
- **Android:** con GitHub Actions (`.github/workflows/android.yml`).
  - Sube el APK del juego como archivo de una release con la etiqueta `apk`.
  - Para firmar siempre con la misma clave, añade los secretos `ANDROID_KEYSTORE_B64` y `ANDROID_KEYSTORE_PASS`. Sin ellos se usa una clave de pruebas.

### Variables útiles

| Variable | Qué hace |
|---|---|
| `GD_LOG=1` (o `--log`) | escribe `data/log.txt` |
| `GD_DATA=carpeta` | carpeta de datos |
| `GD_SIZE=1280x720` | tamaño de la ventana |
| `GD_HEADLESS=1`, `GD_FRAMES=n`, `GD_SCRIPT=...` | ejecuciones sin ventana para pruebas |
| `GD_HOSTS=dominio=ip,...` | redirigir dominios |

## Estructura

```
host/          núcleo (CPU, cargador ELF, libc, OpenGL, JNI, audio, red) y extras del juego
  src/songs.cpp   extras de 1.3 GDPS (canciones, colores, Streak...)
  external/       dynarmic y sus dependencias
android/       proyecto de Android (SDL2 + el núcleo como libmain.so)
```

## Créditos

- [dynarmic](https://github.com/azahar-emu/dynarmic) (0BSD)
- [SDL2 y SDL2_mixer](https://libsdl.org) (zlib)
- [stb](https://github.com/nothings/stb) (dominio público / MIT)
- Geometry Dash es de RobTop Games. 1.3 GDPS y Ascella son de sus respectivos autores.
