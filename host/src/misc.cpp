// setjmp/longjmp, semáforos, sscanf y otras funciones sueltas.
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "log.h"
#include "platform.h"
#include "thunks.h"

namespace host {

namespace {

// ---------- setjmp / longjmp ----------
// Guardamos r4-r11, sp, lr y d8-d15 (s16-s31) en el jmp_buf del invitado.
void do_setjmp(Cpu& c) {
    uint32_t env = arg(c, 0);
    for (int i = 0; i < 8; i++) mem::w32(env + i * 4, cpu::reg(c, 4 + i));
    mem::w32(env + 32, cpu::reg(c, 13));
    mem::w32(env + 36, cpu::reg(c, 14));
    uint32_t* ext = cpu::ext_regs(c);
    for (int i = 0; i < 16; i++) mem::w32(env + 40 + i * 4, ext[16 + i]);
    ret(c, 0);
}
void do_longjmp(Cpu& c) {
    uint32_t env = arg(c, 0), val = arg(c, 1);
    for (int i = 0; i < 8; i++) cpu::reg(c, 4 + i) = mem::r32(env + i * 4);
    cpu::reg(c, 13) = mem::r32(env + 32);
    cpu::reg(c, 14) = mem::r32(env + 36);  // el trampolín hace "bx lr" -> vuelve tras setjmp
    uint32_t* ext = cpu::ext_regs(c);
    for (int i = 0; i < 16; i++) ext[16 + i] = mem::r32(env + 40 + i * 4);
    ret(c, val ? val : 1);
}

// ---------- semáforos ----------
struct Sem {
    std::mutex m;
    std::condition_variable cv;
    uint32_t count = 0;
};
std::mutex g_sem_lock;
std::map<uint32_t, std::unique_ptr<Sem>> g_sems;
Sem& sem(uint32_t a) {
    std::lock_guard<std::mutex> lk(g_sem_lock);
    auto& p = g_sems[a];
    if (!p) p = std::make_unique<Sem>();
    return *p;
}

// ---------- sscanf ----------
int guest_sscanf(const char* in, const char* fmt, VaArgs& va) {
    const char* s = in;
    int assigned = 0;
    bool any_input = false;
    for (const char* f = fmt; *f;) {
        if (std::isspace(uint8_t(*f))) {
            while (std::isspace(uint8_t(*s))) s++;
            f++;
            continue;
        }
        if (*f != '%' || f[1] == '%') {
            if (*f == '%') f++;
            if (*s != *f) break;
            s++;
            f++;
            continue;
        }
        f++;
        bool suppress = false;
        if (*f == '*') { suppress = true; f++; }
        std::string width;
        while (std::isdigit(uint8_t(*f))) width += *f++;
        int lng = 0;  // -2 hh, -1 h, 0, 1 l, 2 ll/L
        while (*f && std::strchr("hlLqjzt", *f)) {
            if (*f == 'h') lng = lng == -1 ? -2 : -1;
            else if (*f == 'l') lng = lng == 1 ? 2 : 1;
            else if (*f == 'L' || *f == 'q') lng = 2;
            f++;
        }
        char conv = *f++;
        if (conv != 'c' && conv != '[' && conv != 'n')
            while (std::isspace(uint8_t(*s))) s++;
        if (!*s && conv != 'n') return any_input || assigned ? assigned : -1;
        int used = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            long long v = 0;
            std::string spec = "%" + width + "ll" + (conv == 'u' || conv == 'X' ? (conv == 'X' ? 'x' : 'u') : conv) + "%n";
            if (std::sscanf(s, spec.c_str(), &v, &used) != 1) return assigned;
            if (!suppress) {
                uint32_t p = va.next32();
                if (lng == -2) *mem::ptr<uint8_t>(p) = uint8_t(v);
                else if (lng == -1) mem::w16(p, uint16_t(v));
                else if (lng == 2) mem::w64(p, uint64_t(v));
                else mem::w32(p, uint32_t(v));
                assigned++;
            }
            break;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
            double v = 0;
            std::string spec = "%" + width + "lf%n";
            if (std::sscanf(s, spec.c_str(), &v, &used) != 1) return assigned;
            if (!suppress) {
                uint32_t p = va.next32();
                if (lng >= 1) { uint64_t b; std::memcpy(&b, &v, 8); mem::w64(p, b); }
                else { float fv = float(v); uint32_t b; std::memcpy(&b, &fv, 4); mem::w32(p, b); }
                assigned++;
            }
            break;
        }
        case 's': case '[': {
            std::string spec = "%" + width;
            if (conv == '[') {
                spec += '[';
                if (*f == '^') spec += *f++;
                if (*f == ']') spec += *f++;
                while (*f && *f != ']') spec += *f++;
                if (*f == ']') f++;
                spec += ']';
            } else {
                spec += 's';
            }
            spec += "%n";
            std::string buf(std::strlen(s) + 1, 0);
            if (std::sscanf(s, spec.c_str(), buf.data(), &used) != 1) return assigned;
            if (!suppress) {
                uint32_t p = va.next32();
                std::memcpy(mem::ptr(p), buf.c_str(), std::strlen(buf.c_str()) + 1);
                assigned++;
            }
            break;
        }
        case 'c': {
            int w = width.empty() ? 1 : std::stoi(width);
            if (int(std::strlen(s)) < w) return assigned;
            if (!suppress) { std::memcpy(mem::ptr(va.next32()), s, w); assigned++; }
            used = w;
            break;
        }
        case 'n':
            if (!suppress) mem::w32(va.next32(), uint32_t(s - in));
            break;
        default:
            return assigned;
        }
        s += used;
        any_input = true;
    }
    return assigned;
}

}  // namespace

void register_misc() {
    for (const char* n : {"setjmp", "_setjmp", "sigsetjmp"}) def(n, do_setjmp);
    for (const char* n : {"longjmp", "_longjmp", "siglongjmp"}) def(n, do_longjmp);

    DEF("sem_init") { sem(arg(c, 0)).count = arg(c, 2); ret(c, 0); });
    DEF("sem_destroy") { ret(c, 0); });
    DEF("sem_post") {
        Sem& s = sem(arg(c, 0));
        { std::lock_guard<std::mutex> lk(s.m); s.count++; }
        s.cv.notify_one();
        ret(c, 0);
    });
    DEF("sem_wait") {
        Sem& s = sem(arg(c, 0));
        std::unique_lock<std::mutex> lk(s.m);
        s.cv.wait(lk, [&] { return s.count > 0; });
        s.count--;
        ret(c, 0);
    });
    DEF("sem_trywait") {
        Sem& s = sem(arg(c, 0));
        std::lock_guard<std::mutex> lk(s.m);
        if (!s.count) { set_errno(11); ret(c, uint32_t(-1)); return; }
        s.count--;
        ret(c, 0);
    });

    DEF("sscanf") { auto va = VaArgs::from_regs(c, 2); ret(c, uint32_t(guest_sscanf(args(c, 0), args(c, 1), va))); });
    DEF("vsscanf") { auto va = VaArgs::from_list(arg(c, 2)); ret(c, uint32_t(guest_sscanf(args(c, 0), args(c, 1), va))); });

    DEF("__fpclassifyd") { ret(c, uint32_t(std::fpclassify(argd(c, 0)))); });
    DEF("__fpclassifyf") { ret(c, uint32_t(std::fpclassify(argf(c, 0)))); });
    DEF("alarm") { ret(c, 0); });
    DEF("getopt") { ret(c, uint32_t(-1)); });
    DEF("getpwuid") { ret(c, 0); });
    DEF("gmtime") {
        static thread_local uint32_t buf = 0;
        if (!buf) buf = mem::zalloc(44);
        std::tm tmv;
        px::gmtime(int32_t(mem::r32(arg(c, 0))), tmv);
        int32_t v[9] = {tmv.tm_sec, tmv.tm_min, tmv.tm_hour, tmv.tm_mday, tmv.tm_mon, tmv.tm_year, tmv.tm_wday, tmv.tm_yday, tmv.tm_isdst};
        std::memcpy(mem::ptr(buf), v, 36);
        ret(c, buf);
    });
}

}  // namespace host
