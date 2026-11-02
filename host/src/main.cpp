// gdhost: ejecuta las librerías ARM32 de 1.3 GDPS en un PC.
//
// Uso:  gdhost [juego.apk]                  (sin argumentos busca un .apk junto al ejecutable)
//       gdhost <carpeta de librerías> <juego.apk>
// Variables: GD_HEADLESS=1 (sin ventana), GD_FRAMES=n, GD_SCRIPT="frame:acción[:x,y];...",
//            GD_SIZE=1280x720, GD_DATA=carpeta, HOST_TRACE, HOST_WATCHDOG.
#ifndef __ANDROID__
#define SDL_MAIN_HANDLED  // en PC el main es nuestro; en Android SDL llama a SDL_main
#endif
#include <SDL2/SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "apk.h"
#include "cpu.h"
#include "fastfloat_blob.h"
#include "fastlib_blob.h"
#include "platform.h"
#include "songs.h"
#include "elf_loader.h"
#include "jni.h"
#include "log.h"
#include "memory.h"
#include "thunks.h"

namespace cpu {
void dump_last_cpu();
void dump_main_thread();
void set_symbolizer(std::function<std::string(uint32_t)> f);
}  // namespace cpu
namespace host {
void register_net();
void audio_init();
void text_init();
void audio_focus(bool focused);
uint64_t call_java_native(const std::string& cls, const std::string& method, const std::vector<uint32_t>& a);
}  // namespace host

static ElfLoader g_loader;
static std::string g_data = "data";

// ---------------------------------------------------------------------------
// dlopen/dlsym/dladdr sobre nuestro cargador.
static void register_dl() {
    using host::arg;
    using host::args;
    using host::ret;
    static std::map<uint32_t, std::string> handles;
    DEF("dlopen") {
        std::string n = args(c, 0) ? args(c, 0) : "";
        auto slash = n.find_last_of('/');
        if (slash != std::string::npos) n = n.substr(slash + 1);
        // El identificador es un "soinfo" como el del cargador de Android 4.x: algunos mods leen
        // de él la dirección base de la librería (name[128], phdr, phnum, entry, base, size).
        uint32_t h = 0;
        for (auto& [k, v] : handles)
            if (v == n) h = k;
        if (!h) {
            h = mem::zalloc(0x200);
            std::memcpy(mem::ptr<char>(h), n.c_str(), std::min<size_t>(n.size(), 127));
            if (ElfLib* l = g_loader.lib(n)) {
                mem::w32(h + 0x80, l->base + mem::r32(l->base + 0x1c));
                mem::w32(h + 0x84, *mem::ptr<uint16_t>(l->base + 0x2c));
                mem::w32(h + 0x8c, l->base);
                mem::w32(h + 0x90, l->size);
            }
            handles[h] = n;
        }
        logf("[dl] dlopen(%s) -> %x", n.c_str(), h);
        ret(c, h);
    });
    DEF("dlsym") {
        std::string sym = args(c, 1);
        uint32_t a = 0;
        auto it = handles.find(host::arg(c, 0));
        if (it != handles.end()) {
            if (ElfLib* l = g_loader.lib(it->second)) {
                auto e = l->exports.find(sym);
                if (e != l->exports.end()) a = e->second;
            }
        }
        if (!a) a = g_loader.find(sym);
        if (!a) a = host::lookup(sym);
        logf("[dl] dlsym(%s) -> %08x", sym.c_str(), a);
        ret(c, a);
    });
    DEF("__gnu_Unwind_Find_exidx") {
        uint32_t pc = arg(c, 0);
        for (auto& l : g_loader.libs()) {
            if (pc >= l->base && pc < l->base + l->size) {
                mem::w32(arg(c, 1), l->exidx_count);
                ret(c, l->exidx);
                return;
            }
        }
        mem::w32(arg(c, 1), 0);
        ret(c, 0);
    });
    DEF("dlerror") { ret(c, 0); });
    DEF("dlclose") { ret(c, 0); });
    DEF("dladdr") {
        uint32_t addr = arg(c, 0), info = arg(c, 1);
        for (auto& l : g_loader.libs()) {
            if (addr >= l->base && addr < l->base + l->size) {
                static std::map<std::string, uint32_t> names;
                if (!names.count(l->name)) names[l->name] = mem::strdup(("/data/app/lib/arm/" + l->name).c_str());
                mem::w32(info, names[l->name]);
                mem::w32(info + 4, l->base);
                mem::w32(info + 8, 0);
                mem::w32(info + 12, 0);
                ret(c, 1);
                return;
            }
        }
        ret(c, 0);
    });
}

// Busca una función nativa de Java: primero RegisterNatives, luego Java_<clase>_<método>.
static uint64_t call_native(const std::string& cls, const std::string& method, const std::vector<uint32_t>& a) {
    uint32_t fn = host::jni_native(cls + "." + method);
    if (!fn) {
        std::string sym = "Java_" + cls + "_" + method;
        for (auto& ch : sym)
            if (ch == '/') ch = '_';
        fn = g_loader.find(sym);
    }
    if (!fn) {
        logf("[main] nativo %s.%s no encontrado", cls.c_str(), method.c_str());
        return 0;
    }
    return cpu::call(fn, a);
}

uint64_t host::call_java_native(const std::string& cls, const std::string& method, const std::vector<uint32_t>& a) {
    return call_native(cls, method, a);
}

static uint32_t fbits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// Resolución interna en porcentaje (GD_RES=100 la desactiva). Por defecto 90 %.
static float res_scale() {
    int p = 90;
    if (const char* r = std::getenv("GD_RES")) p = std::atoi(r);
    return p >= 30 && p < 100 ? p / 100.0f : 1.0f;
}

static void save_png(const std::string& path, int w, int h) {
    host::gl_fb_size(w, h);
    std::vector<uint8_t> px(size_t(w) * h * 4);
    host::gl_read_pixels(w, h, px.data());
    stbi_flip_vertically_on_write(1);  // GL tiene el origen abajo
    if (stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4)) logf("[main] captura %s", path.c_str());
}

// Librerías nativas del juego, en el orden de la actividad (game, fmodL, ascella).
static const char* const kGameLibs[] = {"libgame.so", "libfmodL.so", "libascella.so"};

// Extrae lib/armeabi-v7a/*.so del APK a una carpeta (si no están ya).
static std::string extract_libs(const std::string& apk_path) {
    std::string dir = g_data + "/lib";
    std::filesystem::create_directories(dir);
    for (const char* n : kGameLibs) {
        std::string dst = dir + "/" + n;
        auto data = apk::read(std::string("lib/armeabi-v7a/") + n);
        if (!data) { logf("[main] el APK no contiene %s", n); continue; }
        if (std::filesystem::exists(dst) && std::filesystem::file_size(dst) == data->size()) continue;
        std::FILE* f = std::fopen(dst.c_str(), "wb");
        if (!f) continue;
        std::fwrite(data->data(), 1, data->size(), f);
        std::fclose(f);
        logf("[main] extraída %s", n);
    }
    return dir;
}

// Busca un .apk en la carpeta indicada.
static std::string find_apk(const std::filesystem::path& dir) {
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ".apk") return e.path().string();
    return "";
}

static std::string fake_maps() {
    std::string out;
    char line[256];
    for (auto& l : g_loader.libs()) {
        std::snprintf(line, sizeof line, "%08x-%08x r-xp 00000000 fd:00 1000 /data/app/app.onepointthree.starry_sky/lib/arm/%s\n",
                      l->base, l->base + l->size, l->name.c_str());
        out += line;
    }
    return out;
}

// Teclas del PC → códigos de tecla de Android (KeyEvent.KEYCODE_*), para Ascella (atajos del mod menu).
static bool is_jump_key(SDL_Keycode k) { return k == SDLK_SPACE || k == SDLK_UP || k == SDLK_w; }

static int android_keycode(SDL_Keycode k) {
    if (k >= SDLK_a && k <= SDLK_z) return 29 + (k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return 7 + (k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 131 + (k - SDLK_F1);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 145 + (k - SDLK_KP_1);
    switch (k) {
    case SDLK_KP_0: return 144;
    case SDLK_SPACE: return 62;
    case SDLK_RETURN: return 66;
    case SDLK_KP_ENTER: return 160;
    case SDLK_TAB: return 61;
    case SDLK_BACKSPACE: return 67;
    case SDLK_DELETE: return 112;
    case SDLK_UP: return 19;
    case SDLK_DOWN: return 20;
    case SDLK_LEFT: return 21;
    case SDLK_RIGHT: return 22;
    case SDLK_LSHIFT: return 59;
    case SDLK_RSHIFT: return 60;
    case SDLK_LCTRL: return 113;
    case SDLK_RCTRL: return 114;
    case SDLK_LALT: return 57;
    case SDLK_RALT: return 58;
    case SDLK_COMMA: return 55;
    case SDLK_PERIOD: return 56;
    case SDLK_MINUS: return 69;
    case SDLK_EQUALS: return 70;
    case SDLK_LEFTBRACKET: return 71;
    case SDLK_RIGHTBRACKET: return 72;
    case SDLK_BACKSLASH: return 73;
    case SDLK_SEMICOLON: return 74;
    case SDLK_QUOTE: return 75;
    case SDLK_SLASH: return 76;
    case SDLK_BACKQUOTE: return 68;
    case SDLK_PAGEUP: return 92;
    case SDLK_PAGEDOWN: return 93;
    case SDLK_HOME: return 122;
    case SDLK_END: return 123;
    case SDLK_INSERT: return 124;
    case SDLK_CAPSLOCK: return 115;
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Entrada: toques simulados como en Cocos2dxRenderer.
static float g_game_w = 1280, g_game_h = 720;  // tamaño en píxeles con el que dibuja el juego
namespace input {
const std::string kRenderer = "org/cocos2dx/lib/Cocos2dxRenderer";

// Como Android al pasar a segundo plano: el juego guarda la partida en nativeOnPause.
[[noreturn]] void quit() {
    logf("[main] cerrando: guardando partida...");
    call_native(kRenderer, "nativeOnPause", {host::jni_env(), 0});
    logf("[main] listo");
    std::fflush(stdout);
    std::_Exit(0);
}
bool down = false;

void touch_begin(float x, float y) {
    down = true;
    songs::touch(true, x / g_game_w, y / g_game_h);
    call_native(kRenderer, "nativeTouchesBegin", {host::jni_env(), 0, 0, fbits(x), fbits(y)});
}
void touch_move(float x, float y) {
    uint32_t ids = host::new_java_array(1, 4), xs = host::new_java_array(1, 4), ys = host::new_java_array(1, 4);
    mem::w32(host::java_array_data(ids), 0);
    mem::w32(host::java_array_data(xs), fbits(x));
    mem::w32(host::java_array_data(ys), fbits(y));
    call_native(kRenderer, "nativeTouchesMove", {host::jni_env(), 0, ids, xs, ys});
    mem::free(ids);
    mem::free(xs);
    mem::free(ys);
}
void touch_end(float x, float y) {
    down = false;
    songs::touch(false, x / g_game_w, y / g_game_h);
    call_native(kRenderer, "nativeTouchesEnd", {host::jni_env(), 0, 0, fbits(x), fbits(y)});
}
void key_back() { call_native(kRenderer, "nativeKeyDown", {host::jni_env(), 0, 4}); }  // KEYCODE_BACK
}  // namespace input

// Guion de entradas para pruebas sin pantalla: "120:tap:640,360;300:shot;400:key_back"
struct ScriptStep {
    int frame;
    std::string action;
    float x = 0, y = 0;
};
static std::vector<ScriptStep> parse_script(const char* s) {
    std::vector<ScriptStep> out;
    if (!s) return out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ';')) {
        ScriptStep st;
        char act[32] = {0};
        if (std::sscanf(item.c_str(), "%d:%31[a-z_]:%f,%f", &st.frame, act, &st.x, &st.y) >= 2) {
            st.action = act;
            out.push_back(st);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// FPS Bypass (botón en Opciones): 60, 120, 144, 240, 360, sin límite (0) y vuelta a 60. Se guarda en data/fps.txt.
static int g_fps_cap = 60;
static bool g_vsync = false;  // la pantalla marca el ritmo a 60 FPS

// Vsync solo sirve de reloj si la pantalla va a 60 Hz; en 90/120/144 Hz se usa el temporizador.
static bool set_vsync(SDL_Window* w, int cap) {
    SDL_DisplayMode dm{};
    bool sixty = w && SDL_GetWindowDisplayMode(w, &dm) == 0 && (dm.refresh_rate == 0 || (dm.refresh_rate >= 59 && dm.refresh_rate <= 61));
    bool on = cap == 60 && sixty;
    return SDL_GL_SetSwapInterval(on ? 1 : 0) == 0 && on;
}
static SDL_Window* g_win = nullptr;
static void fps_cycle() {
    static const int steps[] = {60, 120, 144, 240, 360, 0};
    size_t i = 0;
    while (i < std::size(steps) && steps[i] != g_fps_cap) i++;
    g_fps_cap = steps[(i + 1) % std::size(steps)];
    if (g_win) g_vsync = set_vsync(g_win, g_fps_cap);
    if (std::FILE* f = std::fopen((g_data + "/fps.txt").c_str(), "w")) {
        std::fprintf(f, "%d\n", g_fps_cap);
        std::fclose(f);
    }
    logf("[main] FPS bypass: %d", g_fps_cap);
}

#ifdef __ANDROID__
// Android: los datos van a la carpeta externa de la app (Android/data/<paquete>/files) y el
// APK del juego viaja dentro de nuestro APK como "assets/game.apk"; se copia una vez.
static std::string android_setup() {
    std::string ext = SDL_AndroidGetExternalStoragePath() ? SDL_AndroidGetExternalStoragePath() : ".";
    setenv("GD_DATA", ext.c_str(), 1);
    setenv("TMPDIR", SDL_AndroidGetInternalStoragePath(), 1);
    std::string game = std::string(SDL_AndroidGetInternalStoragePath()) + "/game.apk";
    SDL_RWops* in = SDL_RWFromFile("game.apk", "rb");
    if (!in) return "";
    Sint64 size = SDL_RWsize(in);
    std::error_code ec;
    if (!std::filesystem::exists(game, ec) || Sint64(std::filesystem::file_size(game, ec)) != size) {
        logf("[android] copiando el APK del juego (%lld MB)...", (long long)(size >> 20));
        std::FILE* out = std::fopen((game + ".part").c_str(), "wb");
        std::vector<char> buf(1 << 20);
        size_t n;
        while (out && (n = SDL_RWread(in, buf.data(), 1, buf.size())) > 0) std::fwrite(buf.data(), 1, n, out);
        if (out) std::fclose(out);
        std::filesystem::rename(game + ".part", game, ec);
    }
    SDL_RWclose(in);
    return game;
}
#endif

#ifdef __ANDROID__
#include <csignal>
#include <unwind.h>
#include <sched.h>
#include <sys/resource.h>
#include <dlfcn.h>
// Si el programa se cae, apuntar dónde (señal y direcciones de la pila nativa).
static _Unwind_Reason_Code unwind_cb(_Unwind_Context* ctx, void* arg) {
    int& n = *static_cast<int*>(arg);
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) {
        Dl_info di{};
        dladdr(reinterpret_cast<void*>(pc), &di);
        logf("  #%d %p %s+%lx (%s)", n, reinterpret_cast<void*>(pc), di.dli_sname ? di.dli_sname : "?",
             (unsigned long)(pc - reinterpret_cast<uintptr_t>(di.dli_saddr ? di.dli_saddr : di.dli_fbase)),
             di.dli_fname ? di.dli_fname : "?");
    }
    return ++n > 30 ? _URC_END_OF_STACK : _URC_NO_REASON;
}
static void crash_handler(int sig, siginfo_t* info, void*) {
    logf("[FATAL] señal %d en dirección %p", sig, info ? info->si_addr : nullptr);
    int n = 0;
    _Unwind_Backtrace(unwind_cb, &n);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void install_crash_handler() {
    struct sigaction sa {};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE}) sigaction(s, &sa, nullptr);
}
#endif

#ifdef _WIN32
// Portátiles con dos GPU: pedir la dedicada (NVIDIA Optimus / AMD PowerXpress).
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

#ifdef __ANDROID__
// El hilo del juego hace casi todo el trabajo: lo fija a los núcleos más rápidos y le sube la prioridad.
static void android_fast_thread() {
    long best = 0;
    std::vector<int> fast;
    for (int cpu = 0; cpu < 16; cpu++) {
        char path[96];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        FILE* f = std::fopen(path, "r");
        if (!f) continue;
        long khz = 0;
        if (std::fscanf(f, "%ld", &khz) != 1) khz = 0;
        std::fclose(f);
        if (khz > best) { best = khz; fast.clear(); }
        if (khz == best && khz > 0) fast.push_back(cpu);
    }
    if (!fast.empty()) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int cpu : fast) CPU_SET(cpu, &set);
        int r = sched_setaffinity(0, sizeof set, &set);
        logf("[android] hilo del juego en %zu núcleo(s) de %ld MHz%s", fast.size(), best / 1000, r ? " (sin permiso)" : "");
    }
    if (setpriority(PRIO_PROCESS, 0, -8) != 0) logf("[android] no se pudo subir la prioridad del hilo");
}
#endif

// Avisos de PHP que el servidor intercala con los datos ("Deprecated: ... on line 4"),
// en formato HTML (<br /><b>Warning</b>: ...<br />) o texto. El juego en Android los ignora.
static std::string strip_php_notices(const std::string& raw) {
    static const char* kinds[] = {"Deprecated", "Warning", "Notice", "Strict Standards"};
    // Formato de Xdebug: cada aviso es una tabla dentro de <font ...>...</font>.
    std::string in;
    for (size_t i = 0; i < raw.size();) {
        if (raw.compare(i, 5, "<font") == 0) {
            size_t e = raw.find("</font>", i);
            if (e != std::string::npos) { i = e + 7; continue; }
        }
        in += raw[i++];
    }
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        size_t start = i;
        size_t j = i;
        bool html = in.compare(j, 6, "<br />") == 0;
        if (html) {
            j += 6;
            while (j < in.size() && (in[j] == '\n' || in[j] == '\r')) j++;
            if (in.compare(j, 3, "<b>") == 0) j += 3; else html = false;
        }
        size_t k = html ? j : i;
        bool hit = false;
        for (const char* t : kinds) {
            size_t n = std::strlen(t);
            if (in.compare(k, n, t) == 0 && (html ? in.compare(k + n, 5, "</b>:") == 0 : in.compare(k + n, 1, ":") == 0)) hit = true;
        }
        if (hit) {
            size_t e = in.find(" on line ", k);
            if (e != std::string::npos) {
                e += 9;
                if (in.compare(e, 3, "<b>") == 0) e += 3;
                while (e < in.size() && std::isdigit((unsigned char)in[e])) e++;
                if (in.compare(e, 4, "</b>") == 0) e += 4;
                if (in.compare(e, 6, "<br />") == 0) e += 6;
                while (e < in.size() && (in[e] == '\n' || in[e] == '\r')) e++;
                while (!html && !out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
                i = e;
                continue;
            }
        }
        if (!html && (in[start] == '\n' || in[start] == '\r') && out.empty()) { i++; continue; }
        out += in[start];
        i = start + 1;
    }
    // Restos de saltos de línea HTML entre avisos.
    for (size_t k; (k = out.find("<br />")) != std::string::npos;) {
        size_t e = k + 6;
        while (e < out.size() && (out[e] == '\n' || out[e] == '\r')) e++;
        out.erase(k, e - k);
    }
    while (!out.empty() && (out[0] == '\n' || out[0] == '\r' || out[0] == ' ')) out.erase(0, 1);
    return out;
}

int main(int argc, char** argv) {
#ifdef __ANDROID__
    install_crash_handler();
    logf("[android] inicio");
    android_fast_thread();
    std::string android_apk = android_setup();
    logf("[android] apk: %s", android_apk.c_str());
#endif
    if (const char* d = std::getenv("GD_DATA")) g_data = d;
    // Versiones anteriores guardaban en "gdhost-data": se renombra para conservar la partida.
    {
        std::error_code ec;
        if (!std::getenv("GD_DATA") && !std::filesystem::exists("data") && std::filesystem::is_directory("gdhost-data"))
            std::filesystem::rename("gdhost-data", "data", ec);
    }
#ifdef _WIN32
    // Sin consola: el registro solo se escribe si se pide (GD_LOG=1 o --log), en data/log.txt.
    bool want_log = std::getenv("GD_LOG") != nullptr;
    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], "--log") == 0) {
            want_log = true;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--;
            break;
        }
    if (!std::getenv("GD_CONSOLE")) {
        if (want_log) {
            std::filesystem::create_directories(g_data);
            if (!std::freopen((g_data + "/log.txt").c_str(), "w", stdout)) std::fprintf(stderr, "sin log\n");
        } else {
            std::freopen("NUL", "w", stdout);
        }
    }
#endif
    std::string dir, apk_path;
    if (argc >= 3) {
        dir = argv[1];
        apk_path = argv[2];
    } else if (argc == 2 && std::filesystem::is_directory(argv[1])) {
        dir = argv[1];  // solo carga las librerías (pruebas)
    } else {
#ifdef __ANDROID__
        apk_path = android_apk;
#else
        apk_path = argc == 2 ? argv[1] : find_apk(std::filesystem::path(argv[0]).parent_path().empty() ? "." : std::filesystem::path(argv[0]).parent_path());
        if (apk_path.empty()) apk_path = find_apk(".");
#endif
        if (apk_path.empty()) {
            const char* msg = "No se encontró ningún .apk.\n\nPon el APK de 1.3 GDPS en la misma carpeta que este programa.";
            std::fprintf(stderr, "%s\n", msg);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "1.3 GDPS", msg, nullptr);
            return 1;
        }
    }
    std::filesystem::create_directories(g_data + "/data");
    std::filesystem::create_directories(g_data + "/sdcard");
    std::filesystem::create_directories(g_data + "/songs");
    std::filesystem::create_directories(g_data + "/saved_songs");
    songs::set_dir(g_data + "/saved_songs");
    if (const char* w = std::getenv("HOST_WATCHDOG")) {
        int secs = std::atoi(w);
        std::thread([secs] {
            std::this_thread::sleep_for(std::chrono::seconds(secs));
            logf("[watchdog] %d s sin terminar", secs);
            cpu::dump_last_cpu();
            std::_Exit(10);
        }).detach();
    }

    mem::init();
    host::register_libc();
    host::register_pthread();
    host::register_jni();
    host::register_gl();
    host::register_zlib();
    host::register_misc();
    host::register_net();
    register_dl();
    host::set_maps_provider(fake_maps);
    cpu::set_symbolizer([](uint32_t a) { return g_loader.symbolize(a); });

    // Mismo orden que GameActivity: game, fmodL, ascella.
    if (!apk_path.empty() && !apk::open(apk_path)) {
        logf("[main] no se pudo abrir el APK %s", apk_path.c_str());
        return 1;
    }
    if (dir.empty()) dir = extract_libs(apk_path);
    for (const char* n : kGameLibs) {
        if (!std::filesystem::exists(dir + "/" + n)) continue;
        if (!g_loader.load(dir + "/" + n)) return 1;
    }
    // FMOD: Ascella elige OpenSL ES, que no emulamos; forzamos la salida AudioTrack (11).
    {
        static uint32_t orig = g_loader.find_original("_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE");
        g_loader.override_symbol("_ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE",
                                 cpu::add_thunk("FMOD::System::setOutput (forzado)", [](cpu::Cpu& c) {
                                     uint32_t type = cpu::reg(c, 1);
                                     if (type != 2 && type != 3 && type != 4 && type != 5) type = 11;
                                     logf("[fmod] setOutput(%u) -> %u", cpu::reg(c, 1), type);
                                     cpu::reg(c, 0) = uint32_t(cpu::call(orig, {cpu::reg(c, 0), type}));
                                 }));
    }
    // El servidor puede responder con una página HTML (protección anti-bots) en vez de datos
    // del juego; el juego la trocea como datos y se cae. En el hilo de red de CCHttpClient,
    // donde se marca la respuesta como correcta (movs r3,#1; strb r3,[r4,#0x1c]), llamamos a
    // una comprobación que la marca como fallida si el cuerpo es HTML.
    {
        const uint32_t site = 0x01140b6e, veneer = 0x0145fff0;
        if (mem::r32(site) == 0x77232301) {
            mem::commit(veneer, 16);
            uint32_t th = cpu::add_thunk("CCHttpClient (filtro HTML)", [](cpu::Cpu& c) {
                cpu::reg(c, 3) = 1;
                uint32_t r = cpu::reg(c, 4), b = mem::r32(r + 0x20), e = mem::r32(r + 0x24);
                const char* p = mem::ptr<char>(b);
                uint32_t n = e - b, i = 0;
                while (i < n && (p[i] == ' ' || p[i] == '\n' || p[i] == '\r' || p[i] == '\t')) i++;
                // Errores de PHP / base de datos caída ("Connection failed: SQLSTATE..."):
                // el juego los tomaría como un nivel vacío (ID 0) y se cerraría al abrirlo.
                // Quita los avisos de PHP intercalados y deja solo los datos.
                if (b && n && (std::memchr(p, '<', n) || std::strstr(std::string(p, n).c_str(), " on line "))) {
                    std::string full(p, n), clean = strip_php_notices(full);
                    if (clean.size() != full.size()) {
                        std::memcpy(mem::ptr<char>(b), clean.data(), clean.size());
                        mem::w32(r + 0x24, b + uint32_t(clean.size()));
                        logf("[net] avisos de PHP quitados de la respuesta (%u -> %zu bytes)", n, clean.size());
                        e = b + uint32_t(clean.size());
                        n = e - b;
                        i = 0;
                        while (i < n && (p[i] == ' ' || p[i] == '\n' || p[i] == '\r' || p[i] == '\t')) i++;
                    }
                }
                std::string body = b ? std::string(p + i, std::min(n - i, 400u)) : "";
                bool php_err = body.find("SQLSTATE") != std::string::npos
                    || body.rfind("Connection failed", 0) == 0 || body.rfind("Fatal error", 0) == 0
                    || body.rfind("Warning:", 0) == 0 || body.rfind("Parse error", 0) == 0
                    || body.rfind("Error", 0) == 0;
                if (php_err) {
                    logf("[net] error del servidor (%u bytes): %.60s -> se trata como fallo", n, body.c_str());
                    mem::ptr<uint8_t>(r)[0x1c] = 0;
                } else if (b && i < n && p[i] == '<') {
                    logf("[net] respuesta HTML del servidor (%u bytes): se trata como error", n);
                    logf("[net] inicio: %s", std::string(p + i, std::min(n - i, 600u)).c_str());
                    if (std::FILE* f = std::fopen((g_data + "/respuesta_servidor.html").c_str(), "wb")) {
                        std::fwrite(p, 1, n, f);
                        std::fclose(f);
                    }
                    mem::ptr<uint8_t>(r)[0x1c] = 0;
                } else {
                    logf("[net] respuesta del servidor (%u bytes): %.60s", n, b ? std::string(p, std::min(n, 60u)).c_str() : "");
                    mem::ptr<uint8_t>(r)[0x1c] = 1;
                    // CCHttpResponse: petición en +0x18; CCHttpRequest: URL (std::string) en +0x1c.
                    uint32_t req = mem::r32(r + 0x18);
                    const char* url = req ? mem::str(mem::r32(req + 0x1c)) : nullptr;
                    if (url && b) songs::on_http(url, std::string(p, n));
                }
            });
            mem::w32(veneer, 0xe51ff004);  // ldr pc, [pc, #-4]
            mem::w32(veneer + 4, th);
            int32_t off = int32_t(veneer - ((site + 4) & ~3u));
            uint32_t S = (off >> 24) & 1, I1 = (off >> 23) & 1, I2 = (off >> 22) & 1;
            uint32_t J1 = (~I1 ^ S) & 1, J2 = (~I2 ^ S) & 1;
            uint16_t h1 = 0xf000 | (S << 10) | ((off >> 12) & 0x3ff);
            uint16_t h2 = 0xc000 | (J1 << 13) | (J2 << 11) | ((off >> 1) & 0x7fe);
            mem::w16(site, h1);
            mem::w16(site + 2, h2);
            cpu::invalidate(site, 4);
        }
    }
    // Rendimiento: funciones pequeñas y muy usadas como código ARM dentro del juego (sin salir a C++).
    if (!std::getenv("GD_NO_FASTLIB")) {
        uint32_t base = mem::alloc(sizeof(fastlib::kCode) + 16, 16);
        std::memcpy(mem::ptr<uint8_t>(base), fastlib::kCode, sizeof(fastlib::kCode));
        auto off = [](const char* n) {
            for (auto& s : fastlib::kSyms)
                if (std::strcmp(s.name, n) == 0) return s.off;
            return 0xffffffffu;
        };
        mem::w32(base + off("fl_host"), host::lookup("memcpy"));
        mem::w32(base + off("fl_host") + 4, host::lookup("memset"));
        const std::pair<const char*, const char*> map[] = {
            {"memcpy", "fl_memcpy"}, {"memmove", "fl_memmove"}, {"memset", "fl_memset"}, {"memcmp", "fl_memcmp"},
            {"strlen", "fl_strlen"}, {"strcmp", "fl_strcmp"}, {"strncmp", "fl_strncmp"}, {"strchr", "fl_strchr"},
            {"__aeabi_memcpy", "fl_aeabi_memcpy"}, {"__aeabi_memcpy4", "fl_aeabi_memcpy"}, {"__aeabi_memcpy8", "fl_aeabi_memcpy"},
            {"__aeabi_memmove", "fl_aeabi_memmove"}, {"__aeabi_memmove4", "fl_aeabi_memmove"}, {"__aeabi_memmove8", "fl_aeabi_memmove"},
            {"__aeabi_memset", "fl_aeabi_memset"}, {"__aeabi_memset4", "fl_aeabi_memset"}, {"__aeabi_memset8", "fl_aeabi_memset"},
            {"__aeabi_memclr", "fl_aeabi_memclr"}, {"__aeabi_memclr4", "fl_aeabi_memclr"}, {"__aeabi_memclr8", "fl_aeabi_memclr"}};
        for (auto& [sym, fl] : map)
            if (off(fl) != 0xffffffffu) host::def_data(sym, base + off(fl));  // código ARM (no Thumb)
        cpu::invalidate(base, sizeof(fastlib::kCode));
    }
    // Rendimiento: libgame no usa la FPU y hace cada operación de coma flotante con una rutina por
    // software. Se sustituyen por versiones con instrucciones VFP (mismo resultado IEEE 754).
    if (ElfLib* game = g_loader.lib("libgame.so"); game && !std::getenv("GD_NO_FASTFLOAT")) {
        uint32_t base = mem::alloc(sizeof(fastfloat::kCode) + 16, 16);
        std::memcpy(mem::ptr<uint8_t>(base), fastfloat::kCode, sizeof(fastfloat::kCode));
        cpu::invalidate(base, sizeof(fastfloat::kCode));
        auto ff = [&](const char* n) -> uint32_t {
            for (auto& s : fastfloat::kSyms)
                if (std::strcmp(s.name, n) == 0) return base + s.off;
            return 0;
        };
        // La suma original sigue disponible (para los casos con subnormales): sus 8 primeros bytes
        // se copian a un trampolín que salta al resto de la rutina.
        auto keep = [&](const char* sym) -> uint32_t {
            auto it = game->exports.find(sym);
            if (it == game->exports.end()) return 0;
            uint32_t t = mem::alloc(16, 8);
            mem::w32(t, mem::r32(it->second));
            mem::w32(t + 4, mem::r32(it->second + 4));
            mem::w32(t + 8, 0xe51ff004);  // ldr pc, [pc, #-4]
            mem::w32(t + 12, it->second + 8);
            cpu::invalidate(t, 16);
            return t;
        };
        uint32_t table = ff("ff_orig");
        mem::w32(table, keep("__aeabi_fadd"));
        mem::w32(table + 4, keep("__aeabi_dadd"));
        // Solo entradas que no se pisan entre sí (las rutinas vecinas que caen en estas siguen valiendo).
        const std::pair<const char*, const char*> map[] = {
            {"__aeabi_fadd", "ff_fadd"}, {"__aeabi_fmul", "ff_fmul"}, {"__aeabi_fdiv", "ff_fdiv"},
            {"__aeabi_i2f", "ff_i2f"}, {"__aeabi_ui2f", "ff_ui2f"}, {"__aeabi_f2iz", "ff_f2iz"}, {"__aeabi_f2uiz", "ff_f2uiz"},
            {"__aeabi_fcmpeq", "ff_fcmpeq"}, {"__aeabi_fcmplt", "ff_fcmplt"}, {"__aeabi_fcmple", "ff_fcmple"},
            {"__aeabi_fcmpge", "ff_fcmpge"}, {"__aeabi_fcmpgt", "ff_fcmpgt"}, {"__aeabi_fcmpun", "ff_fcmpun"},
            {"__aeabi_dadd", "ff_dadd"}, {"__aeabi_dmul", "ff_dmul"}, {"__aeabi_ddiv", "ff_ddiv"},
            {"__aeabi_i2d", "ff_i2d"}, {"__aeabi_ui2d", "ff_ui2d"}, {"__aeabi_f2d", "ff_f2d"}, {"__aeabi_d2f", "ff_d2f"},
            {"__aeabi_d2iz", "ff_d2iz"}, {"__aeabi_d2uiz", "ff_d2uiz"},
            {"__aeabi_dcmpeq", "ff_dcmpeq"}, {"__aeabi_dcmplt", "ff_dcmplt"}, {"__aeabi_dcmple", "ff_dcmple"},
            {"__aeabi_dcmpge", "ff_dcmpge"}, {"__aeabi_dcmpgt", "ff_dcmpgt"}, {"__aeabi_dcmpun", "ff_dcmpun"}};
        int done = 0;
        for (auto& [sym, name] : map) {
            auto it = game->exports.find(sym);
            uint32_t to = ff(name);
            if (it == game->exports.end() || !to || (it->second & 3)) continue;
            uint32_t at = it->second;
            mem::w32(at, 0xe51ff004);  // ldr pc, [pc, #-4]
            mem::w32(at + 4, to);
            cpu::invalidate(at, 8);
            done++;
        }
        logf("[main] coma flotante por VFP: %d rutinas", done);
    }
    g_loader.link_all([](const std::string& n, bool obj) { return host::resolve(n, obj); });
    g_loader.run_inits();
    for (auto& l : g_loader.libs()) {
        auto it = l->exports.find("JNI_OnLoad");
        if (it == l->exports.end()) continue;
        cpu::call(it->second, {host::jni_vm(), 0});
        logf("[main] JNI_OnLoad de %s listo", l->name.c_str());
    }
    if (g_loader.lib("libascella.so")) songs::install();
    if (const char* d = std::getenv("HOST_DUMP")) {  // HOST_DUMP=dir: 32 bytes tras la inicialización
        uint32_t a = uint32_t(std::strtoul(d, nullptr, 16));
        for (uint32_t i = 0; i < 32; i += 8)
            logf("[dump] %08x: %08x %08x  %s", a + i, mem::r32(a + i), mem::r32(a + i + 4), g_loader.symbolize(a + i).c_str());
        for (uint32_t i = 0; i < 32; i += 4) {
            uint32_t v = mem::r32(a + i);
            std::string s = g_loader.symbolize(v);
            if (!s.empty()) logf("[dump]   palabra %08x -> %s", v, s.c_str());
        }
    }
    if (std::getenv("HOST_DIFF")) {
        ElfLib* g = g_loader.lib("libgame.so");
        std::FILE* f = std::fopen((dir + "/libgame.so").c_str(), "rb");
        std::vector<uint8_t> orig(g->size);
        size_t n = std::fread(orig.data(), 1, orig.size(), f);
        std::fclose(f);
        std::map<std::string, int> changed;
        for (uint32_t off = 0; off < 0x3a0000 && off < n; off++)
            if (*mem::ptr<uint8_t>(g->base + off) != orig[off]) changed[g_loader.symbolize(g->base + off)]++;
        for (auto& [fn, cnt] : changed) logf("[diff] %s: %d bytes", fn.c_str(), cnt);
        // Punteros a código de Ascella guardados dentro de libgame (vtables o tablas parcheadas).
        ElfLib* a = g_loader.lib("libascella.so");
        int ptrs = 0;
        for (uint32_t off = 0x409000; off + 4 <= g->size; off += 4) {  // segmento de datos
            uint32_t v = mem::r32(g->base + off);
            if (v >= a->base && v < a->base + 0x430000) {
                if (ptrs++ < 40) logf("[diff] libgame+%x -> %s", off, g_loader.symbolize(v).c_str());
            }
        }
        logf("[diff] %d punteros de libgame apuntan a código de Ascella", ptrs);
    }
    if (apk_path.empty()) {
        logf("[main] librerías cargadas (sin APK no se arranca el juego)");
        return 0;
    }

    // ---------- Arranque como en Android ----------
    int W = 1280, H = 720;
    if (const char* sz = std::getenv("GD_SIZE")) std::sscanf(sz, "%dx%d", &W, &H);
    bool headless = std::getenv("GD_HEADLESS") != nullptr;
    int max_frames = std::getenv("GD_FRAMES") ? std::atoi(std::getenv("GD_FRAMES")) : (headless ? 300 : 0);
    auto script = parse_script(std::getenv("GD_SCRIPT"));
    bool fixed_dt = std::getenv("GD_FIXED_DT") != nullptr;  // pruebas repetibles: 1/60 s por frame
    if (fixed_dt) px::fixed_clock_start();

    double interval = 1.0 / 60;
    // FPS Bypass: valor guardado (se cambia con el botón de Opciones).
    int& fps_cap = g_fps_cap;
    songs::set_fps_hooks([] { return g_fps_cap; }, fps_cycle);
    if (std::FILE* f = std::fopen((g_data + "/fps.txt").c_str(), "r")) {
        if (std::fscanf(f, "%d", &fps_cap) != 1) fps_cap = 60;
        std::fclose(f);
    }
    if (fps_cap != 0 && (fps_cap < 60 || fps_cap > 1000)) fps_cap = 60;
#ifdef _WIN32
    timeBeginPeriod(1);  // Sleep con precisión de 1 ms
#endif
    host::java_method("org/cocos2dx/lib/Cocos2dxRenderer.setAnimationInterval (D)V", [&](uint32_t, auto& a) -> uint64_t {
        std::memcpy(&interval, &a[0], 8);
        return 0;
    });
    // Teclado: el juego abre/cierra el teclado de Android al tocar un cuadro de texto.
    static bool g_ime = false;
    host::java_method("org/cocos2dx/lib/Cocos2dxGLSurfaceView.openIMEKeyboard ()V", [](uint32_t, auto&) -> uint64_t {
        g_ime = true;
        SDL_StartTextInput();
        return 0;
    });
    host::java_method("org/cocos2dx/lib/Cocos2dxGLSurfaceView.closeIMEKeyboard ()V", [](uint32_t, auto&) -> uint64_t {
        g_ime = false;
        SDL_StopTextInput();
        return 0;
    });
    host::java_method("com/customRobTop/BaseRobTopActivity.setKeyboardState (Z)V", [](uint32_t, auto&) -> uint64_t { return 0; });
    host::java_method("org/cocos2dx/lib/Cocos2dxActivity.showMessageBox (Ljava/lang/String;Ljava/lang/String;)V",
                      [](uint32_t, auto& a) -> uint64_t {
                          const char* title = a[0] ? mem::str(uint32_t(a[0])) : "";
                          const char* msg = a[1] ? mem::str(uint32_t(a[1])) : "";
                          logf("[mensaje] %s: %s", title, msg);
                          SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, msg, nullptr);
                          return 0;
                      });
    host::java_method("org/cocos2dx/lib/Cocos2dxActivity.terminateProcess ()V", [](uint32_t, auto&) -> uint64_t {
        logf("[main] el juego pidió cerrarse");
        std::exit(0);
    });

    SDL_Window* win = nullptr;
#ifdef _WIN32
    bool desktop_gl = !(std::getenv("GD_GL") && std::string(std::getenv("GD_GL")) == "es");
#else
    bool desktop_gl = std::getenv("GD_GL") && std::string(std::getenv("GD_GL")) == "desktop";
#endif
    if (headless && !desktop_gl) {
        if (!host::gl_init_offscreen(W, H)) return 2;
    } else {
        SDL_SetMainReady();
        if (!desktop_gl) SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            logf("[main] SDL_Init: %s", SDL_GetError());
            return 2;
        }
        if (desktop_gl) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        } else {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        }
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#ifdef __ANDROID__
        uint32_t flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI;
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
        {
            SDL_DisplayMode dm;
            if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0) {
                W = std::max(dm.w, dm.h);
                H = std::min(dm.w, dm.h);
            }
        }
#else
        uint32_t flags = SDL_WINDOW_OPENGL | (headless ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE);
#endif
        win = SDL_CreateWindow("1.3 GDPS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, flags);
        if (!win || !SDL_GL_CreateContext(win)) {
            logf("[main] no se pudo crear la ventana: %s", SDL_GetError());
            return 2;
        }
        g_vsync = set_vsync(win, fps_cap) && !headless;
        g_win = win;
        if (!host::gl_load([](const char* n) { return SDL_GL_GetProcAddress(n); }, desktop_gl)) {
            logf("[main] faltan funciones de OpenGL");
            return 2;
        }
        // El juego dibuja siempre a W x H; la ventana puede tener cualquier tamaño.
        g_game_w = float(W);
        g_game_h = float(H);
        bool scalable = !headless && host::gl_offscreen_begin(W, H, res_scale());
        if (!scalable) SDL_SetWindowResizable(win, SDL_FALSE);
        else SDL_SetWindowMinimumSize(win, W / 4, H / 4);
        logf("[main] ventana %dx%d%s", W, H, scalable ? " (redimensionable)" : "");
    }
    SDL_StopTextInput();  // solo se activa cuando el juego abre su teclado
    host::audio_init();
    host::text_init();

    uint32_t env = host::jni_env();
    call_native("org/cocos2dx/lib/Cocos2dxActivity", "nativeSetPaths", {env, 0, mem::strdup(apk_path.c_str())});
    call_native(input::kRenderer, "nativeInit", {env, 0, uint32_t(W), uint32_t(H)});
    logf("[main] juego iniciado");

    // Detector de cuelgues: si un frame tarda más de 5 s, apunta dónde está el juego.
    static std::atomic<int> g_frame{0};
    std::thread([] {
        int last = -1, stuck = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int f = g_frame.load();
            if (f == last) {
                if (++stuck == 5 || stuck % 30 == 0) {
                    logf("[cuelgue] el frame %d lleva %d s sin terminar", f, stuck);
                    cpu::dump_main_thread();
                }
            } else {
                stuck = 0;
                last = f;
            }
        }
    }).detach();

    // Perder/recuperar el foco = la app de Android pasa a segundo plano y vuelve.
    auto set_focus = [&](bool focused) {
        if (!focused) {
            host::audio_focus(false);
            call_native(input::kRenderer, "nativeOnPause", {env, 0});
        } else {
            call_native(input::kRenderer, "nativeOnResume", {env, 0});
            host::audio_focus(true);
        }
    };

    using clock = std::chrono::steady_clock;
    auto next_frame = clock::now();
    float mx = 0, my = 0;
    // Rectángulo donde se dibuja el juego dentro de la ventana (en píxeles de la ventana).
    float view_x = 0, view_y = 0, view_w = float(W), view_h = float(H);
    auto to_game = [&](int x, int y, float& gx, float& gy) {
        gx = std::clamp((float(x) - view_x) * W / view_w, 0.f, float(W - 1));
        gy = std::clamp((float(y) - view_y) * H / view_h, 0.f, float(H - 1));
    };
    for (int frame = 1; max_frames == 0 || frame <= max_frames; frame++) {
        // Eventos de la ventana.
        if (win) {
#ifdef __ANDROID__
            // Ventana de Newgrounds: abre el teclado del móvil al aparecer y lo cierra al irse.
            static bool ng_kb = false;
            bool ng = songs::editor_open();
            if (ng != ng_kb && !g_ime) {
                if (ng) SDL_StartTextInput(); else SDL_StopTextInput();
            }
            ng_kb = ng;
#endif
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                case SDL_QUIT: input::quit();
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) set_focus(false);
                    if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) set_focus(true);
                    if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        // Ajusta la ventana a la proporción del juego (sin barras negras).
                        // Maximizada o en pantalla completa no se puede: ahí sí quedan bandas.
                        uint32_t wf = SDL_GetWindowFlags(win);
                        if (!(wf & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP))) {
                            int ww = e.window.data1, wh = e.window.data2;
                            static int last_w = W, last_h = H;
                            // Manda la dimensión que más ha cambiado.
                            bool by_width = std::abs(ww - last_w) * H >= std::abs(wh - last_h) * W;
                            int nw = by_width ? ww : int(std::lround(double(wh) * W / H));
                            int nh = by_width ? int(std::lround(double(ww) * H / W)) : wh;
                            if (std::abs(nw - ww) > 1 || std::abs(nh - wh) > 1) SDL_SetWindowSize(win, nw, nh);
                            last_w = nw;
                            last_h = nh;
                        }
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
#ifdef __ANDROID__
                    if (!g_ime && songs::editor_open()) SDL_StartTextInput();  // vuelve a mostrar el teclado
#endif
                    if (e.button.button == SDL_BUTTON_LEFT) { to_game(e.button.x, e.button.y, mx, my); input::touch_begin(mx, my); }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (e.button.button == SDL_BUTTON_LEFT) { to_game(e.button.x, e.button.y, mx, my); input::touch_end(mx, my); }
                    break;
                case SDL_MOUSEMOTION:
                    to_game(e.motion.x, e.motion.y, mx, my);
                    if (input::down) input::touch_move(mx, my);
                    break;
                case SDL_TEXTINPUT:
                    if (!g_ime && songs::editor_open()) {  // teclado del móvil en la ventana de Newgrounds
                        for (const char* t = e.text.text; *t; t++)
                            if (*t >= '0' && *t <= '9') songs::key(*t);
                        break;
                    }
                    if (g_ime) call_native(input::kRenderer, "nativeInsertText", {env, 0, mem::strdup(e.text.text)});
                    break;
                case SDL_KEYDOWN:
                    if (g_ime) {  // escribiendo en un cuadro de texto
                        SDL_Keycode k = e.key.keysym.sym;
                        if (k == SDLK_BACKSPACE) call_native(input::kRenderer, "nativeDeleteBackward", {env, 0});
                        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) call_native(input::kRenderer, "nativeInsertText", {env, 0, mem::strdup("\n")});
                        else if (k == SDLK_v && (e.key.keysym.mod & KMOD_CTRL) && SDL_HasClipboardText()) {
                            char* clip = SDL_GetClipboardText();
                            call_native(input::kRenderer, "nativeInsertText", {env, 0, mem::strdup(clip)});
                            SDL_free(clip);
                        } else if (k == SDLK_ESCAPE) input::key_back();
                        break;
                    }
                    if (songs::editor_open()) {  // ventana de Newgrounds: escribir el ID
                        SDL_Keycode k = e.key.keysym.sym;
                        int ch = k >= SDLK_0 && k <= SDLK_9 ? '0' + (k - SDLK_0)
                               : k >= SDLK_KP_1 && k <= SDLK_KP_9 ? '1' + (k - SDLK_KP_1)
                               : k == SDLK_KP_0 ? '0' : k == SDLK_BACKSPACE ? '\b'
                               : k == SDLK_RETURN || k == SDLK_KP_ENTER ? '\n' : k == SDLK_ESCAPE ? 27 : 0;
#ifdef __ANDROID__
                        if (ch >= '0' && ch <= '9') break;  // los números llegan como texto (SDL_TEXTINPUT)
#endif
                        if (ch && songs::key(ch)) break;
                    }
                    if (e.key.repeat) break;
                    // Atajos de Ascella (y cualquier tecla que el juego quiera): KeyEvent de Android.
                    // Las teclas de salto van solo como toque: con los keybinds de Ascella activos contarían doble.
                    if (int ak = is_jump_key(e.key.keysym.sym) ? 0 : android_keycode(e.key.keysym.sym))
                        call_native(input::kRenderer, "nativeKeyDown", {host::jni_env(), 0, uint32_t(ak)});
                    if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_AC_BACK) input::key_back();
                    else if (is_jump_key(e.key.keysym.sym))
                        input::touch_begin(W * 0.02f, H * 0.5f);  // zona sin botones: salta en el juego
                    else if (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
                        bool fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                        SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    } else if (e.key.keysym.sym == SDLK_F12) {
                        char name[64];
                        std::snprintf(name, sizeof name, "/captura_%d.png", frame);
                        save_png(g_data + name, W, H);
                    }
                    break;
                case SDL_KEYUP:
                    if (g_ime) break;
                    if (int ak = is_jump_key(e.key.keysym.sym) ? 0 : android_keycode(e.key.keysym.sym))
                        call_native(input::kRenderer, "nativeKeyUp", {host::jni_env(), 0, uint32_t(ak)});
                    if (is_jump_key(e.key.keysym.sym))
                        input::touch_end(W * 0.02f, H * 0.5f);
                    break;
                }
            }
        }
        // Guion de pruebas.
        for (auto& st : script) {
            if (st.frame != frame) continue;
            if (st.action == "tap") { input::touch_begin(st.x, st.y); input::touch_end(st.x, st.y); }
            else if (st.action == "down") input::touch_begin(st.x, st.y);
            else if (st.action == "up") input::touch_end(st.x, st.y);
            else if (st.action == "key_back") input::key_back();
            else if (st.action == "song") for (char ch : std::to_string(int(st.x))) songs::key(ch);
            else if (st.action == "quit") input::quit();
            else if (st.action == "blur") set_focus(false);
            else if (st.action == "type") call_native(input::kRenderer, "nativeInsertText", {env, 0, mem::strdup("gdhost")});
            else if (st.action == "focus") set_focus(true);
            else if (st.action == "shot") {
                char name[64];
                std::snprintf(name, sizeof name, "/frame_%04d.png", frame);
                save_png(g_data + name, W, H);
            }
        }

        songs::tick();
        cpu::profile_report();
        if (std::getenv("GD_PROFILE")) {
            static auto t0 = clock::now();
            static int f0 = 0;
            if (clock::now() - t0 >= std::chrono::seconds(5)) {
                logf("[perf] %.0f FPS", (frame - f0) / std::chrono::duration<double>(clock::now() - t0).count());
                t0 = clock::now();
                f0 = frame;
            }
        }
        if (fixed_dt) px::fixed_clock_advance(16666667);
        call_native(input::kRenderer, "nativeRender", {env, 0});
        songs::trace_player(frame);
        g_frame++;
        if (headless && max_frames && frame == max_frames) save_png(g_data + "/ultimo.png", W, H);
        if (win) {
            int dw, dh, ww, wh, x, y, w, h;
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            SDL_GetWindowSize(win, &ww, &wh);
            host::gl_present(dw, dh, x, y, w, h);
            // de píxeles del framebuffer a coordenadas de ventana (pantallas HiDPI)
            float sx = float(ww) / float(dw), sy = float(wh) / float(dh);
            view_x = x * sx;
            view_y = y * sy;
            view_w = w * sx;
            view_h = h * sy;
            SDL_GL_SwapWindow(win);
        }
        else host::gl_swap();

        double step = fps_cap > 60 ? 1.0 / fps_cap : interval;
        if (fps_cap != 0) next_frame += std::chrono::microseconds(int64_t(step * 1e6));
        auto now = clock::now();
        if (g_vsync && fps_cap == 60) {
            next_frame = now;  // el intercambio de buffers ya esperó a la pantalla: una segunda espera alternaría 16 y 33 ms
        } else if (fps_cap != 0 && next_frame > now) {
            // dormir casi todo y esperar activamente el último ms (precisión para >60 FPS)
            auto spin = std::chrono::microseconds(1500);
            if (next_frame - now > spin) std::this_thread::sleep_for(next_frame - now - spin);
            while (clock::now() < next_frame) std::this_thread::yield();
        } else {
            next_frame = now;
        }
    }
    logf("[main] fin tras %d frames", max_frames);
    input::quit();  // los hilos del juego siguen vivos: salimos sin esperarlos
}
