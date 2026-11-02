#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace host {
uint32_t jni_vm();
uint32_t jni_env();
uint32_t jni_native(const std::string& key);  // "clase/Java.metodo" -> función registrada

// Implementación en C++ de un método Java: recibe this (o 0) y los argumentos crudos.
// Para floats, el argumento/valor de retorno son los bits del float.
using JMethod = std::function<uint64_t(uint32_t self, const std::vector<uint64_t>& a)>;
void java_method(const std::string& desc, JMethod m);  // desc: "clase/X.metodo (firma)"

// Arrays Java en memoria del invitado: [longitud][relleno][datos...]
uint32_t new_java_array(uint32_t len, uint32_t elem_size);
inline uint32_t java_array_data(uint32_t arr) { return arr + 8; }
}  // namespace host
