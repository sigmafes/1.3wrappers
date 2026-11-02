#include "cpu.h"

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>

#include "log.h"
#include "memory.h"

namespace cpu {

std::function<std::string(uint32_t)> g_symbolizer;

namespace {

constexpr uint32_t kRetSwi = 0xFFFFFF;  // trampolín especial: fin de una llamada host->invitado
// "svc #0" del propio invitado es una llamada directa al kernel Linux (número en r7).
constexpr size_t kMaxProcessors = 1024;

Dynarmic::ExclusiveMonitor g_monitor(kMaxProcessors);
std::atomic<uint32_t> g_next_proc{0};

std::mutex g_thunk_lock;
Handler g_syscall;
std::vector<std::pair<std::string, Handler>> g_thunks;
std::atomic<uint32_t> g_thunk_count{0};

// GD_PROFILE=1: cada 5 s, las funciones del sistema más llamadas y el tiempo que se lleva cada una.
bool profile_on() {
    static const bool on = std::getenv("GD_PROFILE") != nullptr;
    return on;
}
struct ProfEntry {
    std::atomic<uint64_t> calls{0}, ns{0};
};
ProfEntry g_prof[1 << 16];
std::atomic<uint64_t> g_prof_svc{0};
void profile_add(uint32_t swi, int64_t ns) {
    g_prof[swi].calls.fetch_add(1, std::memory_order_relaxed);
    g_prof[swi].ns.fetch_add(uint64_t(ns), std::memory_order_relaxed);
}

std::mutex g_cpus_lock;
std::set<Cpu*> g_all_cpus;

bool trace_on() {  // se lee la primera vez que hace falta (en Android main lo activa antes)
    static const bool on = std::getenv("HOST_TRACE") != nullptr;
    return on;
}

struct ThreadState {
    uint32_t stack_top = 0;
    uint32_t tls = 0;
    std::vector<std::unique_ptr<Cpu>> cpus;
    size_t depth = 0;
};
thread_local ThreadState* t_state = nullptr;
ThreadState* g_main_state = nullptr;  // primer hilo que ejecuta el juego

// CP15: solo TPIDRURO (registro TLS de usuario) es necesario.
using Dynarmic::A32::CoprocReg;
class Cp15 final : public Dynarmic::A32::Coprocessor {
public:
    explicit Cp15(uint32_t* tls) : tls_(tls) {}
    std::optional<Callback> CompileInternalOperation(bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override { return std::nullopt; }
    CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned, CoprocReg, CoprocReg, unsigned) override { return std::monostate{}; }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm, unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == CoprocReg::C13 && CRm == CoprocReg::C0 && opc2 == 3) return tls_;
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }
    std::optional<Callback> CompileStoreWords(bool, bool, CoprocReg, std::optional<std::uint8_t>) override { return std::nullopt; }

private:
    uint32_t* tls_;
};

}  // namespace

[[noreturn]] void fatal_guest(Cpu& c, const char* what, uint32_t addr);

struct Cpu final : Dynarmic::A32::UserCallbacks {
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    bool returned = false;
    uint32_t tls_word = 0;

    Cpu(uint32_t tls) : tls_word(tls) {
        Dynarmic::A32::UserConfig cfg;
        cfg.callbacks = this;
        cfg.processor_id = g_next_proc++;
        cfg.global_monitor = &g_monitor;
        if (!std::getenv("HOST_WATCH") && !std::getenv("HOST_WATCHR")) cfg.fastmem_pointer = reinterpret_cast<uintptr_t>(mem::base);
        cfg.enable_cycle_counting = false;
        // Velocidad: coma flotante algo menos estricta (los emuladores lo usan; el juego no lo nota)
        if (!std::getenv("GD_SAFE_FP")) {
            cfg.unsafe_optimizations = true;
            cfg.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA | Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                                 Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN | Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
        }
        cfg.coprocessors[15] = std::make_shared<Cp15>(&tls_word);
        jit = std::make_unique<Dynarmic::A32::Jit>(cfg);
        std::lock_guard<std::mutex> lk(g_cpus_lock);
        g_all_cpus.insert(this);
    }

    void check(uint32_t a) {
        if (a < mem::kNullGuard) fatal_guest(*this, "acceso a puntero nulo", a);
    }
    // HOST_WATCH=dir: registra las escrituras en esa dirección (desactiva fastmem).
    void watch(uint32_t a, uint64_t v, int size) {
        static const uint32_t w = std::getenv("HOST_WATCH") ? uint32_t(std::strtoul(std::getenv("HOST_WATCH"), nullptr, 16)) : 0;
        if (w && a <= w && w < a + size) {
            auto& r = jit->Regs();
            logf("[watch] escritura %08x <- %llx  pc=%08x %s  lr=%08x %s", a, (unsigned long long)v, r[15],
                 g_symbolizer ? g_symbolizer(r[15]).c_str() : "", r[14], g_symbolizer ? g_symbolizer(r[14]).c_str() : "");
        }
    }
    std::uint8_t MemoryRead8(std::uint32_t a) override { check(a); return *mem::ptr<uint8_t>(a); }
    std::uint16_t MemoryRead16(std::uint32_t a) override { check(a); return mem::r16(a); }
    std::uint32_t MemoryRead32(std::uint32_t a) override {
        check(a);
        static const uint32_t w = std::getenv("HOST_WATCHR") ? uint32_t(std::strtoul(std::getenv("HOST_WATCHR"), nullptr, 16)) : 0;
        if (w && a >= w && a < w + 0x200) {  // HOST_WATCHR=dir: quién lee esas palabras
            auto& r = jit->Regs();
            logf("[watchr] lectura %08x pc=%08x lr=%08x r0-3=%08x %08x %08x %08x", a, r[15], r[14], r[0], r[1], r[2], r[3]);
            for (int i = 0; i < 64; i++) { uint32_t v = mem::r32(r[13] + 4 * i); if (v > 0x015f0000 && v < 0x01b00000 && (v & 1)) logf("[watchr]   pila+%x: %08x (+%x)", 4 * i, v, v - 0x015f0000); }
        }
        return mem::r32(a);
    }
    std::uint64_t MemoryRead64(std::uint32_t a) override { check(a); return mem::r64(a); }
    void MemoryWrite8(std::uint32_t a, std::uint8_t v) override { check(a); watch(a, v, 1); *mem::ptr<uint8_t>(a) = v; }
    void MemoryWrite16(std::uint32_t a, std::uint16_t v) override { check(a); watch(a, v, 2); mem::w16(a, v); }
    void MemoryWrite32(std::uint32_t a, std::uint32_t v) override { check(a); watch(a, v, 4); mem::w32(a, v); }
    void MemoryWrite64(std::uint32_t a, std::uint64_t v) override { check(a); watch(a, v, 8); mem::w64(a, v); }

    template <class T>
    bool cas(std::uint32_t a, T v, T expected) {
        check(a);
        return __atomic_compare_exchange_n(mem::ptr<T>(a), &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    bool MemoryWriteExclusive8(std::uint32_t a, std::uint8_t v, std::uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(std::uint32_t a, std::uint16_t v, std::uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(std::uint32_t a, std::uint32_t v, std::uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(std::uint32_t a, std::uint64_t v, std::uint64_t e) override { return cas(a, v, e); }

    void InterpreterFallback(std::uint32_t pc, size_t n) override {
        fatal_guest(*this, "instrucción no soportada por el JIT", pc);
    }
    void CallSVC(std::uint32_t swi) override {
        if (swi == kRetSwi) {
            returned = true;
            jit->HaltExecution();
            return;
        }
        if (swi == 0) {
            if (trace_on()) logf("[trace] syscall %u (lr=%08x)", jit->Regs()[7], jit->Regs()[14]);
            if (!g_syscall) fatal_guest(*this, "syscall sin manejador", jit->Regs()[7]);
            g_syscall(*this);
            return;
        }
        // Sin bloqueo: la tabla tiene la capacidad reservada desde el principio y nunca se mueve.
        if (swi >= g_thunk_count.load(std::memory_order_acquire)) fatal_guest(*this, "SVC desconocido", swi);
        auto& t = g_thunks[swi];
        if (trace_on()) logf("[trace] %s (lr=%08x)", t.first.c_str(), jit->Regs()[14]);
        static const char* bt_name = std::getenv("HOST_BT");  // HOST_BT=función: pila de las primeras llamadas
        if (bt_name && t.first == bt_name) {
            static std::atomic<long> seen{0};
            static const long skip = std::getenv("HOST_BT_SKIP") ? std::atol(std::getenv("HOST_BT_SKIP")) : 0;
            long k = seen.fetch_add(1);
            if (k >= skip && k < skip + 6) {
                std::string st;
                uint32_t sp = jit->Regs()[13];
                for (int i = 0; i < 1024; i++) {
                    uint32_t v = mem::r32(sp + 4 * i);
                    if ((v & 1) && v > 0x01000000 && v < 0x01b60000 && g_symbolizer) st += "\n    " + g_symbolizer(v - 1);
                }
                logf("[bt] %s lr=%s%s", bt_name, g_symbolizer ? g_symbolizer(jit->Regs()[14]).c_str() : "", st.c_str());
            }
        }
        if (profile_on()) {
            auto t0 = std::chrono::steady_clock::now();
            t.second(*this);
            profile_add(swi, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            return;
        }
        t.second(*this);
    }
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception e) override {
        using E = Dynarmic::A32::Exception;
        switch (e) {
        case E::SendEvent: case E::SendEventLocal: case E::WaitForInterrupt:
        case E::WaitForEvent: case E::Yield: case E::PreloadData:
        case E::PreloadDataWithIntentToWrite: case E::PreloadInstruction:
            jit->Regs()[15] = pc + ((jit->Cpsr() & 0x20) ? 2 : 4);  // continuar
            return;
        default: {
            logf("[cpu] excepción %d en %08x (thumb=%d) bytes=%08x %08x", int(e), pc, (jit->Cpsr() >> 5) & 1,
                 mem::r32(pc & ~1u), mem::r32((pc & ~1u) + 4));
            fatal_guest(*this, "excepción del CPU invitado", pc);
        }
        }
    }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000; }
};

Cpu* g_last_cpu = nullptr;

std::string backtrace(Cpu& c, int depth);

[[noreturn]] void fatal_guest(Cpu& c, const char* what, uint32_t addr) {
    auto& r = c.jit->Regs();
    logf("[FATAL] %s (dir=%08x)", what, addr);
    logf("  pc=%08x %s", r[15], g_symbolizer ? g_symbolizer(r[15]).c_str() : "");
    logf("  lr=%08x %s", r[14], g_symbolizer ? g_symbolizer(r[14]).c_str() : "");
    for (int i = 0; i < 13; i += 4)
        logf("  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
    logf("  cadena de llamadas:%s", backtrace(c, 12).c_str());
    // Direcciones de código guardadas en la pila (pistas de quién llamó).
    if (g_symbolizer) {
        int found = 0;
        for (uint32_t sp = r[13]; sp < r[13] + 1024 && found < 16; sp += 4) {
            uint32_t v = mem::r32(sp);
            if (v < 0x01000000 || v >= 0x02000000) continue;
            std::string sym = g_symbolizer(v);
            if (sym.empty()) continue;
            logf("  pila+%03x: %08x %s", sp - r[13], v, sym.c_str());
            found++;
        }
    }
    std::fflush(stdout);
    std::_Exit(3);
}

void set_symbolizer(std::function<std::string(uint32_t)> f) { g_symbolizer = std::move(f); }

uint32_t& reg(Cpu& c, int i) { return c.jit->Regs()[i]; }
uint32_t* ext_regs(Cpu& c) { return c.jit->ExtRegs().data(); }
uint32_t get_cpsr(Cpu& c) { return c.jit->Cpsr(); }
void set_cpsr(Cpu& c, uint32_t v) { c.jit->SetCpsr(v); }

void thread_init(uint32_t stack_size) {
    if (t_state) return;
    t_state = new ThreadState;
    if (!g_main_state) g_main_state = t_state;
    uint32_t stack = mem::alloc(stack_size, 16);
    t_state->stack_top = (stack + stack_size) & ~15u;
    t_state->tls = mem::zalloc(1024);
}

uint32_t tls_base() {
    thread_init();
    return t_state->tls;
}

uint64_t call(uint32_t fn, const std::vector<uint32_t>& args) {
    thread_init();
    ThreadState& t = *t_state;
    if (t.depth == t.cpus.size()) t.cpus.push_back(std::make_unique<Cpu>(t.tls));
    Cpu& c = *t.cpus[t.depth];
    uint32_t sp = t.depth == 0 ? t.stack_top : ((t.cpus[t.depth - 1]->jit->Regs()[13] - 256) & ~7u);
    t.depth++;

    auto& r = c.jit->Regs();
    size_t extra = args.size() > 4 ? args.size() - 4 : 0;
    sp = (sp - extra * 4) & ~7u;
    for (size_t i = 0; i < extra; i++) mem::w32(sp + i * 4, args[4 + i]);
    for (size_t i = 0; i < 4; i++) r[i] = i < args.size() ? args[i] : 0;
    r[13] = sp;
    r[14] = mem::kThunkBase;  // trampolín de retorno
    r[15] = fn & ~1u;
    c.jit->SetCpsr(0x10 | ((fn & 1) ? 0x20 : 0));
    c.returned = false;
    g_last_cpu = &c;
    while (!c.returned) {
        c.jit->ClearHalt(Dynarmic::HaltReason::UserDefined1);
        c.jit->Run();
    }
    uint64_t res = r[0] | (uint64_t(r[1]) << 32);
    t.depth--;
    g_last_cpu = t.depth ? t.cpus[t.depth - 1].get() : nullptr;
    return res;
}

void invalidate(uint32_t addr, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_cpus_lock);
    for (Cpu* c : g_all_cpus) c->jit->InvalidateCacheRange(addr, len);
}

void profile_report() {
    if (!profile_on()) return;
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    std::vector<std::pair<uint64_t, uint32_t>> v;
    uint64_t total_calls = 0, total_ns = 0;
    uint32_t n = g_thunk_count.load();
    for (uint32_t i = 0; i < n; i++) {
        uint64_t c = g_prof[i].calls.exchange(0), ns = g_prof[i].ns.exchange(0);
        total_calls += c;
        total_ns += ns;
        if (c) v.push_back({ns, i});
        g_prof[i].calls.store(c);  // para imprimir abajo
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    logf("[perf] %.0f llamadas/s al sistema, %.1f%% del tiempo dentro de ellas", total_calls / secs, total_ns / (secs * 1e7));
    for (size_t k = 0; k < v.size() && k < 15; k++) {
        uint32_t i = v[k].second;
        logf("[perf]   %9.0f/s %6.2f%%  %s", g_prof[i].calls.load() / secs, v[k].first / (secs * 1e7), g_thunks[i].first.c_str());
    }
    for (uint32_t i = 0; i < n; i++) g_prof[i].calls.store(0);
}

uint32_t add_thunk(const std::string& name, Handler h) {
    std::lock_guard<std::mutex> lk(g_thunk_lock);
    if (g_thunks.empty()) {
        g_thunks.reserve(1 << 16);
        g_thunks.emplace_back("__return", nullptr);
        mem::w32(mem::kThunkBase + 0, 0xEF000000 | kRetSwi);  // svc #0xFFFFFF
        mem::w32(mem::kThunkBase + 4, 0xE12FFF1E);  // bx lr
    }
    uint32_t idx = static_cast<uint32_t>(g_thunks.size());
    if (idx >= g_thunks.capacity()) { logf("[cpu] demasiados thunks"); std::abort(); }
    g_thunks.emplace_back(name, std::move(h));
    g_thunk_count.store(idx + 1, std::memory_order_release);
    uint32_t a = mem::kThunkBase + idx * 8;
    mem::w32(a, 0xEF000000 | idx);  // svc #idx (ARM)
    mem::w32(a + 4, 0xE12FFF1E);    // bx lr
    return a;
}

const std::string& thunk_name(uint32_t swi) { return g_thunks.at(swi).first; }
void set_syscall_handler(Handler h) { g_syscall = std::move(h); }

std::string backtrace(Cpu& c, int depth);

// Estado del CPU del hilo principal (para el detector de cuelgues).
void dump_main_thread() {
    ThreadState* t = g_main_state;
    if (!t || t->depth == 0) { logf("[cuelgue] el hilo principal no está ejecutando código del juego"); return; }
    Cpu& c = *t->cpus[t->depth - 1];
    auto& r = c.jit->Regs();
    logf("[cuelgue] pc=%08x %s", r[15], g_symbolizer ? g_symbolizer(r[15]).c_str() : "");
    logf("[cuelgue] lr=%08x %s", r[14], g_symbolizer ? g_symbolizer(r[14]).c_str() : "");
    logf("[cuelgue] r0=%08x r1=%08x r2=%08x r3=%08x%s", r[0], r[1], r[2], r[3], backtrace(c, 12).c_str());
}

void dump_last_cpu() {
    if (!g_last_cpu) return;
    auto& r = g_last_cpu->jit->Regs();
    logf("[watchdog] pc=%08x %s", r[15], g_symbolizer ? g_symbolizer(r[15]).c_str() : "");
    logf("[watchdog] lr=%08x %s", r[14], g_symbolizer ? g_symbolizer(r[14]).c_str() : "");
    for (int i = 0; i < 13; i += 4)
        logf("  r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
}

std::string backtrace(Cpu& c, int depth) {
    std::string out;
    uint32_t fp = c.jit->Regs()[7];
    for (int i = 0; i < depth && fp >= mem::kNullGuard && fp < 0xF0000000; i++) {
        uint32_t lr = mem::r32(fp + 4);
        out += "\n    #" + std::to_string(i) + " " + (g_symbolizer ? g_symbolizer(lr) : std::to_string(lr));
        uint32_t next = mem::r32(fp);
        if (next <= fp) break;
        fp = next;
    }
    return out;
}

uint32_t current_pc() {
    if (!t_state || t_state->depth == 0) return 0;
    return t_state->cpus[t_state->depth - 1]->jit->Regs()[15];
}

}  // namespace cpu
