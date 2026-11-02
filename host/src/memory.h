// Espacio de memoria del invitado (32 bits) dentro del proceso host.
// Una dirección de invitado es un offset dentro de una reserva de 4 GB.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace mem {

extern uint8_t* base;

// Zonas fijas del espacio del invitado.
constexpr uint32_t kNullGuard = 0x00010000;  // [0, 64K) sin acceso: detecta punteros nulos
constexpr uint32_t kLibBase = 0x01000000;    // librerías cargadas
constexpr uint32_t kHeapBase = 0x20000000;   // heap del invitado
constexpr uint32_t kHeapEnd = 0xE0000000;
constexpr uint32_t kThunkBase = 0xF0000000;  // trampolines hacia funciones del host

void init();
// Windows: confirma memoria reservada antes de usarla (en Linux no hace nada).
void commit(uint32_t a, uint32_t len);

template <class T = void>
inline T* ptr(uint32_t a) { return reinterpret_cast<T*>(base + a); }
inline uint32_t addr(const void* p) {
    return static_cast<uint32_t>(static_cast<const uint8_t*>(p) - base);
}
inline const char* str(uint32_t a) { return a ? ptr<const char>(a) : nullptr; }

inline uint32_t r32(uint32_t a) { uint32_t v; std::memcpy(&v, base + a, 4); return v; }
inline void w32(uint32_t a, uint32_t v) { std::memcpy(base + a, &v, 4); }
inline uint16_t r16(uint32_t a) { uint16_t v; std::memcpy(&v, base + a, 2); return v; }
inline void w16(uint32_t a, uint16_t v) { std::memcpy(base + a, &v, 2); }
inline uint64_t r64(uint32_t a) { uint64_t v; std::memcpy(&v, base + a, 8); return v; }
inline void w64(uint32_t a, uint64_t v) { std::memcpy(base + a, &v, 8); }

// Heap del invitado (thread-safe).
uint32_t alloc(uint32_t size, uint32_t align = 8);
uint32_t zalloc(uint32_t size);
void free(uint32_t a);
uint32_t realloc(uint32_t a, uint32_t size);
uint32_t usable_size(uint32_t a);
uint32_t strdup(const char* s);
uint32_t memdup(const void* p, uint32_t n);

// Reserva rango para cargar una librería (alineado a 64K).
uint32_t reserve_lib(uint32_t size);
// Zona entre librerías: ¿libre? (para mmap con dirección sugerida)
bool lib_range_free(uint32_t a, uint32_t len);
void claim_lib_range(uint32_t a, uint32_t len);

}  // namespace mem
