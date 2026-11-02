// Lector mínimo de ZIP para sacar archivos del APK (assets/...).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace apk {

bool open(const std::string& path);
// Devuelve el contenido de una entrada ("assets/menuLoop.mp3"); vacío si no existe.
std::shared_ptr<std::vector<uint8_t>> read(const std::string& name);
bool exists(const std::string& name);

}  // namespace apk
