// Funciones de libc / libm / runtime de C++ / helpers __aeabi para el invitado.
#include <atomic>
#include <filesystem>
#include <ctime>

#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#include "log.h"
#include "platform.h"
#include "thunks.h"
#include "songs.h"
#ifdef __ANDROID__
#include <unistd.h>
#endif

namespace host {

namespace {

// ---------- errno ----------
thread_local uint32_t t_errno = 0;

// ---------- FILE* del invitado ----------
constexpr uint32_t kFileSize = 84;  // sizeof(FILE) en bionic 32 bits
std::mutex g_files_lock;
std::map<uint32_t, FILE*> g_files;
uint32_t g_sF = 0;

std::atomic<uint32_t> g_files_gen{1};  // cambia en cada fclose: invalida las cachés
FILE* hfile(uint32_t g) {
    // Caché por hilo del último archivo: el lector de ZIP de cocos lee byte a byte del mismo FILE.
    thread_local uint32_t last_g = 0, last_gen = 0;
    thread_local FILE* last_f = nullptr;
    uint32_t gen = g_files_gen.load(std::memory_order_acquire);
    if (g == last_g && gen == last_gen) return last_f;
    std::lock_guard<std::mutex> lk(g_files_lock);
    auto it = g_files.find(g);
    FILE* f = it == g_files.end() ? nullptr : it->second;
    last_g = g;
    last_gen = gen;
    last_f = f;
    return f;
}
uint32_t gfile(FILE* f) {
    if (!f) return 0;
    uint32_t g = mem::zalloc(kFileSize);
    // Campos internos de bionic (BSD): _flags en +12, _file en +14.
    mem::w16(g + 12, 0x0010);
    mem::w16(g + 14, uint16_t(fileno(f)));
    std::lock_guard<std::mutex> lk(g_files_lock);
    g_files[g] = f;
    return g;
}

// ---------- DIR* ----------
struct GDir {
    void* d;
    uint32_t ent;
};
std::map<uint32_t, GDir> g_dirs;

// En Windows los archivos se abren en modo binario siempre.
std::string fmode(const char* m) {
    std::string s = m ? m : "r";
#ifdef _WIN32
    if (s.find('b') == std::string::npos) s += 'b';
#endif
    return s;
}

// struct stat de bionic ARM32 (104 bytes).
void write_stat(uint32_t g, const px::Stat& s) {
    std::memset(mem::ptr(g), 0, 104);
    mem::w64(g + 0, s.dev);
    mem::w32(g + 12, uint32_t(s.ino));
    mem::w32(g + 16, s.mode);
    mem::w32(g + 20, s.nlink);
    mem::w32(g + 24, s.uid);
    mem::w32(g + 28, s.gid);
    mem::w64(g + 32, s.rdev);
    mem::w64(g + 48, uint64_t(s.size));
    mem::w32(g + 56, s.blksize);
    mem::w64(g + 64, s.blocks);
    mem::w32(g + 72, uint32_t(s.atime));
    mem::w32(g + 80, uint32_t(s.mtime));
    mem::w32(g + 88, uint32_t(s.ctime));
    mem::w64(g + 96, s.ino);
}

// struct tm de bionic 32 bits: 9 int + long gmtoff + char* zone (44 bytes).
void tm_to_guest(uint32_t g, const struct tm& t) {
    int32_t v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    std::memcpy(mem::ptr(g), v, 36);
#ifdef _WIN32
    mem::w32(g + 36, 0);
#else
    mem::w32(g + 36, uint32_t(t.tm_gmtoff));
#endif
    mem::w32(g + 40, 0);
}
struct tm tm_from_guest(uint32_t g) {
    struct tm t{};
    int32_t v[9];
    std::memcpy(v, mem::ptr(g), 36);
    t.tm_sec = v[0]; t.tm_min = v[1]; t.tm_hour = v[2]; t.tm_mday = v[3]; t.tm_mon = v[4];
    t.tm_year = v[5]; t.tm_wday = v[6]; t.tm_yday = v[7]; t.tm_isdst = v[8];
    return t;
}

// Para funciones de la CRT (fopen, remove...): errno del host.
uint32_t ret_errno(Cpu& c, long r) {
    if (r < 0) set_errno(errno);
    ret(c, uint32_t(r));
    return uint32_t(r);
}
// Para funciones de px:: (ya traducen el error a la numeración de Linux).
uint32_t ret_px(Cpu& c, long r) {
    if (r < 0) set_errno(px::last_error());
    ret(c, uint32_t(r));
    return uint32_t(r);
}



// Rutas de Android -> carpetas del PC (dentro de GD_DATA).
std::string vpath_str(const char* p) {
    if (!p) return "";
    static const std::string data = [] {
        const char* d = std::getenv("GD_DATA");
        return std::string(d ? d : "data");
    }();
    static const std::pair<const char*, const char*> maps[] = {
        // Canciones personalizadas: Ascella busca <almacenamiento>/13gdps/songs/<n>.
        {"/storage/emulated/0/13gdps/songs", "/songs"},
        {"/sdcard/13gdps/songs", "/songs"},
        {"/data/data/app.onepointthree.starry_sky", "/data"},
        {"/data/user/0/app.onepointthree.starry_sky", "/data"},
        {"/storage/emulated/0", "/sdcard"},
        {"/mnt/sdcard", "/sdcard"},
        {"/sdcard", "/sdcard"},
    };
    std::string s(p);
    for (auto& [from, to] : maps) {
        size_t n = std::strlen(from);
        if (s.compare(0, n, from) != 0 || (s.size() != n && s[n] != '/')) continue;
#ifdef __ANDROID__
        // Con "Acceso a todos los archivos" concedido se usa el almacenamiento real del móvil.
        static const bool real_sd = ::access("/storage/emulated/0", W_OK) == 0;
        if (real_sd && std::strcmp(to, "/sdcard") == 0)
            return "/storage/emulated/0" + s.substr(n);
#endif
        std::string r = data + to + s.substr(n);
        // data/songs/5 -> también vale 5.mp3, 5.ogg o 5.wav
        if (std::strcmp(to, "/songs") == 0 && s.size() > n + 1 && s.find('.', n) == std::string::npos) {
            std::string ng = songs::play_path();
            if (!ng.empty()) return ng;
            int track = std::atoi(s.c_str() + n + 1) - 1;
            if (track >= songs::kFirstNG) return songs::resolve(r, track);
            std::error_code ec;
            if (!std::filesystem::exists(r, ec))
                for (const char* ext : {".mp3", ".ogg", ".wav"})
                    if (std::filesystem::exists(r + ext, ec)) return r + ext;
        }
        return r;
    }
    return s;
}
// Devuelve un char* válido hasta unas cuantas llamadas después (buffer rotatorio por hilo).
const char* VP(Cpu& c, int i) {
    static thread_local std::string ring[4];
    static thread_local int idx = 0;
    if (!arg(c, i)) return nullptr;
    std::string& s = ring[idx++ & 3];
    s = vpath_str(args(c, i));
    return s.c_str();
}

std::string (*g_maps_provider)() = nullptr;

bool is_maps_path(const char* p) {
    if (!p) return false;
    std::string s(p);
    return s == "/proc/self/maps" || (s.rfind("/proc/", 0) == 0 && s.size() > 11 && s.compare(s.size() - 5, 5, "/maps") == 0);
}

}  // namespace

void set_maps_provider(std::string (*f)()) { g_maps_provider = f; }

// Archivos de sistema de Linux/Android que el juego (sobre todo FMOD) lee.
// En Windows no existen y en Linux x86 dirían cosas de un PC, así que los inventamos.
bool virtual_file(const char* path, std::string& out) {
    if (!path) return false;
    std::string p(path);
    if (is_maps_path(path) && g_maps_provider) { out = g_maps_provider(); return true; }
    if (p == "/sys/devices/system/cpu/possible" || p == "/sys/devices/system/cpu/present" ||
        p == "/sys/devices/system/cpu/online") {
        unsigned n = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        out = "0-" + std::to_string(n - 1) + "\n";
        return true;
    }
    if (p.rfind("/sys/devices/system/cpu/cpu", 0) == 0 && p.find("cpuinfo_max_freq") != std::string::npos) {
        out = "2000000\n";
        return true;
    }
    if (p == "/proc/cpuinfo") {
        out.clear();
        unsigned n = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        for (unsigned i = 0; i < n; i++)
            out += "processor\t: " + std::to_string(i) +
                   "\nBogoMIPS\t: 38.40\nFeatures\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt\n"
                   "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x0\nCPU part\t: 0xc07\nCPU revision\t: 5\n\n";
        out += "Hardware\t: gdhost\n";
        return true;
    }
    if (p == "/proc/self/auxv") {  // pares de 32 bits: AT_HWCAP, AT_HWCAP2, AT_PAGESZ, AT_NULL
        uint32_t v[] = {16, (1u << 6) | (1u << 12) | (1u << 13) | (1u << 15) | (1u << 16) | (1u << 17) | (1u << 18),
                        26, 0, 6, 4096, 0, 0};
        out.assign(reinterpret_cast<const char*>(v), sizeof v);
        return true;
    }
#ifdef _WIN32
    if (p == "/dev/urandom" || p == "/dev/random") {
        std::random_device rd;
        out.resize(65536);
        for (auto& ch : out) ch = char(rd());
        return true;
    }
#endif
    return false;
}

int open_path(const char* path, int flags, int mode) {
    std::string content;
    if (virtual_file(path, content)) return px::temp_fd_with(content);
    return px::open(path, uint32_t(flags), mode);
}

namespace {

uint32_t g_c_locale = 0;

std::mutex g_guard_lock;
std::recursive_mutex g_guard_mutex;

}  // namespace

void set_errno(int e) { mem::w32(errno_addr(), uint32_t(e)); }
uint32_t errno_addr() {
    if (!t_errno) t_errno = mem::zalloc(4);
    return t_errno;
}

void register_libc() {
    // ---------- variables ----------
    g_sF = mem::zalloc(kFileSize * 3);
    {
        std::lock_guard<std::mutex> lk(g_files_lock);
        g_files[g_sF] = stdin;
        g_files[g_sF + kFileSize] = stdout;
        g_files[g_sF + kFileSize * 2] = stderr;
    }
    for (uint32_t i = 0; i < 3; i++) mem::w16(g_sF + i * kFileSize + 14, uint16_t(i));
    def_data("__sF", g_sF);
    uint32_t guard = mem::zalloc(4);
    mem::w32(guard, 0x13371337);
    def_data("__stack_chk_guard", guard);

    // _ctype_: puntero a tabla de 257 entradas (índice -1 = EOF).
    uint32_t ctab = mem::zalloc(260);
    for (int ch = 0; ch < 256; ch++) {
        uint8_t f = 0;
        if (ch < 128) {
            if (std::isupper(ch)) f |= 0x01;
            if (std::islower(ch)) f |= 0x02;
            if (std::isdigit(ch)) f |= 0x04;
            if (std::isspace(ch)) f |= 0x08;
            if (std::ispunct(ch)) f |= 0x10;
            if (std::iscntrl(ch)) f |= 0x20;
            if (std::isxdigit(ch) && !std::isdigit(ch)) f |= 0x40;
            if (ch == ' ') f |= 0x80;
        }
        *mem::ptr<uint8_t>(ctab + 1 + ch) = f;
    }
    uint32_t ctype_ptr = mem::zalloc(4);
    mem::w32(ctype_ptr, ctab);
    def_data("_ctype_", ctype_ptr);
    uint32_t lower = mem::zalloc(258 * 2), upper = mem::zalloc(258 * 2);
    mem::w16(lower, 0xFFFF);
    mem::w16(upper, 0xFFFF);
    for (int ch = 0; ch < 256; ch++) {
        mem::w16(lower + 2 + ch * 2, uint16_t(ch < 128 ? std::tolower(ch) : ch));
        mem::w16(upper + 2 + ch * 2, uint16_t(ch < 128 ? std::toupper(ch) : ch));
    }
    uint32_t lp = mem::zalloc(4), up = mem::zalloc(4);
    mem::w32(lp, lower);
    mem::w32(up, upper);
    def_data("_tolower_tab_", lp);
    def_data("_toupper_tab_", up);
    def_data("optarg", mem::zalloc(4));
    def_data("optind", mem::zalloc(4));
    def_data("timezone", mem::zalloc(4));
    g_c_locale = mem::strdup("C");

    // ---------- memoria ----------
    DEF("malloc") { ret(c, mem::alloc(arg(c, 0))); });
    DEF("calloc") { ret(c, mem::zalloc(arg(c, 0) * arg(c, 1))); });
    DEF("realloc") { ret(c, mem::realloc(arg(c, 0), arg(c, 1))); });
    DEF("free") { mem::free(arg(c, 0)); });
    DEF("memalign") { ret(c, mem::alloc(arg(c, 1), arg(c, 0))); });
    DEF("posix_memalign") { mem::w32(arg(c, 0), mem::alloc(arg(c, 2), arg(c, 1))); ret(c, 0); });
    DEF("malloc_usable_size") { ret(c, mem::usable_size(arg(c, 0))); });
    DEF("_Znwj") { ret(c, mem::alloc(arg(c, 0))); });
    DEF("_Znaj") { ret(c, mem::alloc(arg(c, 0))); });
    DEF("_ZnwjRKSt9nothrow_t") { ret(c, mem::alloc(arg(c, 0))); });
    DEF("_ZnajRKSt9nothrow_t") { ret(c, mem::alloc(arg(c, 0))); });
    DEF("_ZdlPv") { mem::free(arg(c, 0)); });
    DEF("_ZdaPv") { mem::free(arg(c, 0)); });
    DEF("_ZdlPvRKSt9nothrow_t") { mem::free(arg(c, 0)); });
    DEF("_ZdaPvRKSt9nothrow_t") { mem::free(arg(c, 0)); });

    // ---------- memoria/cadenas ----------
    auto memcpy_h = [](Cpu& c) {
        static const char* w = std::getenv("HOST_WATCH");
        if (w) {
            uint32_t a = uint32_t(std::strtoul(w, nullptr, 16));
            if (arg(c, 0) <= a && a < arg(c, 0) + arg(c, 2))
                logf("[watch] aeabi_memcpy(%08x, %08x, %u) lr=%08x src=%08x %08x%s", arg(c, 0), arg(c, 1), arg(c, 2), cpu::reg(c, 14), mem::r32(arg(c, 1)), mem::r32(arg(c, 1) + 4), cpu::backtrace(c).c_str());
        }
        std::memmove(argp(c, 0), argp(c, 1), arg(c, 2));
    };
    DEF("memcpy") {
        static const char* w = std::getenv("HOST_WATCH");
        if (w) {
            uint32_t a = uint32_t(std::strtoul(w, nullptr, 16));
            if (arg(c, 0) <= a && a < arg(c, 0) + arg(c, 2))
                logf("[watch] memcpy(%08x, %08x, %u) lr=%08x  palabra=%08x", arg(c, 0), arg(c, 1), arg(c, 2), cpu::reg(c, 14),
                     mem::r32(arg(c, 1) + (a - arg(c, 0))));
        }
        std::memmove(argp(c, 0), argp(c, 1), arg(c, 2));
    });
    DEF("memmove") {
        static const char* w = std::getenv("HOST_WATCH");
        if (w) {
            uint32_t a = uint32_t(std::strtoul(w, nullptr, 16));
            if (arg(c, 0) <= a && a < arg(c, 0) + arg(c, 2)) logf("[watch] memmove(%08x, %u) lr=%08x", arg(c, 0), arg(c, 2), cpu::reg(c, 14));
        }
        std::memmove(argp(c, 0), argp(c, 1), arg(c, 2));
    });
    DEF("memset") { std::memset(argp(c, 0), int(arg(c, 1)), arg(c, 2)); });
    DEF("memcmp") { ret(c, uint32_t(std::memcmp(argp(c, 0), argp(c, 1), arg(c, 2)))); });
    DEF("memchr") {
        auto* r = std::memchr(argp(c, 0), int(arg(c, 1)), arg(c, 2));
        ret(c, r ? mem::addr(r) : 0);
    });
    DEF("memrchr") {
        auto* p = argp<uint8_t>(c, 0);
        uint32_t n = arg(c, 2);
        uint8_t ch = uint8_t(arg(c, 1));
        uint32_t r = 0;
        while (n--) if (p[n] == ch) { r = mem::addr(p + n); break; }
        ret(c, r);
    });
    for (const char* n : {"__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8", "__aeabi_memmove", "__aeabi_memmove4", "__aeabi_memmove8"})
        def(n, memcpy_h);
    for (const char* n : {"__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8"})
        def(n, [](Cpu& c) { std::memset(argp(c, 0), int(arg(c, 2)), arg(c, 1)); });
    for (const char* n : {"__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"})
        def(n, [](Cpu& c) { std::memset(argp(c, 0), 0, arg(c, 1)); });

    auto ret_str = [](Cpu& c, const char* r) { ret(c, r ? mem::addr(r) : 0); };
    DEF("strlen") {
        ret(c, uint32_t(std::strlen(args(c, 0))));
    });
    DEF("strnlen") { ret(c, uint32_t(strnlen(args(c, 0), arg(c, 1)))); });
    DEF("strcmp") { ret(c, uint32_t(std::strcmp(args(c, 0), args(c, 1)))); });
    DEF("strncmp") { ret(c, uint32_t(std::strncmp(args(c, 0), args(c, 1), arg(c, 2)))); });
    DEF("strcasecmp") {
        const char *a = args(c, 0), *b = args(c, 1);
        while (*a && std::tolower(uint8_t(*a)) == std::tolower(uint8_t(*b))) { a++; b++; }
        ret(c, uint32_t(std::tolower(uint8_t(*a)) - std::tolower(uint8_t(*b))));
    });
    DEF("strncasecmp") {
        const char *a = args(c, 0), *b = args(c, 1);
        uint32_t n = arg(c, 2);
        int r = 0;
        for (; n; n--, a++, b++) {
            r = std::tolower(uint8_t(*a)) - std::tolower(uint8_t(*b));
            if (r || !*a) break;
        }
        ret(c, uint32_t(r));
    });
    DEF("strcoll") { ret(c, uint32_t(std::strcmp(args(c, 0), args(c, 1)))); });
    DEF("strxfrm") {
        size_t n = std::strlen(args(c, 1));
        if (arg(c, 2) > n) std::memcpy(argp(c, 0), args(c, 1), n + 1);
        ret(c, uint32_t(n));
    });
    DEF("strcpy") { std::strcpy(argp<char>(c, 0), args(c, 1)); });
    DEF("strncpy") { std::strncpy(argp<char>(c, 0), args(c, 1), arg(c, 2)); });
    DEF("strcat") { std::strcat(argp<char>(c, 0), args(c, 1)); });
    DEF("strncat") { std::strncat(argp<char>(c, 0), args(c, 1), arg(c, 2)); });
    DEF("strlcpy") {
        size_t n = std::strlen(args(c, 1)), sz = arg(c, 2);
        if (sz) { size_t k = std::min(n, sz - 1); std::memcpy(argp(c, 0), args(c, 1), k); argp<char>(c, 0)[k] = 0; }
        ret(c, uint32_t(n));
    });
    def("strchr", [ret_str](Cpu& c) { ret_str(c, std::strchr(args(c, 0), int(arg(c, 1)))); });
    def("strrchr", [ret_str](Cpu& c) { ret_str(c, std::strrchr(args(c, 0), int(arg(c, 1)))); });
    def("strstr", [ret_str](Cpu& c) { ret_str(c, std::strstr(args(c, 0), args(c, 1))); });
    def("strpbrk", [ret_str](Cpu& c) { ret_str(c, std::strpbrk(args(c, 0), args(c, 1))); });
    DEF("strspn") { ret(c, uint32_t(std::strspn(args(c, 0), args(c, 1)))); });
    DEF("strcspn") { ret(c, uint32_t(std::strcspn(args(c, 0), args(c, 1)))); });
    DEF("strdup") { ret(c, mem::strdup(args(c, 0))); });
    DEF("strndup") {
        size_t n = strnlen(args(c, 0), arg(c, 1));
        uint32_t a = mem::alloc(uint32_t(n + 1));
        std::memcpy(mem::ptr(a), args(c, 0), n);
        *mem::ptr<char>(a + n) = 0;
        ret(c, a);
    });
    DEF("__strncpy_chk2") { std::strncpy(argp<char>(c, 0), args(c, 1), arg(c, 2)); ret(c, arg(c, 0)); });
    DEF("__strlen_chk") { ret(c, uint32_t(std::strlen(args(c, 0)))); });
    DEF("__strcpy_chk") { std::strcpy(argp<char>(c, 0), args(c, 1)); ret(c, arg(c, 0)); });
    DEF("__strcat_chk") { std::strcat(argp<char>(c, 0), args(c, 1)); ret(c, arg(c, 0)); });
    DEF("__memcpy_chk") { std::memmove(argp(c, 0), argp(c, 1), arg(c, 2)); ret(c, arg(c, 0)); });
    DEF("__memmove_chk") { std::memmove(argp(c, 0), argp(c, 1), arg(c, 2)); ret(c, arg(c, 0)); });
    DEF("__memset_chk") { std::memset(argp(c, 0), int(arg(c, 1)), arg(c, 2)); ret(c, arg(c, 0)); });
    DEF("sincosf") {
        float x = argf(c, 0), s = std::sin(x), co = std::cos(x);
        std::memcpy(argp(c, 1), &s, 4);
        std::memcpy(argp(c, 2), &co, 4);
    });
    DEF("sincos") {
        double x = argd(c, 0), s = std::sin(x), co = std::cos(x);
        std::memcpy(argp(c, 2), &s, 8);
        std::memcpy(argp(c, 3), &co, 8);
    });
    DEF("ftime") {  // struct timeb {time_t, unsigned short ms, short tz, short dst}
        int64_t sec, nsec;
        px::realtime(sec, nsec);
        mem::w32(arg(c, 0), uint32_t(sec));
        mem::w16(arg(c, 0) + 4, uint16_t(nsec / 1000000));
        mem::w16(arg(c, 0) + 6, 0);
        mem::w16(arg(c, 0) + 8, 0);
        ret(c, 0);
    });
    DEF("strtok") {
        static uint32_t save = 0;
        uint32_t s = arg(c, 0);
        if (!s) s = save;
        if (!s) { ret(c, 0); return; }
        const char* d = args(c, 1);
        s += uint32_t(std::strspn(mem::str(s), d));
        if (!*mem::str(s)) { save = 0; ret(c, 0); return; }
        uint32_t e = s + uint32_t(std::strcspn(mem::str(s), d));
        if (*mem::str(e)) { *mem::ptr<char>(e) = 0; save = e + 1; } else save = 0;
        ret(c, s);
    });
    DEF("strtok_r") {
        // char* strtok_r(char* s, const char* delim, char** saveptr)
        uint32_t s = arg(c, 0), save = arg(c, 2);
        if (!s) s = mem::r32(save);
        if (!s) { ret(c, 0); return; }
        const char* d = args(c, 1);
        s += uint32_t(std::strspn(mem::str(s), d));
        if (!*mem::str(s)) { mem::w32(save, 0); ret(c, 0); return; }
        uint32_t e = s + uint32_t(std::strcspn(mem::str(s), d));
        if (*mem::str(e)) { *mem::ptr<char>(e) = 0; mem::w32(save, e + 1); }
        else mem::w32(save, 0);
        ret(c, s);
    });
    DEF("strerror") {
        static std::map<int, uint32_t> cache;
        static std::mutex m;
        std::lock_guard<std::mutex> lk(m);
        int e = int(arg(c, 0));
        if (!cache.count(e)) cache[e] = mem::strdup(std::strerror(e));
        ret(c, cache[e]);
    });
    DEF("strerror_r") {
        std::snprintf(argp<char>(c, 1), arg(c, 2), "%s", std::strerror(int(arg(c, 0))));
        ret(c, 0);
    });

    // strto* con puntero de fin.
    auto set_end = [](Cpu& c, int idx, const char* base, char* end) {
        if (arg(c, idx)) mem::w32(arg(c, idx), arg(c, 0) + uint32_t(end - base));
    };
    def("strtol", [set_end](Cpu& c) { char* e; long v = std::strtol(args(c, 0), &e, int(arg(c, 2)));
        v = std::max<long>(INT32_MIN, std::min<long>(INT32_MAX, v)); set_end(c, 1, args(c, 0), e); ret(c, uint32_t(v)); });
    def("strtoul", [set_end](Cpu& c) { char* e; unsigned long v = std::strtoul(args(c, 0), &e, int(arg(c, 2)));
        if (v > UINT32_MAX && v < (unsigned long)-UINT32_MAX) v = UINT32_MAX; set_end(c, 1, args(c, 0), e); ret(c, uint32_t(v)); });
    def("strtoll", [set_end](Cpu& c) { char* e; long long v = std::strtoll(args(c, 0), &e, int(arg(c, 2))); set_end(c, 1, args(c, 0), e); ret64(c, uint64_t(v)); });
    def("strtoull", [set_end](Cpu& c) { char* e; unsigned long long v = std::strtoull(args(c, 0), &e, int(arg(c, 2))); set_end(c, 1, args(c, 0), e); ret64(c, v); });
    def("strtod", [set_end](Cpu& c) { char* e; double v = std::strtod(args(c, 0), &e); set_end(c, 1, args(c, 0), e); retd(c, v); });
    def("strtof", [set_end](Cpu& c) { char* e; float v = std::strtof(args(c, 0), &e); set_end(c, 1, args(c, 0), e); retf(c, v); });
    DEF("atoi") { ret(c, uint32_t(std::atoi(args(c, 0)))); });
    DEF("atol") { ret(c, uint32_t(std::atol(args(c, 0)))); });
    DEF("atof") { retd(c, std::atof(args(c, 0))); });

    // ctype
    DEF("tolower") { ret(c, uint32_t(std::tolower(int(arg(c, 0))))); });
    DEF("toupper") { ret(c, uint32_t(std::toupper(int(arg(c, 0))))); });
    DEF("isspace") { ret(c, std::isspace(int(arg(c, 0))) ? 1 : 0); });
    DEF("isalpha") { ret(c, std::isalpha(int(arg(c, 0))) ? 1 : 0); });
    DEF("isdigit") { ret(c, std::isdigit(int(arg(c, 0))) ? 1 : 0); });
    DEF("isalnum") { ret(c, std::isalnum(int(arg(c, 0))) ? 1 : 0); });
    DEF("isupper") { ret(c, std::isupper(int(arg(c, 0))) ? 1 : 0); });
    DEF("islower") { ret(c, std::islower(int(arg(c, 0))) ? 1 : 0); });
    DEF("isxdigit") { ret(c, std::isxdigit(int(arg(c, 0))) ? 1 : 0); });
    DEF("isprint") { ret(c, std::isprint(int(arg(c, 0))) ? 1 : 0); });
    DEF("ispunct") { ret(c, std::ispunct(int(arg(c, 0))) ? 1 : 0); });

    // wchar_t es de 4 bytes tanto en Android ARM como en Linux x86-64.
    DEF("wcslen") { ret(c, uint32_t(std::wcslen(argp<wchar_t>(c, 0)))); });
    DEF("wmemcpy") { std::memmove(argp(c, 0), argp(c, 1), arg(c, 2) * 4); });
    DEF("wmemmove") { std::memmove(argp(c, 0), argp(c, 1), arg(c, 2) * 4); });
    DEF("wmemset") { auto* p = argp<uint32_t>(c, 0); for (uint32_t i = 0; i < arg(c, 2); i++) p[i] = arg(c, 1); });
    DEF("wmemcmp") { ret(c, uint32_t(std::wmemcmp(argp<wchar_t>(c, 0), argp<wchar_t>(c, 1), arg(c, 2)))); });
    DEF("wmemchr") { auto* r = std::wmemchr(argp<wchar_t>(c, 0), wchar_t(arg(c, 1)), arg(c, 2)); ret(c, r ? mem::addr(r) : 0); });
    DEF("wcscoll") { ret(c, uint32_t(std::wcscmp(argp<wchar_t>(c, 0), argp<wchar_t>(c, 1)))); });
    DEF("wcsxfrm") { size_t n = std::wcslen(argp<wchar_t>(c, 1)); if (arg(c, 2) > n) std::memcpy(argp(c, 0), argp(c, 1), (n + 1) * 4); ret(c, uint32_t(n)); });
    DEF("btowc") { int ch = int(arg(c, 0)); ret(c, ch >= 0 && ch < 128 ? uint32_t(ch) : 0xFFFFFFFF); });
    DEF("wctob") { uint32_t ch = arg(c, 0); ret(c, ch < 128 ? ch : 0xFFFFFFFF); });
    DEF("towlower") { ret(c, uint32_t(std::towlower(wint_t(arg(c, 0))))); });
    DEF("towupper") { ret(c, uint32_t(std::towupper(wint_t(arg(c, 0))))); });
    DEF("mbrtowc") {
        // Locale "C": un byte = un carácter.
        const char* s = args(c, 1);
        if (!s) { ret(c, 0); return; }
        if (arg(c, 2) == 0) { ret(c, uint32_t(-2)); return; }
        if (arg(c, 0)) mem::w32(arg(c, 0), uint8_t(*s));
        ret(c, *s ? 1 : 0);
    });
    DEF("wcrtomb") { if (arg(c, 0)) *argp<char>(c, 0) = char(arg(c, 1)); ret(c, 1); });
    DEF("wctype") {
        static const char* names[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit"};
        for (uint32_t i = 0; i < 12; i++) if (!std::strcmp(args(c, 0), names[i])) { ret(c, i + 1); return; }
        ret(c, 0);
    });
    DEF("iswctype") {
        wint_t ch = wint_t(arg(c, 0));
        int r = 0;
        switch (arg(c, 1)) {
        case 1: r = std::iswalnum(ch); break; case 2: r = std::iswalpha(ch); break;
        case 3: r = std::iswblank(ch); break; case 4: r = std::iswcntrl(ch); break;
        case 5: r = std::iswdigit(ch); break; case 6: r = std::iswgraph(ch); break;
        case 7: r = std::iswlower(ch); break; case 8: r = std::iswprint(ch); break;
        case 9: r = std::iswpunct(ch); break; case 10: r = std::iswspace(ch); break;
        case 11: r = std::iswupper(ch); break; case 12: r = std::iswxdigit(ch); break;
        }
        ret(c, r ? 1 : 0);
    });
    DEF("wcsftime") { ret(c, 0); });
    DEF("getwc") { ret(c, 0xFFFFFFFF); });
    DEF("ungetwc") { ret(c, 0xFFFFFFFF); });
    DEF("putwc") { ret(c, arg(c, 0)); });
    DEF("setlocale") { ret(c, g_c_locale); });

    // ---------- __aeabi (división y punto flotante en software) ----------
    DEF("__aeabi_idiv") { int32_t b = int32_t(arg(c, 1)); ret(c, b ? uint32_t(int32_t(arg(c, 0)) / b) : 0); });
    DEF("__aeabi_uidiv") { uint32_t b = arg(c, 1); ret(c, b ? arg(c, 0) / b : 0); });
    DEF("__aeabi_idivmod") {
        int32_t a = int32_t(arg(c, 0)), b = int32_t(arg(c, 1));
        if (!b) { ret64(c, 0); return; }
        ret64(c, uint32_t(a / b) | (uint64_t(uint32_t(a % b)) << 32));
    });
    DEF("__aeabi_uidivmod") {
        uint32_t a = arg(c, 0), b = arg(c, 1);
        if (!b) { ret64(c, 0); return; }
        ret64(c, (a / b) | (uint64_t(a % b) << 32));
    });
    DEF("__aeabi_ldivmod") {
        int64_t a = int64_t(arg64(c, 0)), b = int64_t(arg64(c, 2));
        int64_t q = b ? a / b : 0, r = b ? a % b : 0;
        ret64(c, uint64_t(q));
        cpu::reg(c, 2) = uint32_t(uint64_t(r));
        cpu::reg(c, 3) = uint32_t(uint64_t(r) >> 32);
    });
    DEF("__aeabi_uldivmod") {
        uint64_t a = arg64(c, 0), b = arg64(c, 2);
        uint64_t q = b ? a / b : 0, r = b ? a % b : 0;
        ret64(c, q);
        cpu::reg(c, 2) = uint32_t(r);
        cpu::reg(c, 3) = uint32_t(r >> 32);
    });
    DEF("__aeabi_lmul") { ret64(c, arg64(c, 0) * arg64(c, 2)); });
    DEF("__aeabi_llsl") { ret64(c, arg64(c, 0) << (arg(c, 2) & 63)); });
    DEF("__aeabi_llsr") { ret64(c, arg64(c, 0) >> (arg(c, 2) & 63)); });
    DEF("__aeabi_lasr") { ret64(c, uint64_t(int64_t(arg64(c, 0)) >> (arg(c, 2) & 63))); });
    DEF("__aeabi_i2d") { retd(c, double(int32_t(arg(c, 0)))); });
    DEF("__aeabi_ui2d") { retd(c, double(arg(c, 0))); });
    DEF("__aeabi_l2d") { retd(c, double(int64_t(arg64(c, 0)))); });
    DEF("__aeabi_ul2d") { retd(c, double(arg64(c, 0))); });
    DEF("__aeabi_i2f") { retf(c, float(int32_t(arg(c, 0)))); });
    DEF("__aeabi_ui2f") { retf(c, float(arg(c, 0))); });
    DEF("__aeabi_l2f") { retf(c, float(int64_t(arg64(c, 0)))); });
    DEF("__aeabi_ul2f") { retf(c, float(arg64(c, 0))); });
    DEF("__aeabi_d2iz") { ret(c, uint32_t(int32_t(argd(c, 0)))); });
    DEF("__aeabi_d2uiz") { double d = argd(c, 0); ret(c, d <= 0 ? 0 : uint32_t(d)); });
    DEF("__aeabi_d2lz") { ret64(c, uint64_t(int64_t(argd(c, 0)))); });
    DEF("__aeabi_d2ulz") { double d = argd(c, 0); ret64(c, d <= 0 ? 0 : uint64_t(d)); });
    DEF("__aeabi_f2iz") { ret(c, uint32_t(int32_t(argf(c, 0)))); });
    DEF("__aeabi_f2uiz") { float f = argf(c, 0); ret(c, f <= 0 ? 0 : uint32_t(f)); });
    DEF("__aeabi_f2lz") { ret64(c, uint64_t(int64_t(argf(c, 0)))); });
    DEF("__aeabi_f2ulz") { float f = argf(c, 0); ret64(c, f <= 0 ? 0 : uint64_t(f)); });
    DEF("__aeabi_f2d") { retd(c, double(argf(c, 0))); });
    DEF("__aeabi_d2f") { retf(c, float(argd(c, 0))); });
    DEF("__aeabi_dadd") { retd(c, argd(c, 0) + argd(c, 2)); });
    DEF("__aeabi_dsub") { retd(c, argd(c, 0) - argd(c, 2)); });
    DEF("__aeabi_drsub") { retd(c, argd(c, 2) - argd(c, 0)); });
    DEF("__aeabi_dmul") { retd(c, argd(c, 0) * argd(c, 2)); });
    DEF("__aeabi_ddiv") { retd(c, argd(c, 0) / argd(c, 2)); });
    DEF("__aeabi_dcmpeq") { ret(c, argd(c, 0) == argd(c, 2)); });
    DEF("__aeabi_dcmplt") { ret(c, argd(c, 0) < argd(c, 2)); });
    DEF("__aeabi_dcmple") { ret(c, argd(c, 0) <= argd(c, 2)); });
    DEF("__aeabi_dcmpge") { ret(c, argd(c, 0) >= argd(c, 2)); });
    DEF("__aeabi_dcmpgt") { ret(c, argd(c, 0) > argd(c, 2)); });
    DEF("__aeabi_dcmpun") { ret(c, std::isnan(argd(c, 0)) || std::isnan(argd(c, 2))); });
    DEF("__aeabi_fadd") { retf(c, argf(c, 0) + argf(c, 1)); });
    DEF("__aeabi_fsub") { retf(c, argf(c, 0) - argf(c, 1)); });
    DEF("__aeabi_frsub") { retf(c, argf(c, 1) - argf(c, 0)); });
    DEF("__aeabi_fmul") { retf(c, argf(c, 0) * argf(c, 1)); });
    DEF("__aeabi_fdiv") { retf(c, argf(c, 0) / argf(c, 1)); });
    DEF("__aeabi_fcmpeq") { ret(c, argf(c, 0) == argf(c, 1)); });
    DEF("__aeabi_fcmplt") { ret(c, argf(c, 0) < argf(c, 1)); });
    DEF("__aeabi_fcmple") { ret(c, argf(c, 0) <= argf(c, 1)); });
    DEF("__aeabi_fcmpge") { ret(c, argf(c, 0) >= argf(c, 1)); });
    DEF("__aeabi_fcmpgt") { ret(c, argf(c, 0) > argf(c, 1)); });
    DEF("__aeabi_fcmpun") { ret(c, std::isnan(argf(c, 0)) || std::isnan(argf(c, 1))); });

    // ---------- libm (softfp: double en r0:r1, float en r0) ----------
#define MATH_D1(n) DEF(#n) { retd(c, std::n(argd(c, 0))); });
#define MATH_D2(n) DEF(#n) { retd(c, std::n(argd(c, 0), argd(c, 2))); });
#define MATH_F1(n) DEF(#n "f") { retf(c, std::n(argf(c, 0))); });
#define MATH_F2(n) DEF(#n "f") { retf(c, std::n(argf(c, 0), argf(c, 1))); });
    MATH_D1(sin) MATH_D1(cos) MATH_D1(tan) MATH_D1(asin) MATH_D1(acos) MATH_D1(atan)
    MATH_D1(sinh) MATH_D1(cosh) MATH_D1(tanh) MATH_D1(exp) MATH_D1(log) MATH_D1(log10) MATH_D1(log2)
    MATH_D1(sqrt) MATH_D1(floor) MATH_D1(ceil) MATH_D1(round) MATH_D1(trunc) MATH_D1(fabs) MATH_D1(cbrt)
    MATH_D2(atan2) MATH_D2(pow) MATH_D2(fmod) MATH_D2(hypot) MATH_D2(fmin) MATH_D2(fmax)
    MATH_F1(sin) MATH_F1(cos) MATH_F1(tan) MATH_F1(asin) MATH_F1(acos) MATH_F1(atan)
    MATH_F1(exp) MATH_F1(log) MATH_F1(log10) MATH_F1(sqrt) MATH_F1(floor) MATH_F1(ceil)
    MATH_F1(round) MATH_F1(trunc) MATH_F1(fabs)
    MATH_D1(rint) MATH_D1(nearbyint) MATH_D1(exp2) MATH_D1(expm1) MATH_D1(log1p) MATH_D1(asinh) MATH_D1(acosh) MATH_D1(atanh)
    MATH_F1(rint) MATH_F1(nearbyint) MATH_F1(exp2) MATH_F1(expm1) MATH_F1(log1p) MATH_F1(log2) MATH_F1(cbrt) MATH_F1(sinh) MATH_F1(cosh) MATH_F1(tanh)
    MATH_F2(atan2) MATH_F2(pow) MATH_F2(fmod) MATH_F2(hypot) MATH_F2(fmin) MATH_F2(fmax)
    DEF("lrint") { ret(c, uint32_t(int32_t(std::lrint(argd(c, 0))))); });
    DEF("lrintf") { ret(c, uint32_t(int32_t(std::lrintf(argf(c, 0))))); });
    DEF("llrint") { ret64(c, uint64_t(std::llrint(argd(c, 0)))); });
    DEF("copysign") { retd(c, std::copysign(argd(c, 0), argd(c, 2))); });
    DEF("copysignf") { retf(c, std::copysign(argf(c, 0), argf(c, 1))); });
    DEF("setpriority") { ret(c, 0); });
    DEF("getpriority") { ret(c, 0); });
    DEF("lround") { ret(c, uint32_t(int32_t(std::lround(argd(c, 0))))); });
    DEF("lroundf") { ret(c, uint32_t(int32_t(std::lroundf(argf(c, 0))))); });
    DEF("ldexp") { retd(c, std::ldexp(argd(c, 0), int(arg(c, 2)))); });
    DEF("ldexpf") { retf(c, std::ldexp(argf(c, 0), int(arg(c, 1)))); });
    DEF("frexp") { int e; double r = std::frexp(argd(c, 0), &e); mem::w32(arg(c, 2), uint32_t(e)); retd(c, r); });
    DEF("modf") { double ip; double r = std::modf(argd(c, 0), &ip); uint64_t b; std::memcpy(&b, &ip, 8); mem::w64(arg(c, 2), b); retd(c, r); });
    DEF("modff") { float ip; float r = std::modf(argf(c, 0), &ip); uint32_t b; std::memcpy(&b, &ip, 4); mem::w32(arg(c, 1), b); retf(c, r); });
    DEF("isnan") { ret(c, std::isnan(argd(c, 0))); });
    DEF("isinf") { ret(c, std::isinf(argd(c, 0))); });
    DEF("__isnanf") { ret(c, std::isnan(argf(c, 0))); });
    DEF("__isinff") { ret(c, std::isinf(argf(c, 0))); });
    DEF("abs") { ret(c, uint32_t(std::abs(int32_t(arg(c, 0))))); });
    DEF("labs") { ret(c, uint32_t(std::abs(int32_t(arg(c, 0))))); });

    // ---------- números aleatorios ----------
    DEF("rand") { ret(c, uint32_t(std::rand() & 0x7FFFFFFF)); });
    DEF("srand") { std::srand(arg(c, 0)); });
    DEF("random") { ret(c, uint32_t(std::rand() & 0x7FFFFFFF)); });
    DEF("srandom") { std::srand(arg(c, 0)); });
    DEF("lrand48") { static std::mt19937 rng(1); ret(c, rng() & 0x7FFFFFFF); });
    DEF("srand48") { ret(c, 0); });
    DEF("arc4random") { static std::mt19937 rng(std::random_device{}()); ret(c, rng()); });
    DEF("getentropy") { for (uint32_t i = 0; i < arg(c, 1); i++) argp<uint8_t>(c, 0)[i] = uint8_t(std::rand()); ret(c, 0); });

    // ---------- printf / log ----------
    auto out = [](FILE* f, const std::string& s) {
        if (f == stdout || f == stderr) { std::fwrite(s.data(), 1, s.size(), stdout); std::fflush(stdout); }
        else if (f) std::fwrite(s.data(), 1, s.size(), f);
    };
    def("printf", [out](Cpu& c) { auto va = VaArgs::from_regs(c, 1); auto s = format(args(c, 0), va); out(stdout, s); ret(c, uint32_t(s.size())); });
    def("vprintf", [out](Cpu& c) { auto va = VaArgs::from_list(arg(c, 1)); auto s = format(args(c, 0), va); out(stdout, s); ret(c, uint32_t(s.size())); });
    def("fprintf", [out](Cpu& c) { auto va = VaArgs::from_regs(c, 2); auto s = format(args(c, 1), va); out(hfile(arg(c, 0)), s); ret(c, uint32_t(s.size())); });
    def("vfprintf", [out](Cpu& c) { auto va = VaArgs::from_list(arg(c, 2)); auto s = format(args(c, 1), va); out(hfile(arg(c, 0)), s); ret(c, uint32_t(s.size())); });
    auto put_str = [](Cpu& c, uint32_t dst, uint32_t max, const std::string& s) {
        if (max) {
            size_t n = std::min<size_t>(s.size(), max - 1);
            std::memcpy(mem::ptr(dst), s.data(), n);
            *mem::ptr<char>(dst + uint32_t(n)) = 0;
        }
        ret(c, uint32_t(s.size()));
    };
    def("sprintf", [put_str](Cpu& c) { auto va = VaArgs::from_regs(c, 2); put_str(c, arg(c, 0), 0x7FFFFFFF, format(args(c, 1), va)); });
    def("snprintf", [put_str](Cpu& c) { auto va = VaArgs::from_regs(c, 3); put_str(c, arg(c, 0), arg(c, 1), format(args(c, 2), va)); });
    def("vsprintf", [put_str](Cpu& c) { auto va = VaArgs::from_list(arg(c, 2)); put_str(c, arg(c, 0), 0x7FFFFFFF, format(args(c, 1), va)); });
    def("vsnprintf", [put_str](Cpu& c) { auto va = VaArgs::from_list(arg(c, 3)); put_str(c, arg(c, 0), arg(c, 1), format(args(c, 2), va)); });
    def("__vsnprintf_chk", [put_str](Cpu& c) { auto va = VaArgs::from_list(arg(c, 5)); put_str(c, arg(c, 0), arg(c, 1), format(args(c, 4), va)); });
    def("__sprintf_chk", [put_str](Cpu& c) { auto va = VaArgs::from_regs(c, 4); put_str(c, arg(c, 0), 0x7FFFFFFF, format(args(c, 3), va)); });
    DEF("__android_log_print") {
        auto va = VaArgs::from_regs(c, 3);
        logf("[android %u/%s] %s", arg(c, 0), args(c, 1), format(args(c, 2), va).c_str());
        ret(c, 1);
    });
    DEF("__android_log_vprint") {
        auto va = VaArgs::from_list(arg(c, 3));
        logf("[android %u/%s] %s", arg(c, 0), args(c, 1), format(args(c, 2), va).c_str());
        ret(c, 1);
    });
    DEF("__android_log_write") { logf("[android %u/%s] %s", arg(c, 0), args(c, 1), args(c, 2)); ret(c, 1); });
    DEF("puts") { logf("%s", args(c, 0)); ret(c, 1); });

    // ---------- stdio ----------
    DEF("fopen") {
        if (std::getenv("HOST_TRACE_FS")) logf("[fs] fopen(%s, %s)", VP(c, 0), args(c, 1)); std::string vcontent;
        FILE* f = virtual_file(VP(c, 0), vcontent) ? fdopen(px::temp_fd_with(vcontent), fmode("r").c_str()) : std::fopen(VP(c, 0), fmode(args(c, 1)).c_str()); if (!f) set_errno(errno); ret(c, gfile(f)); });
    DEF("fdopen") { FILE* f = fdopen(int(arg(c, 0)), fmode(args(c, 1)).c_str()); ret(c, gfile(f)); });
    DEF("fclose") {
        FILE* f = hfile(arg(c, 0));
        int r = f ? std::fclose(f) : -1;
        { std::lock_guard<std::mutex> lk(g_files_lock); g_files.erase(arg(c, 0)); g_files_gen.fetch_add(1); }
        ret(c, uint32_t(r));
    });
    DEF("fread") { FILE* f = hfile(arg(c, 3)); ret(c, f ? uint32_t(std::fread(argp(c, 0), arg(c, 1), arg(c, 2), f)) : 0); });
    def("fwrite", [out](Cpu& c) {
        FILE* f = hfile(arg(c, 3));
        size_t n = size_t(arg(c, 1)) * arg(c, 2);
        if (f == stdout || f == stderr) { out(f, std::string(argp<char>(c, 0), n)); ret(c, arg(c, 2)); return; }
        ret(c, f ? uint32_t(std::fwrite(argp(c, 0), arg(c, 1), arg(c, 2), f)) : 0);
    });
    DEF("fseek") { FILE* f = hfile(arg(c, 0)); ret(c, f ? uint32_t(std::fseek(f, long(int32_t(arg(c, 1))), int(arg(c, 2)))) : uint32_t(-1)); });
    DEF("ftell") { FILE* f = hfile(arg(c, 0)); ret(c, f ? uint32_t(std::ftell(f)) : uint32_t(-1)); });
    DEF("rewind") { if (FILE* f = hfile(arg(c, 0))) std::rewind(f); });
    DEF("fflush") { FILE* f = hfile(arg(c, 0)); if (f) std::fflush(f); ret(c, 0); });
    DEF("feof") { FILE* f = hfile(arg(c, 0)); ret(c, f ? uint32_t(std::feof(f)) : 1); });
    DEF("ferror") { FILE* f = hfile(arg(c, 0)); ret(c, f ? uint32_t(std::ferror(f)) : 1); });
    DEF("fileno") { FILE* f = hfile(arg(c, 0)); ret(c, f ? uint32_t(fileno(f)) : uint32_t(-1)); });
    DEF("fgets") { FILE* f = hfile(arg(c, 2)); ret(c, f && std::fgets(argp<char>(c, 0), int(arg(c, 1)), f) ? arg(c, 0) : 0); });
    def("fputs", [out](Cpu& c) { out(hfile(arg(c, 1)), args(c, 0)); ret(c, 1); });
    def("fputc", [out](Cpu& c) { out(hfile(arg(c, 1)), std::string(1, char(arg(c, 0)))); ret(c, arg(c, 0)); });
    def("putc", [out](Cpu& c) { out(hfile(arg(c, 1)), std::string(1, char(arg(c, 0)))); ret(c, arg(c, 0)); });
    DEF("getc") { FILE* f = hfile(arg(c, 0)); ret(c, f ? uint32_t(std::fgetc(f)) : uint32_t(-1)); });
    DEF("fgetc") { FILE* f = hfile(arg(c, 0)); ret(c, f ? uint32_t(std::fgetc(f)) : uint32_t(-1)); });
    DEF("ungetc") { FILE* f = hfile(arg(c, 1)); ret(c, f ? uint32_t(std::ungetc(int(arg(c, 0)), f)) : uint32_t(-1)); });
    DEF("setvbuf") { ret(c, 0); });

    // ---------- sistema de archivos ----------
    DEF("open") {
        if (std::getenv("HOST_TRACE_FS")) logf("[fs] open(%s)", VP(c, 0));
        ret_px(c, open_path(VP(c, 0), int(arg(c, 1)), int(arg(c, 2))));
    });
    DEF("close") { ret_px(c, px::close(int(arg(c, 0)))); });
    DEF("read") { ret_px(c, px::read(int(arg(c, 0)), argp(c, 1), arg(c, 2))); });
    DEF("write") {
        int fd = int(arg(c, 0));
        if (fd == 1 || fd == 2) { std::fwrite(argp(c, 1), 1, arg(c, 2), stdout); std::fflush(stdout); ret(c, arg(c, 2)); return; }
        ret_px(c, px::write(fd, argp(c, 1), arg(c, 2)));
    });
    DEF("writev") {
        long total = 0;
        for (uint32_t i = 0; i < arg(c, 2); i++) {
            uint32_t iov = arg(c, 1) + i * 8;
            long r = px::write(int(arg(c, 0)), mem::ptr(mem::r32(iov)), mem::r32(iov + 4));
            if (r < 0) { ret_px(c, r); return; }
            total += r;
        }
        ret(c, uint32_t(total));
    });
    DEF("lseek") { ret_px(c, long(px::lseek(int(arg(c, 0)), int32_t(arg(c, 1)), int(arg(c, 2))))); });
    DEF("lseek64") { int64_t r = px::lseek(int(arg(c, 0)), int64_t(arg64(c, 2)), int(arg(c, 4))); if (r < 0) set_errno(px::last_error()); ret64(c, uint64_t(r)); });
    DEF("pread64") { ret_px(c, px::pread(int(arg(c, 0)), argp(c, 1), arg(c, 2), int64_t(arg64(c, 4)))); });
    DEF("pwrite64") { ret_px(c, px::pwrite(int(arg(c, 0)), argp(c, 1), arg(c, 2), int64_t(arg64(c, 4)))); });
    DEF("fsync") { ret_px(c, px::fsync(int(arg(c, 0)))); });
    DEF("ftruncate64") { ret_px(c, px::ftruncate(int(arg(c, 0)), int64_t(arg64(c, 2)))); });
    DEF("fcntl") {
        int fd = int(arg(c, 0)), cmd = int(arg(c, 1));
        // Bloqueos: flock de 32 bits (cmd 5-7) o flock64 de ARM EABI (cmd 12-14).
        if ((cmd >= 5 && cmd <= 7) || (cmd >= 12 && cmd <= 14)) {
            bool is64 = cmd >= 12;
            uint32_t g = arg(c, 2);
            int16_t type = int16_t(mem::r16(g)), whence = int16_t(mem::r16(g + 2));
            int64_t start = is64 ? int64_t(mem::r64(g + 8)) : int32_t(mem::r32(g + 4));
            int64_t len = is64 ? int64_t(mem::r64(g + 16)) : int32_t(mem::r32(g + 8));
            int32_t pid = 0;
            int base = is64 ? cmd - 7 : cmd;  // 12-14 -> 5-7
            int r = px::lock(fd, base, type, whence, start, len, pid);
            if (r == 0 && base == 5) {
                mem::w16(g, uint16_t(type));
                mem::w32(g + (is64 ? 24 : 12), uint32_t(pid));
            }
            ret_px(c, r);
            return;
        }
        ret_px(c, px::fcntl(fd, cmd, arg(c, 2)));
    });
    DEF("pipe") {
        int fds[2];
        int r = px::pipe(fds);
        if (!r) { mem::w32(arg(c, 0), uint32_t(fds[0])); mem::w32(arg(c, 0) + 4, uint32_t(fds[1])); }
        ret_px(c, r);
    });
    DEF("poll") { ret_px(c, px::poll(argp<px::PollFd>(c, 0), arg(c, 1), int(arg(c, 2)))); });
    DEF("stat") {
        if (std::getenv("HOST_TRACE_FS")) logf("[fs] stat(%s)", VP(c, 0));
        px::Stat s;
        int r = px::stat(VP(c, 0), s);
        if (!r) write_stat(arg(c, 1), s);
        ret_px(c, r);
    });
    DEF("lstat") { px::Stat s; int r = px::lstat(VP(c, 0), s); if (!r) write_stat(arg(c, 1), s); ret_px(c, r); });
    DEF("fstat") { px::Stat s; int r = px::fstat(int(arg(c, 0)), s); if (!r) write_stat(arg(c, 1), s); ret_px(c, r); });
    DEF("access") {
        if (std::getenv("HOST_TRACE_FS")) logf("[fs] access(%s)", VP(c, 0));
        std::string vc;
        if (virtual_file(VP(c, 0), vc)) { ret(c, 0); return; }
        ret_px(c, px::access(VP(c, 0), int(arg(c, 1))));
    });
    DEF("mkdir") {
        if (std::getenv("HOST_TRACE_FS")) logf("[fs] mkdir(%s)", VP(c, 0));
        ret_px(c, px::mkdir(VP(c, 0), int(arg(c, 1))));
    });
    DEF("rmdir") { ret_px(c, px::rmdir(VP(c, 0))); });
    DEF("unlink") { ret_px(c, px::unlink(VP(c, 0))); });
    DEF("remove") { ret_errno(c, std::remove(VP(c, 0))); });
    DEF("rename") {
#ifdef _WIN32
        std::remove(VP(c, 1));  // en Windows rename no sobrescribe
#endif
        ret_errno(c, std::rename(VP(c, 0), VP(c, 1)));
    });
    DEF("fchmod") { ret(c, 0); });
    DEF("fchown") { ret(c, 0); });
    DEF("utimes") { ret(c, 0); });
    DEF("readlink") { ret_px(c, px::readlink(VP(c, 0), argp<char>(c, 1), arg(c, 2))); });
    DEF("getcwd") {
        std::string d = px::getcwd();
        if (d.size() + 1 > arg(c, 1)) { set_errno(34); ret(c, 0); return; }
        std::memcpy(argp(c, 0), d.c_str(), d.size() + 1);
        ret(c, arg(c, 0));
    });
    DEF("basename") {
        static thread_local uint32_t buf = 0;
        if (!buf) buf = mem::alloc(4096);
        const char* p = args(c, 0);
        const char* s = p ? std::strrchr(p, '/') : nullptr;
        std::snprintf(mem::ptr<char>(buf), 4096, "%s", s ? s + 1 : (p ? p : "."));
        ret(c, buf);
    });
    DEF("opendir") {
        void* d = px::opendir(VP(c, 0));
        if (!d) { set_errno(px::last_error()); ret(c, 0); return; }
        uint32_t g = mem::zalloc(16);
        g_dirs[g] = GDir{d, mem::zalloc(280)};
        ret(c, g);
    });
    DEF("readdir") {
        // struct dirent de bionic: d_ino u64, d_off s64, d_reclen u16, d_type u8, d_name[256]
        auto it = g_dirs.find(arg(c, 0));
        if (it == g_dirs.end()) { ret(c, 0); return; }
        px::DirEntry e;
        if (!px::readdir(it->second.d, e)) { ret(c, 0); return; }
        uint32_t g = it->second.ent;
        mem::w64(g, e.ino);
        mem::w64(g + 8, 0);
        mem::w16(g + 16, 280);
        *mem::ptr<uint8_t>(g + 18) = e.type;
        std::snprintf(mem::ptr<char>(g + 19), 256, "%s", e.name.c_str());
        ret(c, g);
    });
    DEF("closedir") {
        auto it = g_dirs.find(arg(c, 0));
        if (it != g_dirs.end()) { px::closedir(it->second.d); g_dirs.erase(it); }
        ret(c, 0);
    });
    DEF("fnmatch") { ret(c, 1); });

    // ---------- memoria virtual ----------
    auto do_mmap = [](Cpu& c, uint64_t off) {
        uint32_t len = (arg(c, 1) + 4095) & ~4095u;
        int fd = int(arg(c, 4));
        uint32_t hint = arg(c, 0), flags = arg(c, 3);
        uint32_t a;
        if (hint && (flags & 0x10)) {  // MAP_FIXED: todo el espacio del invitado ya está reservado
            a = hint & ~4095u;
            mem::commit(a, len);
        } else if (hint && hint >= mem::kNullGuard && hint < mem::kHeapBase && mem::lib_range_free(hint & ~4095u, len)) {
            a = hint & ~4095u;  // respetamos la sugerencia si cae en zona libre (cerca del código)
            mem::claim_lib_range(a, len);
        } else {
            a = mem::alloc(len, 4096);
        }
        if (std::getenv("HOST_TRACE_MPROTECT")) logf("[host] mmap(%08x, %x, flags=%x) -> %08x", hint, len, flags, a);
        std::memset(mem::ptr(a), 0, len);
        if (fd >= 0 && !(arg(c, 3) & 0x20)) px::pread(fd, mem::ptr(a), arg(c, 1), int64_t(off));
        ret(c, a);
    };
    def("mmap", [do_mmap](Cpu& c) { do_mmap(c, arg(c, 5)); });
    def("__mmap2", [do_mmap](Cpu& c) { do_mmap(c, uint64_t(arg(c, 5)) * 4096); });
    DEF("munmap") { ret(c, 0); });
    DEF("mprotect") {  // la memoria del invitado siempre es RW
        if (std::getenv("HOST_TRACE_MPROTECT")) logf("[host] mprotect(%08x, %x, %u) lr=%08x", arg(c, 0), arg(c, 1), arg(c, 2), cpu::reg(c, 14));
        ret(c, 0);
    });
    DEF("madvise") { ret(c, 0); });
    DEF("mlock") { ret(c, 0); });
    DEF("mremap") { ret(c, mem::realloc(arg(c, 0), arg(c, 2))); });
    DEF("syscall") {
        uint32_t n = arg(c, 0);
        if (n == 0xF0002) {  // cacheflush(start, end, flags): el invitado reescribió código
            cpu::invalidate(arg(c, 1), arg(c, 2) - arg(c, 1));
            ret(c, 0);
        } else if (n == 224) {
            ret(c, px::tid());
        } else {
            logf("[host] syscall %u no soportada", n);
            set_errno(38);  // ENOSYS
            ret(c, uint32_t(-1));
        }
    });
    // Syscalls directas (svc #0) con la numeración de ARM EABI.
    cpu::set_syscall_handler([](Cpu& c) {
        uint32_t n = cpu::reg(c, 7);
        auto r = [&](long v) { cpu::reg(c, 0) = uint32_t(v < 0 ? -px::last_error() : v); };
        switch (n) {
        case 0xF0002: cpu::invalidate(arg(c, 0), arg(c, 1) - arg(c, 0)); cpu::reg(c, 0) = 0; break;  // cacheflush
        case 224: cpu::reg(c, 0) = px::tid(); break;
        case 20: cpu::reg(c, 0) = px::pid(); break;
        case 125: cpu::reg(c, 0) = 0; break;  // mprotect
        case 4: r(px::write(int(arg(c, 0)), argp(c, 1), arg(c, 2))); break;
        case 3: r(px::read(int(arg(c, 0)), argp(c, 1), arg(c, 2))); break;
        case 6: r(px::close(int(arg(c, 0)))); break;
        case 158: std::this_thread::yield(); cpu::reg(c, 0) = 0; break;  // sched_yield
        case 1: case 248: logf("[host] exit por syscall (%d)", int(arg(c, 0))); std::_Exit(int(arg(c, 0)));
        default:
            logf("[host] syscall directa %u no soportada (lr=%08x)", n, cpu::reg(c, 14));
            cpu::reg(c, 0) = uint32_t(-38);
        }
    });
    DEF("cacheflush") { cpu::invalidate(arg(c, 0), arg(c, 1) - arg(c, 0)); ret(c, 0); });
    DEF("__clear_cache") { cpu::invalidate(arg(c, 0), arg(c, 1) - arg(c, 0)); });


    // ---------- ayudas del kernel ARM (página 0xffff0000) ----------
    {
        auto place = [](uint32_t at, const char* name, cpu::Handler h) {
            uint32_t t = cpu::add_thunk(name, std::move(h));
            // ldr pc, [pc, #-4] ; .word thunk
            mem::w32(at, 0xE51FF004);
            mem::w32(at + 4, t);
        };
        auto set_c = [](Cpu& c, bool ok) {
            uint32_t cpsr = cpu::get_cpsr(c);
            cpu::set_cpsr(c, ok ? (cpsr | (1u << 29)) : (cpsr & ~(1u << 29)));
        };
        mem::w32(0xFFFF0FFC, 5);  // __kuser_helper_version
        place(0xFFFF0F60, "__kuser_cmpxchg64", [set_c](Cpu& c) {
            uint32_t ptr = arg(c, 2);
            uint64_t expected = mem::r64(arg(c, 0)), desired = mem::r64(arg(c, 1));
            bool ok = __atomic_compare_exchange_n(mem::ptr<uint64_t>(ptr), &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            ret(c, ok ? 0 : 1);
            set_c(c, ok);
        });
        place(0xFFFF0FA0, "__kuser_memory_barrier", [](Cpu& c) { __atomic_thread_fence(__ATOMIC_SEQ_CST); });
        place(0xFFFF0FC0, "__kuser_cmpxchg", [set_c](Cpu& c) {
            uint32_t expected = arg(c, 0);
            bool ok = __atomic_compare_exchange_n(mem::ptr<uint32_t>(arg(c, 2)), &expected, arg(c, 1), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            ret(c, ok ? 0 : 1);
            set_c(c, ok);
        });
        place(0xFFFF0FE0, "__kuser_get_tls", [](Cpu& c) { ret(c, cpu::tls_base()); });
    }
    DEF("getauxval") {
        // AT_HWCAP: VFP, NEON, VFPv3, TLS, VFPv4, IDIVA, IDIVT
        if (arg(c, 0) == 16) ret(c, (1u << 6) | (1u << 12) | (1u << 13) | (1u << 15) | (1u << 16) | (1u << 17) | (1u << 18));
        else ret(c, 0);
    });
    DEF("__open_2") {
        if (std::getenv("HOST_TRACE_FS")) logf("[fs] __open_2(%s)", VP(c, 0));
        ret_px(c, open_path(VP(c, 0), int(arg(c, 1)), 0));
    });
    DEF("__read_chk") { ret_px(c, px::read(int(arg(c, 0)), argp(c, 1), arg(c, 2))); });
    DEF("memmem") {
        auto* h = argp<uint8_t>(c, 0);
        auto* n = argp<uint8_t>(c, 2);
        uint32_t hl = arg(c, 1), nl = arg(c, 3);
        uint32_t r = 0;
        if (nl == 0) r = arg(c, 0);
        else for (uint32_t i = 0; i + nl <= hl; i++) if (!std::memcmp(h + i, n, nl)) { r = arg(c, 0) + i; break; }
        ret(c, r);
    });

    // ---------- proceso / tiempo ----------
    DEF("__errno") { ret(c, errno_addr()); });
    DEF("getpid") { ret(c, px::pid()); });
    DEF("gettid") { ret(c, px::tid()); });
    DEF("getuid") { ret(c, 10123); });
    DEF("geteuid") { ret(c, 10123); });
    DEF("getgid") { ret(c, 10123); });
    DEF("getegid") { ret(c, 10123); });
    DEF("getenv") { ret(c, 0); });
    DEF("sysconf") {
        switch (arg(c, 0)) {
        case 0x27: ret(c, 4096); break;                                   // _SC_PAGESIZE
        case 0x60: case 0x61: ret(c, std::thread::hardware_concurrency()); break;  // CPUs
        case 6: ret(c, 100); break;                                       // _SC_CLK_TCK
        default: logf("[host] sysconf(%u)", arg(c, 0)); ret(c, uint32_t(-1)); break;
        }
    });
    DEF("time") { uint32_t t = uint32_t(std::time(nullptr)); if (arg(c, 0)) mem::w32(arg(c, 0), t); ret(c, t); });
    DEF("clock_gettime") {
        int64_t sec, nsec;
        if (arg(c, 0) == 0) px::realtime(sec, nsec);  // CLOCK_REALTIME; el resto, monotónico
        else px::monotonic(sec, nsec);
        mem::w32(arg(c, 1), uint32_t(sec));
        mem::w32(arg(c, 1) + 4, uint32_t(nsec));
        ret(c, 0);
    });
    DEF("gettimeofday") {
        int64_t sec, nsec;
        px::realtime(sec, nsec);
        if (arg(c, 0)) { mem::w32(arg(c, 0), uint32_t(sec)); mem::w32(arg(c, 0) + 4, uint32_t(nsec / 1000)); }
        ret(c, 0);
    });
    DEF("clock") { ret(c, uint32_t(std::clock())); });
    DEF("localtime") {
        static thread_local uint32_t buf = 0;
        if (!buf) buf = mem::zalloc(44);
        std::tm tmv;
        px::localtime(int32_t(mem::r32(arg(c, 0))), tmv);
        tm_to_guest(buf, tmv);
        ret(c, buf);
    });
    DEF("localtime_r") { std::tm tmv; px::localtime(int32_t(mem::r32(arg(c, 0))), tmv); tm_to_guest(arg(c, 1), tmv); ret(c, arg(c, 1)); });
    DEF("gmtime_r") { std::tm tmv; px::gmtime(int32_t(mem::r32(arg(c, 0))), tmv); tm_to_guest(arg(c, 1), tmv); ret(c, arg(c, 1)); });
    DEF("mktime") { struct tm t = tm_from_guest(arg(c, 0)); ret(c, uint32_t(std::mktime(&t))); });
    DEF("strftime") { struct tm t = tm_from_guest(arg(c, 3)); ret(c, uint32_t(std::strftime(argp<char>(c, 0), arg(c, 1), args(c, 2), &t))); });
    DEF("sleep") { px::sleep_ns(int64_t(arg(c, 0)) * 1000000000); ret(c, 0); });
    DEF("usleep") { px::sleep_ns(int64_t(arg(c, 0)) * 1000); ret(c, 0); });
    DEF("nanosleep") {
        px::sleep_ns(int64_t(mem::r32(arg(c, 0))) * 1000000000 + int32_t(mem::r32(arg(c, 0) + 4)));
        ret(c, 0);
    });
    DEF("sched_yield") { std::this_thread::yield(); ret(c, 0); });
    DEF("sched_setaffinity") { ret(c, 0); });
    DEF("sigaction") { ret(c, 0); });
    DEF("signal") { ret(c, 0); });
    DEF("raise") { logf("[host] raise(%u)", arg(c, 0)); ret(c, 0); });
    DEF("exit") { logf("[host] exit(%d) lr=%08x%s", int(arg(c, 0)), cpu::reg(c, 14), cpu::backtrace(c, 12).c_str()); std::fflush(stdout); std::_Exit(int(arg(c, 0))); });
    DEF("abort") { logf("[FATAL] abort() desde el invitado, lr=%08x", cpu::reg(c, 14)); std::fflush(stdout); std::_Exit(4); });
    DEF("__stack_chk_fail") { logf("[FATAL] __stack_chk_fail lr=%08x", cpu::reg(c, 14)); std::_Exit(5); });
    DEF("__assert2") { logf("[FATAL] assert %s:%u %s: %s", args(c, 0), arg(c, 1), args(c, 2), args(c, 3)); std::_Exit(6); });
    DEF("gethostname") { std::snprintf(argp<char>(c, 0), arg(c, 1), "android"); ret(c, 0); });
    DEF("getpwuid_r") { mem::w32(arg(c, 4), 0); ret(c, 2); });

    // ---------- runtime de C++ ----------
    DEF("__cxa_atexit") { ret(c, 0); });
    DEF("__cxa_finalize") {});
    DEF("__cxa_pure_virtual") { logf("[FATAL] llamada a método virtual puro, lr=%08x", cpu::reg(c, 14)); std::_Exit(7); });
    DEF("__cxa_guard_acquire") {
        g_guard_mutex.lock();
        if (*argp<uint8_t>(c, 0)) { g_guard_mutex.unlock(); ret(c, 0); }
        else ret(c, 1);
    });
    DEF("__cxa_guard_release") { *argp<uint8_t>(c, 0) = 1; g_guard_mutex.unlock(); });
    DEF("__cxa_guard_abort") { g_guard_mutex.unlock(); });
    for (const char* n : {"__cxa_throw", "__cxa_rethrow", "_Unwind_RaiseException", "_Unwind_Resume", "__cxa_bad_cast"}) {
        std::string name = n;
        def(n, [name](Cpu& c) {
            logf("[FATAL] %s: excepción C++ en el invitado (no soportado aún), lr=%08x", name.c_str(), cpu::reg(c, 14));
            std::_Exit(8);
        });
    }

    // ---------- qsort / bsearch con comparador del invitado ----------
    DEF("qsort") {
        uint32_t b = arg(c, 0), n = arg(c, 1), sz = arg(c, 2), cmp = arg(c, 3);
        std::vector<uint8_t> tmp(sz);
        for (uint32_t i = 1; i < n; i++) {  // inserción: estable y suficiente
            std::memcpy(tmp.data(), mem::ptr(b + i * sz), sz);
            uint32_t key = mem::alloc(sz);
            std::memcpy(mem::ptr(key), tmp.data(), sz);
            int32_t j = int32_t(i) - 1;
            while (j >= 0 && int32_t(cpu::call(cmp, {b + uint32_t(j) * sz, key})) > 0) {
                std::memcpy(mem::ptr(b + uint32_t(j + 1) * sz), mem::ptr(b + uint32_t(j) * sz), sz);
                j--;
            }
            std::memcpy(mem::ptr(b + uint32_t(j + 1) * sz), tmp.data(), sz);
            mem::free(key);
        }
    });
    DEF("bsearch") {
        uint32_t key = arg(c, 0), b = arg(c, 1), n = arg(c, 2), sz = arg(c, 3), cmp = arg(c, 4);
        int32_t lo = 0, hi = int32_t(n) - 1;
        while (lo <= hi) {
            int32_t mid = (lo + hi) / 2;
            int32_t r = int32_t(cpu::call(cmp, {key, b + uint32_t(mid) * sz}));
            if (r == 0) { ret(c, b + uint32_t(mid) * sz); return; }
            if (r < 0) hi = mid - 1; else lo = mid + 1;
        }
        ret(c, 0);
    });
}

}  // namespace host
