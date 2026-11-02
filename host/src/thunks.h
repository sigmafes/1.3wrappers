// Utilidades para implementar funciones del sistema que llama el invitado.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

#include "cpu.h"
#include "memory.h"

namespace host {

using cpu::Cpu;

// Registro de símbolos del host (funciones y variables).
void def(const std::string& name, cpu::Handler h);
void def_data(const std::string& name, uint32_t guest_addr);
uint32_t lookup(const std::string& name);  // 0 si no existe
uint32_t resolve(const std::string& name, bool is_object);  // crea stub si no existe

void register_libc();
void register_pthread();
void register_jni();
void register_gl();
void register_zlib();
void register_misc();
bool gl_init_offscreen(int w, int h);
void gl_swap();
bool gl_load(void* (*getproc)(const char*), bool desktop);
void gl_read_pixels(int w, int h, void* rgba);
void gl_fb_size(int& w, int& h);
bool gl_offscreen_begin(int w, int h, float scale = 1.0f);
void gl_present(int win_w, int win_h, int& x, int& y, int& w, int& h);

// Argumentos según AAPCS (softfp).
inline uint32_t arg(Cpu& c, int i) { return i < 4 ? cpu::reg(c, i) : mem::r32(cpu::reg(c, 13) + 4 * (i - 4)); }
inline uint64_t arg64(Cpu& c, int i) { return arg(c, i) | (uint64_t(arg(c, i + 1)) << 32); }  // i ya alineado
inline double argd(Cpu& c, int i) { uint64_t v = arg64(c, i); double d; std::memcpy(&d, &v, 8); return d; }
inline float argf(Cpu& c, int i) { uint32_t v = arg(c, i); float f; std::memcpy(&f, &v, 4); return f; }
inline const char* args(Cpu& c, int i) { return mem::str(arg(c, i)); }
template <class T = void>
inline T* argp(Cpu& c, int i) { uint32_t a = arg(c, i); return a ? mem::ptr<T>(a) : nullptr; }

inline void ret(Cpu& c, uint32_t v) { cpu::reg(c, 0) = v; }
inline void ret64(Cpu& c, uint64_t v) { cpu::reg(c, 0) = uint32_t(v); cpu::reg(c, 1) = uint32_t(v >> 32); }
inline void retd(Cpu& c, double d) { uint64_t v; std::memcpy(&v, &d, 8); ret64(c, v); }
inline void retf(Cpu& c, float f) { uint32_t v; std::memcpy(&v, &f, 4); ret(c, v); }

// errno del invitado (por hilo).
void set_errno(int e);
uint32_t errno_addr();

// Lector de argumentos variables (registros+pila o va_list en memoria).
struct VaArgs {
    Cpu* c = nullptr;
    int ncrn = 0;        // siguiente registro r0..r3
    uint32_t stack = 0;  // siguiente posición en pila / va_list
    static VaArgs from_regs(Cpu& c, int first) { VaArgs v; v.c = &c; v.ncrn = first; v.stack = cpu::reg(c, 13); return v; }
    static VaArgs from_list(uint32_t ap) { VaArgs v; v.ncrn = 4; v.stack = ap; return v; }
    uint32_t next32() {
        if (ncrn < 4) return cpu::reg(*c, ncrn++);
        uint32_t v = mem::r32(stack);
        stack += 4;
        return v;
    }
    uint64_t next64() {
        if (ncrn < 4) {
            ncrn = (ncrn + 1) & ~1;
            if (ncrn <= 2) {
                uint64_t v = cpu::reg(*c, ncrn) | (uint64_t(cpu::reg(*c, ncrn + 1)) << 32);
                ncrn += 2;
                return v;
            }
            ncrn = 4;
        }
        stack = (stack + 7) & ~7u;
        uint64_t v = mem::r64(stack);
        stack += 8;
        return v;
    }
};

// printf con argumentos del invitado.
std::string format(const char* fmt, VaArgs& va);

// Contenido falso de /proc/self/maps (direcciones del invitado).
void set_maps_provider(std::string (*f)());
// Abre una ruta aplicando rutas virtuales (/proc/self/maps...). Devuelve fd del host.
int open_path(const char* path, int flags, int mode);

}  // namespace host

#define DEF(name) host::def(name, [](host::Cpu & c)
