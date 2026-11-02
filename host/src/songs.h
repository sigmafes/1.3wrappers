// Canciones de Newgrounds: descarga, títulos y selección por ID en el editor.
#pragma once
#include <string>

namespace songs {
constexpr int kFirstNG = 1000;  // números de canción desde aquí = ID de Newgrounds
void set_dir(const std::string& dir);
void set_fps_hooks(int (*get)(), void (*cycle)());  // botón "FPS Bypass" en Opciones
void install();                  // tras JNI_OnLoad de Ascella
bool editor_open();              // ¿ajustes del nivel abiertos en el editor?
bool key(int ch);
void touch(bool down, float fx, float fy);  // pulsación (fracción de la pantalla)
void trace_player(int frame);   // GD_TRACE_PLAYER=archivo: física del jugador por fotograma
void on_http(const std::string& url, const std::string& body);  // respuestas del servidor del GDPS
void tick();                     // una vez por fotograma (hilo principal)                // '0'-'9' o '\b'; true si se usó
std::string play_path();          // mp3 de Newgrounds del nivel cargado, o ""
// Ruta real de data/songs/<n> pedida por Ascella (track = n-1).
std::string resolve(const std::string& base_path, int track);
}  // namespace songs
