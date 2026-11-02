// CPU ARM32 del invitado sobre dynarmic.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cpu {

struct Cpu;

// Registros del CPU activo (el que está ejecutando la llamada al host actual).
uint32_t& reg(Cpu& c, int i);
uint32_t* ext_regs(Cpu& c);  // s0..s63 (VFP)
uint32_t get_cpsr(Cpu& c);
void set_cpsr(Cpu& c, uint32_t v);

// Llama a una función del invitado y devuelve r0 | (r1 << 32).
// Se puede usar de forma anidada (host -> invitado -> host -> invitado...).
uint64_t call(uint32_t fn, const std::vector<uint32_t>& args = {});

// Debe llamarse desde cada hilo nativo nuevo antes de usar call().
void thread_init(uint32_t stack_size = 1u << 20);

// Invalida la caché de código JIT (cuando el invitado reescribe código).
void invalidate(uint32_t addr, uint32_t len);

// Trampolines: cada función del host tiene una dirección en el invitado.
using Handler = std::function<void(Cpu&)>;
uint32_t add_thunk(const std::string& name, Handler h);
void profile_report();  // GD_PROFILE=1: resumen periódico de llamadas al sistema
const std::string& thunk_name(uint32_t swi);
// Manejador de "svc #0" (syscalls directas del invitado; número en r7, args en r0-r5).
void set_syscall_handler(Handler h);

// Información para depuración.
uint32_t current_pc();
std::string backtrace(Cpu& c, int depth = 8);  // cadena de r7 (frame pointer Thumb)

// Dirección de la zona TLS del hilo actual (TPIDRURO).
uint32_t tls_base();

}  // namespace cpu
