// Canciones de Newgrounds (como en GD 1.9).
//
// Un nivel guarda el ID de Newgrounds en su número de canción (audioTrack). Los números
// menores que kFirstNG son las canciones oficiales. Ascella busca la canción en
// <almacenamiento>/13gdps/songs/<audioTrack+1>; el host redirige esa ruta a
// data/saved_songs/<id>.mp3 y, si no existe, la descarga de Newgrounds.
//
// En el editor (ajustes del nivel), escribir números con el teclado cambia la canción.
#include "songs.h"
#include "gamesheet_frames.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>

#include "cpu.h"
#include "log.h"
#include "memory.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif
#ifdef __ANDROID__
#include <jni.h>
#include <SDL2/SDL_system.h>
#endif

namespace songs {
// Los parches de este archivo son para libgame de 1.3 GDPS (con Ascella); en otros juegos no se aplican.
static bool g_on = false;
std::atomic<bool> g_song_failed{false};  // falló la descarga de la canción del nivel que se carga

namespace {

std::string g_dir = "data/saved_songs";
std::mutex g_mu;
std::condition_variable g_cv;
std::set<int> g_busy, g_failed;
struct Info {
    std::string title, author;
    int secs = 0;
};
std::map<int, Info> g_info;           // id -> datos de la canción

uint32_t g_title_orig = 0;  // getAudioTitle de Ascella
uint32_t g_layer = 0, g_layer_vt = 0;

// ---------------------------------------------------------------- HTTP
// Servidor de colores de nombre (Cloudflare). También se puede poner en data/colores_url.txt.
const char* kColorServer = "https://colors-13gdps.mrfierrocarrilgames.workers.dev";
const char* kUA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
// El servidor de GD (boomlings.com) rechaza las peticiones con User-Agent: GD no manda ninguno.
const char* ua_for(const std::string& url) { return url.find("boomlings.com") != std::string::npos ? "" : kUA; }

#ifdef _WIN32
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

bool http_get(const std::string& url, std::string& out, int& status, const std::string& post = "") {
    out.clear();
    status = 0;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    std::wstring wurl = widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    HINTERNET s = WinHttpOpen(widen(ua_for(url)).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!s) s = WinHttpOpen(widen(ua_for(url)).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!s) return false;
    bool ok = false;
    if (HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0)) {
        DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        if (HINTERNET r = WinHttpOpenRequest(c, post.empty() ? L"GET" : L"POST", path, nullptr, nullptr, nullptr, flags)) {
            const wchar_t* hdr = L"Content-Type: application/x-www-form-urlencoded\r\n";
            if (WinHttpSendRequest(r, post.empty() ? nullptr : hdr, post.empty() ? 0 : DWORD(-1L),
                                   post.empty() ? nullptr : (void*)post.data(), DWORD(post.size()), DWORD(post.size()), 0) && WinHttpReceiveResponse(r, nullptr)) {
                DWORD code = 0, len = sizeof code;
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr);
                status = int(code);
                char buf[65536];
                DWORD got = 0;
                while (WinHttpReadData(r, buf, sizeof buf, &got) && got) out.append(buf, got);
                ok = true;
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}
#elif defined(__ANDROID__)
// Android: no hay curl; se usa app.gdps13.port.Net (HttpURLConnection) por JNI.
bool http_get(const std::string& url, std::string& out, int& status, const std::string& post = "") {
    out.clear();
    status = 0;
    JavaVM* vm = nullptr;
    JNIEnv* env0 = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env0 || env0->GetJavaVM(&vm) != JNI_OK) return false;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }
    static jclass cls = [&] {
        // FindClass desde un hilo propio no ve las clases de la app: se usa el cargador de la actividad.
        jobject act = static_cast<jobject>(SDL_AndroidGetActivity());
        jclass ac = env->GetObjectClass(act);
        jobject loader = env->CallObjectMethod(ac, env->GetMethodID(env->FindClass("java/lang/Class"), "getClassLoader", "()Ljava/lang/ClassLoader;"));
        jclass lc = env->FindClass("java/lang/ClassLoader");
        jstring name = env->NewStringUTF("app.gdps13.port.Net");
        jclass c = static_cast<jclass>(env->CallObjectMethod(loader, env->GetMethodID(lc, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;"), name));
        return static_cast<jclass>(env->NewGlobalRef(c));
    }();
    bool ok = false;
    if (cls) {
        jmethodID m = env->GetStaticMethodID(cls, "fetch", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[I)[B");
        jintArray st = env->NewIntArray(1);
        jstring ju = env->NewStringUTF(url.c_str()), jp = env->NewStringUTF(post.c_str()), ja = env->NewStringUTF(ua_for(url));
        auto data = static_cast<jbyteArray>(env->CallStaticObjectMethod(cls, m, ju, jp, ja, st));
        if (env->ExceptionCheck()) env->ExceptionClear();
        jint code = 0;
        env->GetIntArrayRegion(st, 0, 1, &code);
        status = code;
        if (data) {
            jsize n = env->GetArrayLength(data);
            out.resize(size_t(n));
            env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(out.data()));
            ok = true;
        }
        env->DeleteLocalRef(ju);
        env->DeleteLocalRef(jp);
        env->DeleteLocalRef(ja);
        env->DeleteLocalRef(st);
        if (data) env->DeleteLocalRef(data);
    }
    if (attached) vm->DetachCurrentThread();
    return ok;
}
#else
bool http_get(const std::string& url, std::string& out, int& status, const std::string& post = "") {
    out.clear();
    status = 0;
    std::string cmd = "curl -sSL -m 120 -A '" + std::string(ua_for(url)) + "' -w '\\n%{http_code}' '" + url + "'";
    if (!post.empty()) cmd += " -d '" + post + "'";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    size_t nl = out.rfind('\n');
    if (nl == std::string::npos) return false;
    status = std::atoi(out.c_str() + nl + 1);
    out.resize(nl);
    return status != 0;
}
#endif

bool is_audio(const std::string& d) {
    if (d.size() < 1024) return false;
    if (d.compare(0, 3, "ID3") == 0 || d.compare(0, 4, "OggS") == 0 || d.compare(0, 4, "RIFF") == 0) return true;
    return uint8_t(d[0]) == 0xFF && (uint8_t(d[1]) & 0xE0) == 0xE0;
}

std::string html_unescape(std::string s) {
    static const std::pair<const char*, const char*> rep[] = {
        {"&amp;", "&"}, {"&#039;", "'"}, {"&#39;", "'"}, {"&quot;", "\""}, {"&lt;", "<"}, {"&gt;", ">"}};
    for (auto& [a, b] : rep)
        for (size_t p; (p = s.find(a)) != std::string::npos;) s.replace(p, std::strlen(a), b);
    return s;
}

// Busca en la página de Newgrounds el enlace al mp3 y el título.
std::string json_field(const std::string& html, const char* key) {
    std::string k = std::string("\"") + key + "\":\"";
    size_t p = html.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    std::string v;
    for (size_t i = p; i < html.size() && html[i] != '"'; i++) {
        if (html[i] == '\\' && i + 1 < html.size()) {
            char n = html[++i];
            if (n == 'u' && i + 4 < html.size()) {  // \u00e9 -> UTF-8
                unsigned cp = unsigned(std::strtoul(html.substr(i + 1, 4).c_str(), nullptr, 16));
                i += 4;
                if (cp < 0x80) v += char(cp);
                else if (cp < 0x800) { v += char(0xC0 | (cp >> 6)); v += char(0x80 | (cp & 0x3F)); }
                else { v += char(0xE0 | (cp >> 12)); v += char(0x80 | ((cp >> 6) & 0x3F)); v += char(0x80 | (cp & 0x3F)); }
            } else v += n;
        } else v += html[i];
    }
    return html_unescape(v);
}

void parse_listen_page(const std::string& html, std::string& mp3, std::string& title, std::string& author) {
    author = json_field(html, "artist");
    if (author.empty()) {
        size_t a = html.find("<meta name=\"author\" content=\"");
        if (a != std::string::npos) {
            a += 30;
            size_t e = html.find('"', a);
            if (e != std::string::npos) author = html_unescape(html.substr(a, e - a));
        }
    }
    size_t p = html.find("audio.ngfiles.com");
    if (p != std::string::npos) {
        size_t a = html.rfind("http", p);
        size_t b = html.find_first_of("\"' <", p);
        if (a != std::string::npos && b != std::string::npos && p - a < 16) {
            mp3 = html.substr(a, b - a);
            for (size_t q; (q = mp3.find("\\/")) != std::string::npos;) mp3.replace(q, 2, "/");
        }
    }
    size_t t = html.find("<title>");
    if (t != std::string::npos) {
        size_t e = html.find("</title>", t);
        if (e != std::string::npos) title = html_unescape(html.substr(t + 7, e - t - 7));
    }
    for (const char* cut : {" | Newgrounds", " - Newgrounds"})
        if (size_t c = title.find(cut); c != std::string::npos) title.resize(c);
    while (!title.empty() && (title.back() == ' ' || title.back() == '\n')) title.pop_back();
}

std::string mp3_path(int id) { return g_dir + "/" + std::to_string(id) + ".mp3"; }
std::string info_path(int id) { return g_dir + "/" + std::to_string(id) + ".txt"; }

void save_file(const std::string& path, const std::string& data) {
    std::string tmp = path + ".part";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(data.data(), std::streamsize(data.size()));
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
}

int audio_seconds(const std::string& path) {
#if SDL_MIXER_VERSION_ATLEAST(2, 6, 0)
    if (Mix_Music* m = Mix_LoadMUS(path.c_str())) {
        double d = Mix_MusicDuration(m);
        Mix_FreeMusic(m);
        if (d > 0) return int(d + 0.5);
    }
#endif
    return 0;
}

void save_info(int id, const Info& in) {
    save_file(info_path(id), in.title + "\n" + in.author + "\n" + std::to_string(in.secs) + "\n");
}

bool load_info(int id, Info& in) {
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (auto it = g_info.find(id); it != g_info.end()) { in = it->second; return true; }
    }
    std::ifstream f(info_path(id), std::ios::binary);
    if (!f) return false;
    std::string secs;
    std::getline(f, in.title);
    std::getline(f, in.author);
    std::getline(f, secs);
    in.secs = std::atoi(secs.c_str());
    if (in.title.empty()) return false;
    std::lock_guard<std::mutex> l(g_mu);
    g_info[id] = in;
    return true;
}

bool have_song(int id) {
    std::error_code ec;
    return std::filesystem::exists(mp3_path(id), ec);
}

std::string url_decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) { o += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)); i += 2; }
        else o += s[i];
    }
    return o;
}

// Plan B: el servidor oficial de Geometry Dash (el mismo que usa GD para Newgrounds).
// Respuesta: 1~|~ID~|~2~|~Nombre~|~3~|~...~|~4~|~Autor~|~5~|~MB~|~...~|~10~|~URL
void gd_song_info(int id, std::string& mp3, Info& in) {
    std::string r;
    int st = 0;
    if (!http_get("https://www.boomlings.com/database/getGJSongInfo.php", r, st,
                  "songID=" + std::to_string(id) + "&secret=Wmfd2893gb7") || st != 200 || r.find("~|~") == std::string::npos) {
        logf("[canciones] %d: servidor de GD sin datos (HTTP %d) %.40s", id, st, r.c_str());
        return;
    }
    std::vector<std::string> f;
    for (size_t a = 0;;) {
        size_t b = r.find("~|~", a);
        f.push_back(r.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 3;
    }
    for (size_t i = 0; i + 1 < f.size(); i += 2) {
        if (f[i] == "2" && in.title.empty()) in.title = f[i + 1];
        if (f[i] == "4" && in.author.empty()) in.author = f[i + 1];
        if (f[i] == "10" && mp3.empty()) mp3 = url_decode(f[i + 1]);
    }
}

void download(int id) {
    std::string html, mp3, data;
    Info in;
    int st = 0;
    std::string listen = "https://www.newgrounds.com/audio/listen/" + std::to_string(id);
    if (http_get(listen, html, st) && st == 200) parse_listen_page(html, mp3, in.title, in.author);
    else logf("[canciones] %d: no se pudo abrir la página de Newgrounds (HTTP %d)", id, st);
    bool ok = false;
    if (!mp3.empty() && http_get(mp3, data, st) && is_audio(data)) ok = true;
    if (!ok && http_get("https://www.newgrounds.com/audio/download/" + std::to_string(id), data, st) && is_audio(data)) ok = true;
    if (!ok) {
        std::string mp3b;
        gd_song_info(id, mp3b, in);
        if (!mp3b.empty() && http_get(mp3b, data, st) && is_audio(data)) ok = true;
        else if (!mp3b.empty()) logf("[canciones] %d: no se pudo bajar %s (HTTP %d)", id, mp3b.c_str(), st);
    }
    if (ok) {
        save_file(mp3_path(id), data);
        in.secs = audio_seconds(mp3_path(id));
        if (in.title.empty()) in.title = "Newgrounds " + std::to_string(id);
        save_info(id, in);
        logf("[canciones] %d descargada (%zu KB, %d s): %s - %s", id, data.size() / 1024, in.secs, in.title.c_str(), in.author.c_str());
    } else {
        logf("[canciones] %d: no se pudo descargar el audio (HTTP %d)", id, st);
    }
    std::lock_guard<std::mutex> l(g_mu);
    if (ok) g_info[id] = in;
    g_busy.erase(id);
    if (!ok) g_failed.insert(id);
    g_cv.notify_all();
}

void start_download(int id, bool retry = false) {
    if (have_song(id)) return;
    std::lock_guard<std::mutex> l(g_mu);
    if (retry) g_failed.erase(id);
    if (g_busy.count(id) || g_failed.count(id)) return;
    g_busy.insert(id);
    std::thread(download, id).detach();
}

bool busy(int id) {
    std::lock_guard<std::mutex> l(g_mu);
    return g_busy.count(id) != 0;
}

std::string title_of(int id) {
    Info in;
    if (load_info(id, in)) return in.title;
    start_download(id);
    return busy(id) ? "Newgrounds (descargando...)" : "Newgrounds";
}

// Cadena fija en memoria del invitado (una por texto distinto).
uint32_t gstr(const std::string& t) {
    static std::map<std::string, uint32_t> cache;
    static std::mutex mu;
    std::lock_guard<std::mutex> l(mu);
    auto it = cache.find(t);
    if (it != cache.end()) return it->second;
    return cache[t] = mem::strdup(t.c_str());
}


// Escribe en el código del invitado un salto Thumb BLX desde `site` hasta `target` (ARM).
void write_blx(uint32_t site, uint32_t target) {
    int32_t off = int32_t(target - ((site + 4) & ~3u));
    uint32_t S = (off >> 24) & 1, I1 = (off >> 23) & 1, I2 = (off >> 22) & 1;
    uint32_t J1 = (~I1 ^ S) & 1, J2 = (~I2 ^ S) & 1;
    mem::w16(site, uint16_t(0xf000 | (S << 10) | ((off >> 12) & 0x3ff)));
    mem::w16(site + 2, uint16_t(0xc000 | (J1 << 13) | (J2 << 11) | ((off >> 1) & 0x7fe)));
    cpu::invalidate(site, 4);
}

// ---------------------------------------------------------------- funciones del juego
// Direcciones en libgame.so (cargada en 0x01000000); +1 = Thumb.
constexpr uint32_t G(uint32_t off) { return 0x01000000 + off + 1; }
constexpr uint32_t kGetAudioTitle = 0x011432d4;
constexpr uint32_t kUpdateLabelCall = 0x01150378;  // bl getAudioTitle dentro de updateAudioLabel
constexpr uint32_t kLabelFmtLit = 0x01150418;      // literal del formato "%i: %s" (relativo a pc)
constexpr uint32_t kLabelFmtPC = 0x01150388;
constexpr uint32_t kVeneer = 0x0145ffe0;

uint32_t f2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
float u2f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

uint32_t call(uint32_t fn, std::initializer_list<uint32_t> a) { return uint32_t(cpu::call(fn, a)); }

uint32_t point(float x, float y) {  // cocos2d::CCPoint (en esta versión hereda de CCObject)
    uint32_t p = mem::alloc(0x20);
    call(G(0x19f050), {p, f2u(x), f2u(y)});
    return p;
}

void win_size(float& w, float& h) {
    uint32_t d = call(G(0x1a4420), {});  // CCDirector::sharedDirector
    uint32_t s = mem::alloc(0x20);
    call(G(0x1a3f10), {s, d});  // getWinSize (resultado por puntero)
    w = u2f(mem::r32(s + 0x18));
    h = u2f(mem::r32(s + 0x1c));
    mem::free(s);
}

std::string ascii(const std::string& t);
uint32_t label(const std::string& text, const char* font) {
    static uint32_t zero = point(0, 0);
    return call(G(0x1c6d9c), {gstr(ascii(text)), gstr(font), mem::r32(0x01152fe8), 0, zero});
}
// Las fuentes del juego solo tienen ASCII: se quitan acentos y lo demás se omite.
std::string ascii(const std::string& t) {
    static const std::pair<const char*, char> rep[] = {
        {"á", 'a'}, {"é", 'e'}, {"í", 'i'}, {"ó", 'o'}, {"ú", 'u'}, {"ñ", 'n'}, {"ü", 'u'}, {"Á", 'A'}, {"É", 'E'},
        {"Í", 'I'}, {"Ó", 'O'}, {"Ú", 'U'}, {"Ñ", 'N'}, {"à", 'a'}, {"è", 'e'}, {"ö", 'o'}, {"ä", 'a'}, {"ç", 'c'}};
    std::string o;
    for (size_t i = 0; i < t.size();) {
        unsigned char ch = t[i];
        if (ch >= 32 && ch < 127) { o += char(ch); i++; continue; }
        bool done = false;
        for (auto& [u, a] : rep)
            if (t.compare(i, std::strlen(u), u) == 0) { o += a; i += std::strlen(u); done = true; break; }
        if (done) continue;
        i++;
        while (i < t.size() && (uint8_t(t[i]) & 0xC0) == 0x80) i++;  // resto del carácter UTF-8
    }
    if (o.find_first_not_of(' ') == std::string::npos) o = " ";
    return o;
}
void label_text(uint32_t l, const std::string& t) { call(G(0x1c3d88), {l, gstr(ascii(t))}); }
void color(uint32_t l, uint8_t r, uint8_t g, uint8_t b) {
    static uint32_t c = mem::alloc(4);
    mem::w32(c, r | (g << 8) | (b << 16));
    call(G(0x1c3fa4), {l, c});
}
void pos(uint32_t n, float x, float y) { call(G(0x19c8e4), {n, f2u(x), f2u(y)}); }
void scale(uint32_t n, float s) { call(G(0x19c700), {n, f2u(s)}); }
void visible(uint32_t n, bool v) { call(G(0x19c774), {n, v}); }
void add(uint32_t parent, uint32_t child, int z = 0) { call(G(0x19d1e8), {parent, child, uint32_t(z)}); }
void remove(uint32_t n) { call(G(0x19c7dc), {n, 1}); }

// Botón verde con letras doradas (como "OK") que llama a `cb` al pulsarlo.
uint32_t button(uint32_t menu, const std::string& text, uint32_t target, uint32_t cb, float x, float y, float sc = 1.f) {
    uint32_t spr = call(G(0x102e40), {gstr(text)});  // ButtonSprite::create
    if (sc != 1.f) scale(spr, sc);
    // CCMenuItemSpriteExtra::create(normal, selected, target, selector); selector = {ptr, adj}
    uint32_t item = call(G(0x107b84), {spr, 0, target, cb, 0});
    add(menu, item);
    pos(item, x, y);
    return item;
}

uint32_t new_menu() {
    uint32_t m = call(G(0x1cf058), {});  // CCMenu::create
    pos(m, 0, 0);
    return m;
}

// ---------------------------------------------------------------- ventana de Newgrounds

struct Popup {
    uint32_t alert = 0, alert_vt = 0, id_label = 0, status = 0, name = 0, author = 0;
    std::string typed;
    int searching = 0, found = 0;
} g_pop;

enum Action { kNone, kOpen, kSearch, kSelect, kInfoDownload };
Action g_action = kNone;  // las pulsaciones se atienden en tick(), fuera del menú

bool alive(uint32_t obj, uint32_t vt) { return obj && mem::r32(obj) == vt; }
bool layer_alive() { return alive(g_layer, g_layer_vt); }
bool popup_alive() { return alive(g_pop.alert, g_pop.alert_vt); }

uint32_t children(uint32_t node, std::vector<uint32_t>& out) {
    out.clear();
    uint32_t arr = mem::r32(node + 0x54);
    if (!arr) return 0;
    uint32_t n = call(G(0x1a2e20), {arr});
    for (uint32_t i = 0; i < n; i++) out.push_back(call(G(0x1a2e44), {arr, i}));
    return n;
}
float px(uint32_t n) { return u2f(mem::r32(n + 0x44)); }
float py(uint32_t n) { return u2f(mem::r32(n + 0x48)); }
float width(uint32_t n) { return u2f(mem::r32(n + 0xa4 + 0x18)); }

// Menú de botones de la ventana de ajustes (donde están BG/Ground) y la capa animada que lo contiene.
bool settings_nodes(uint32_t layer, uint32_t& menu, uint32_t& inner) {
    const uint32_t item_vt = 0x01000000 + 0x4107a8 + 8;
    uint32_t n = mem::r32(layer + 0x1d4);
    for (int i = 0; i < 4 && n && mem::r32(n) != item_vt; i++) n = call(G(0x19c78c), {n});
    if (!n || mem::r32(n) != item_vt) return false;
    menu = call(G(0x19c78c), {n});
    inner = menu ? call(G(0x19c78c), {menu}) : 0;
    return menu && inner;
}

// Canción de Newgrounds de cada LevelSettingsObject (cabecera del nivel: "...,kNG,<id>").
// El número de canción del nivel queda con la oficial elegida (la que oyen los demás).
std::map<uint32_t, int> g_lso_ng;
std::map<uint32_t, int> g_lso_track;  // última canción oficial vista en la etiqueta
int g_parsed_ng = 0;                  // último kNG leído de una cabecera
int g_play_ng = 0;                    // canción del nivel que se acaba de cargar

int lso_ng(uint32_t lso) {
    auto it = g_lso_ng.find(lso);
    return it == g_lso_ng.end() ? 0 : it->second;
}

int parse_ng(const char* text) {  // busca "kNG,<id>" en la cabecera (antes del primer ';')
    if (!text) return 0;
    for (const char* p = text; *p && *p != ';'; p++)
        if (std::strncmp(p, "kNG,", 4) == 0 && (p == text || p[-1] == ',')) return std::atoi(p + 4);
    return 0;
}

void set_ng(int id) {
    if (!layer_alive()) return;
    uint32_t lso = mem::r32(g_layer + 0x1dc);
    if (id) g_lso_ng[lso] = id;
    else g_lso_ng.erase(lso);
    g_play_ng = id;
    call(G(0x150358), {g_layer});  // updateAudioLabel
}

std::string mmss(int s) {
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

void popup_refresh() {
    if (!popup_alive()) return;
    label_text(g_pop.id_label, g_pop.typed.empty() ? " " : g_pop.typed);
    if (g_pop.searching) {
        label_text(g_pop.status, "Searching...");
        color(g_pop.status, 255, 255, 255);
        label_text(g_pop.name, " ");
        label_text(g_pop.author, " ");
    } else if (g_pop.found) {
        Info in;
        load_info(g_pop.found, in);
        label_text(g_pop.status, "Found!");
        color(g_pop.status, 80, 255, 80);
        label_text(g_pop.name, in.title + (in.secs ? " (" + mmss(in.secs) + ")" : ""));
        label_text(g_pop.author, in.author.empty() ? " " : in.author);
    }
}

void popup_error() {
    label_text(g_pop.status, "Error. Not found");
    color(g_pop.status, 255, 70, 70);
    label_text(g_pop.name, " ");
    label_text(g_pop.author, " ");
}

// Ventana con el mismo estilo y animación que las del juego: un FLAlertLayer
// ("Close" = botón 1, "Select" = botón 2) al que se le añaden los controles.
void popup_open() {
    if (popup_alive() || !layer_alive()) return;
    static uint32_t proto = [] {
        uint32_t vt = mem::alloc(16), obj = mem::alloc(8);
        uint32_t th = cpu::add_thunk("Newgrounds: FLAlert_Clicked", [](cpu::Cpu& c) {
            if (cpu::reg(c, 2)) g_action = kSelect;  // botón 2
        });
        for (int i = 0; i < 4; i++) mem::w32(vt + 4 * i, th);
        mem::w32(obj, vt);
        return obj;
    }();
    g_pop = Popup{};
    uint32_t a = call(G(0x1135fc), {proto, gstr("Newgrounds"), gstr("\n\n\n\n\n\n\n"), gstr("Close"), gstr("Select"), f2u(360)});
    if (!a) return;
    g_pop.alert = a;
    g_pop.alert_vt = mem::r32(a);
    uint32_t layer = mem::r32(a + 0x1c8), bmenu = mem::r32(a + 0x1b0);
    float w, h;
    win_size(w, h);
    float cx = w / 2, cy = h / 2;
    float mx = px(bmenu), my = py(bmenu);  // los botones van en coordenadas del menú

    uint32_t sid = label("Song ID:", "bigFont.fnt");
    scale(sid, 0.55f);
    pos(sid, cx - 105, cy + 52);
    add(layer, sid, 5);
    uint32_t boxc = mem::alloc(4);
    mem::w32(boxc, 0x64000000);
    uint32_t box = call(G(0x1c99c8), {boxc, f2u(110), f2u(28)});
    pos(box, cx - 50, cy + 38);
    add(layer, box, 5);
    g_pop.id_label = label(" ", "bigFont.fnt");
    scale(g_pop.id_label, 0.55f);
    pos(g_pop.id_label, cx + 5, cy + 52);
    add(layer, g_pop.id_label, 6);
    static uint32_t on_search = cpu::add_thunk("Newgrounds: Search", [](cpu::Cpu&) { g_action = kSearch; });
    button(bmenu, "Search", a, on_search, cx + 110 - mx, cy + 52 - my, 0.7f);

    g_pop.status = label(" ", "bigFont.fnt");
    scale(g_pop.status, 0.5f);
    pos(g_pop.status, cx, cy + 15);
    add(layer, g_pop.status, 5);
    g_pop.name = label(" ", "bigFont.fnt");
    scale(g_pop.name, 0.45f);
    pos(g_pop.name, cx, cy - 10);
    add(layer, g_pop.name, 5);
    g_pop.author = label(" ", "goldFont.fnt");
    scale(g_pop.author, 0.6f);
    pos(g_pop.author, cx, cy - 32);
    add(layer, g_pop.author, 5);

    int cur = lso_ng(mem::r32(g_layer + 0x1dc));
    if (cur >= kFirstNG) {
        g_pop.typed = std::to_string(cur);
        if (have_song(cur)) g_pop.found = cur;
    }
    popup_refresh();
    call(mem::r32(mem::r32(a) + 0x184), {a});  // show(): misma animación que las demás ventanas
}

uint32_t g_decorated = 0;  // ventana de ajustes que ya tiene el botón

void decorate() {
    // Botón "Newgrounds" dentro del mismo menú que "OK" (así se anima con la ventana),
    // y los dos centrados.
    g_decorated = g_layer;
    uint32_t menu = mem::r32(g_layer + 0x1b0);
    std::vector<uint32_t> ch;
    children(menu, ch);
    uint32_t ok = 0;
    for (uint32_t c : ch)
        if (!ok || py(c) < py(ok)) ok = c;
    if (!ok) return;
    static uint32_t on_open = cpu::add_thunk("Newgrounds: abrir", [](cpu::Cpu&) { g_action = kOpen; });
    float cx = px(ok), y = py(ok);
    uint32_t ng = button(menu, "Newgrounds", g_layer, on_open, cx, y);
    float wo = width(ok), wn = width(ng), gap = 12;
    float left = cx - (wo + gap + wn) / 2;
    pos(ok, left + wo / 2, y);
    pos(ng, left + wo + gap + wn / 2, y);
}

// ---------------------------------------------------------------- información en la página del nivel
uint32_t g_info_layer = 0, g_info_vt = 0, g_info_status = 0;
int g_info_id = 0;

void info_status() {
    if (!alive(g_info_layer, g_info_vt) || !g_info_status) return;
    if (have_song(g_info_id)) {
        label_text(g_info_status, "Downloaded");
        color(g_info_status, 80, 255, 80);
    } else if (busy(g_info_id)) {
        label_text(g_info_status, "Downloading...");
        color(g_info_status, 255, 255, 255);
    } else {
        label_text(g_info_status, " ");
    }
}

uint32_t g_info_author = 0;

void decorate_info(uint32_t layer) {
    g_info_layer = layer;
    g_info_vt = mem::r32(layer);
    g_info_status = g_info_author = 0;
    uint32_t level = mem::r32(layer + 0x154);
    g_info_id = level ? parse_ng(mem::str(mem::r32(level + 0x134))) : 0;
    if (!g_info_id && level && int(mem::r32(level + 0x144)) >= kFirstNG) g_info_id = int(mem::r32(level + 0x144));
    if (g_info_id < kFirstNG) {
        g_info_layer = 0;  // los datos del nivel pueden llegar después (descarga): volver a mirar
        return;
    }
    float w, h;
    win_size(w, h);
    // Una sola línea debajo de la barra de Practice Mode: "NGID: <id>" y el botón Download al lado.
    g_info_author = label("NGID: " + std::to_string(g_info_id), "goldFont.fnt");
    scale(g_info_author, 0.5f);
    pos(g_info_author, w / 2 - 45, 9);
    add(layer, g_info_author, 20);
    uint32_t menu = new_menu();
    add(layer, menu, 20);
    static uint32_t on_dl = cpu::add_thunk("Newgrounds: Download", [](cpu::Cpu&) { g_action = kInfoDownload; });
    button(menu, "Download", layer, on_dl, w / 2 + 55, 9, 0.4f);
    g_info_status = label(" ", "goldFont.fnt");
    scale(g_info_status, 0.4f);
    pos(g_info_status, w / 2 + 135, 9);
    add(layer, g_info_status, 20);
    info_status();
}

// ---------------------------------------------------------------- botón FPS Bypass en Opciones
int (*g_fps_get)() = nullptr;
void (*g_fps_cycle)() = nullptr;
uint32_t g_opt_layer = 0, g_opt_vt = 0, g_fps_sprite = 0;
bool g_fps_clicked = false;

std::string fps_text() {
    int f = g_fps_get ? g_fps_get() : 60;
    return "FPS Bypass: " + (f == 0 ? std::string("Unlimited") : std::to_string(f));
}

void decorate_options(uint32_t layer) {
    g_opt_layer = layer;
    g_opt_vt = mem::r32(layer);
    uint32_t in = mem::r32(layer + 0x20c), out = mem::r32(layer + 0x210);  // Google Play: entrar / salir
    if (!in) return;
    uint32_t menu = mem::r32(in + 0xc8);  // getParent
    float x = px(in), y = py(in);
    for (uint32_t b : {in, out})
        if (b) {
            visible(b, false);
            pos(b, -10000, -10000);  // que toggleGP no lo vuelva a mostrar en su sitio
        }
    if (!menu) return;
    static uint32_t on_fps = cpu::add_thunk("FPS Bypass", [](cpu::Cpu&) { g_fps_clicked = true; });
    g_fps_sprite = button(menu, fps_text(), layer, on_fps, x, y, 0.8f);  // el ButtonSprite es su hijo
}

uint32_t find_options_layer() {
    const uint32_t vt = 0x01000000 + 0x41bb50 + 8;
    uint32_t d = call(G(0x1a4420), {});
    uint32_t scene = mem::r32(d + 0x70);
    if (!scene) return 0;
    std::vector<uint32_t> ch, ch2;
    children(scene, ch);
    for (uint32_t c : ch) {
        if (mem::r32(c) == vt) return c;
        children(c, ch2);
        for (uint32_t c2 : ch2)
            if (mem::r32(c2) == vt) return c2;
    }
    return 0;
}

// ¿Hay una LevelInfoLayer en la escena actual?
uint32_t find_info_layer() {
    const uint32_t vt = 0x01000000 + 0x418848 + 8;  // vtable de LevelInfoLayer
    uint32_t d = call(G(0x1a4420), {});
    uint32_t scene = mem::r32(d + 0x70);
    if (!scene) return 0;
    std::vector<uint32_t> ch;
    children(scene, ch);
    for (uint32_t c : ch)
        if (mem::r32(c) == vt) return c;
    return 0;
}

}  // namespace

void install_level_header();
namespace { void install_colors(); void colors_tick(); }
namespace namecol { void install(); void tick(); std::string data_dir(); void on_response(const std::string& url, const std::string& body); }
namespace streak { void install(); void tick(); }
void install_zip_cache();
void set_dir(const std::string& d) { g_dir = d; }
void set_fps_hooks(int (*get)(), void (*cycle)()) {
    g_fps_get = get;
    g_fps_cycle = cycle;
}
std::string play_path() {  // canción de Newgrounds del nivel cargado ("" si no tiene)
    if (!g_on) return "";
    int id = g_play_ng;
    if (id < kFirstNG) return "";
    std::string p = mp3_path(id);
    if (have_song(id)) return p;
    start_download(id);
    std::unique_lock<std::mutex> l(g_mu);
    g_cv.wait_for(l, std::chrono::seconds(90), [&] { return !g_busy.count(id); });
    return p;
}

uint32_t hook_entry(uint32_t fn, const char* name, cpu::Handler h);

void install() {
    g_on = true;
    namecol::install();
    streak::install();
    install_level_header();
    install_colors();
    install_zip_cache();
    if (const char* t = std::getenv("HOST_SONGTEST")) {  // qué haría la app original con este número
        uint32_t id = uint32_t(std::atoi(t));
        uint32_t ti = uint32_t(cpu::call(kGetAudioTitle | 1, {id}));
        uint32_t fn = uint32_t(cpu::call(0x01143365, {id}));
        logf("[songtest] %u: titulo='%s' archivo='%s'", id, ti ? mem::str(ti) : "(null)", fn ? mem::str(fn) : "(null)");
    }
    // getAudioTitle: Ascella pone al principio "ldr.w pc, [pc]" + dirección de su versión.
    if (mem::r32(kGetAudioTitle) == 0xf000f85f) {
        g_title_orig = mem::r32(kGetAudioTitle + 4);
        uint32_t th = cpu::add_thunk("getAudioTitle (Newgrounds)", [](cpu::Cpu& c) {
            int id = int(cpu::reg(c, 0));
            if (id >= kFirstNG) cpu::reg(c, 0) = gstr(ascii(title_of(id)));
            else cpu::reg(c, 0) = uint32_t(cpu::call(g_title_orig, {uint32_t(id)}));
        });
        mem::w32(kGetAudioTitle + 4, th);
        cpu::invalidate(kGetAudioTitle, 8);
    } else {
        logf("[canciones] getAudioTitle no tiene el formato esperado; títulos de Newgrounds desactivados");
    }
    // updateAudioLabel: recuerda la ventana de ajustes abierta y, para Newgrounds, muestra "(ID) Nombre".
    if (mem::r32(kUpdateLabelCall) == 0xffacf7f2) {
        mem::commit(kVeneer, 16);
        static uint32_t orig_lit = mem::r32(kLabelFmtLit);
        static uint32_t ng_fmt = mem::strdup("(%i) %s");
        uint32_t th = cpu::add_thunk("updateAudioLabel (Newgrounds)", [](cpu::Cpu& c) {
            uint32_t layer = cpu::reg(c, 4);
            if (layer != g_layer) {
                g_layer = layer;
                g_layer_vt = mem::r32(layer);
            }
            int id = int(cpu::reg(c, 0));
            uint32_t lso = mem::r32(layer + 0x1dc);
            // Si cambió la canción oficial (flechas), se quita la de Newgrounds.
            auto lt = g_lso_track.find(lso);
            if (lt != g_lso_track.end() && lt->second != id) g_lso_ng.erase(lso);
            g_lso_track[lso] = id;
            // Canción de reserva (la oficial) para quien juegue sin el parche.
            static uint32_t ph_layer = 0, ph_label = 0;
            if (ph_layer != layer) {
                ph_layer = layer;
                float w, h;
                win_size(w, h);
                ph_label = label(" ", "goldFont.fnt");
                scale(ph_label, 0.45f);
                uint32_t m = 0, inner = 0;
                if (settings_nodes(layer, m, inner)) {
                    pos(ph_label, w / 2 - px(inner), 100 - py(inner));  // dentro de la ventana: se anima con ella
                    add(inner, ph_label, 30);
                } else {
                    pos(ph_label, w / 2, 100);
                    add(layer, ph_label, 30);
                }
            }
            if (lso_ng(lso)) {
                uint32_t t = uint32_t(cpu::call(g_title_orig, {uint32_t(id)}));
                label_text(ph_label, std::string("Placeholder: ") + (t ? mem::str(t) : "?"));
                visible(ph_label, true);
            } else {
                visible(ph_label, false);
            }
            if (int ng = lso_ng(lso)) id = ng;
            cpu::reg(c, 0) = uint32_t(cpu::call(kGetAudioTitle | 1, {uint32_t(id)}));
            if (id >= kFirstNG) {
                cpu::reg(c, 6) = uint32_t(id - 1);  // el juego imprime id+1
                mem::w32(kLabelFmtLit, ng_fmt - kLabelFmtPC);
            } else {
                mem::w32(kLabelFmtLit, orig_lit);
            }
        });
        mem::w32(kVeneer, 0xe51ff004);  // ldr pc, [pc, #-4]
        mem::w32(kVeneer + 4, th);
        write_blx(kUpdateLabelCall, kVeneer);
    } else {
        logf("[canciones] updateAudioLabel no tiene el formato esperado; botón de Newgrounds desactivado");
    }
}

// Cambia un "bl" del juego por una llamada a `h` (con un trampolín ARM cerca de libgame).
void hook_bl(uint32_t site, const char* name, cpu::Handler h) {
    static uint32_t next = 0x0145ffd0;
    if ((mem::r16(site) & 0xf800) != 0xf000 || (mem::r16(site + 2) & 0xd000) != 0xd000) {
        logf("[canciones] %s: instrucción inesperada en %08x", name, site);
        return;
    }
    uint32_t v = next;
    next -= 16;
    mem::commit(v, 16);
    mem::w32(v, 0xe51ff004);  // ldr pc, [pc, #-4]
    mem::w32(v + 4, cpu::add_thunk(name, std::move(h)));
    write_blx(site, v);
}

// Engancha la ENTRADA de una función Thumb del juego (dirección alineada a 4 cuyos primeros 8 bytes
// son instrucciones de 16 bits sin referencias a pc). Devuelve la dirección para llamar a la original.
uint32_t hook_entry(uint32_t fn, const char* name, cpu::Handler h) {
    static uint32_t next = 0x0145fe00;  // trampolines, cerca de los veneers
    if (fn & 3) return 0;
    uint32_t tr = next;
    next += 16;
    mem::commit(tr, 16);
    mem::w32(tr, mem::r32(fn));
    mem::w32(tr + 4, mem::r32(fn + 4));
    mem::w32(tr + 8, 0xf000f8df);  // ldr.w pc, [pc, #0]
    mem::w32(tr + 12, (fn + 8) | 1);
    cpu::invalidate(tr, 16);
    mem::w32(fn, 0xf000f8df);
    mem::w32(fn + 4, cpu::add_thunk(name, std::move(h)));
    cpu::invalidate(fn, 8);
    return tr | 1;
}

// Rendimiento: unzLocateFile recorre el índice del APK leyendo byte a byte. Ascella crea etiquetas
// cada fotograma y cocos busca su fuente así; se recuerda dónde estaba cada archivo.
void install_zip_cache() {
    constexpr uint32_t kLocate = 0x011ef250, kGetPos = 0x011eda8d, kGoToPos = 0x011ee0dd;
    if (mem::r32(kLocate) != 0x465fb5f0 || mem::r32(kLocate + 4) != 0x464d4656) {
        logf("[perf] unzLocateFile no tiene el formato esperado; sin caché");
        return;
    }
    static uint32_t orig = 0;
    orig = hook_entry(kLocate, "unzLocateFile (caché)", [](cpu::Cpu& c) {
        struct Pos { uint32_t a, b; bool found; };
        static std::mutex mu;
        static std::map<std::pair<uint32_t, std::string>, Pos> cache;  // clave: (tamaño del índice, nombre)
        static uint32_t buf = mem::alloc(8);
        uint32_t file = cpu::reg(c, 0), name = cpu::reg(c, 1), cs = cpu::reg(c, 2);
        // cocos abre el APK de nuevo en cada búsqueda: la clave no puede ser el puntero. Se usa el
        // número de archivos del zip (unz64_s.gi.number_entry, primer campo tras los callbacks).
        std::pair<uint32_t, std::string> key{0, std::string(mem::str(name) ? mem::str(name) : "")};
        if (std::getenv("GD_PROFILE")) {
            static std::map<std::string, int> counts;
            static int total = 0;
            std::lock_guard<std::mutex> l(mu);
            counts[key.second]++;
            if (++total % 2000 == 0)
                for (auto& [n, k] : counts)
                    if (k > 100) logf("[perf] zip: %s x%d", n.c_str(), k);
        }
        {
            std::lock_guard<std::mutex> l(mu);
            auto it = cache.find(key);
            if (it != cache.end()) {
                if (!it->second.found) { cpu::reg(c, 0) = uint32_t(-100); return; }  // UNZ_END_OF_LIST_OF_FILE
                mem::w32(buf, it->second.a);
                mem::w32(buf + 4, it->second.b);
                cpu::reg(c, 0) = uint32_t(cpu::call(kGoToPos, {file, buf}));
                return;
            }
        }
        int r = int(cpu::call(orig, {file, name, cs}));
        Pos p{0, 0, r == 0};
        if (r == 0 && cpu::call(kGetPos, {file, buf}) == 0) {
            p.a = mem::r32(buf);
            p.b = mem::r32(buf + 4);
        } else if (r == 0) {
            cpu::reg(c, 0) = 0;
            return;  // no se pudo guardar la posición: sin caché para este
        }
        std::lock_guard<std::mutex> l(mu);
        cache[key] = p;
        cpu::reg(c, 0) = uint32_t(r);
    });
}

// kNG en la cabecera del nivel: se escribe al guardar y se lee al cargar.

// ---------------------------------------------------------------- colores P1/P2 del nivel
// Se activan en secreto: Opciones > Support, mantener "Email Support" 3 segundos (el texto pasa
// a minúsculas). Se guardan en la cabecera del nivel (kP1 / kP2), que el 1.3 GDPS original ignora.
namespace {
bool g_cc_on = false;                                     // función desbloqueada
std::map<uint32_t, int> g_lso_p1, g_lso_p2;               // LevelSettingsObject -> 0xBBGGRR
int g_parsed_p1 = -1, g_parsed_p2 = -1, g_play_p1 = -1, g_play_p2 = -1;
bool g_touch = false;                                     // dedo/ratón pulsado
float g_touch_x = 0, g_touch_y = 0;                       // fracción de la pantalla (0..1, y hacia abajo)
uint32_t g_touch_t = 0;
uint32_t g_cc_layer = 0, g_cc_sq1 = 0, g_cc_sq2 = 0;      // ajustes del nivel con P1/P2 añadidos
uint32_t g_sup_layer = 0, g_sup_btn = 0;
bool g_sup_fired = false;
// El objeto "Level End" (ID 34), que en 1.3 no hace nada al jugar, se usa como trigger de color o
// como objeto con textura. Claves en su texto: kCT (canal: 1 P1, 2 P2, 3 BG, 4 GR), kCC (copia el
// color de otro canal), kNB (sin mezcla aditiva), kTX / kTC (textura y su color), y las del juego
// 7,8,9 (color) y 10 (duración).
constexpr int kLevelEndID = 34;
constexpr float kUnitsPerSec = 311.58f;            // velocidad normal del cubo (unidades/segundo)
enum { kChP1 = 1, kChP2 = 2, kChBG = 3, kChGR = 4 };
struct LeCfg {
    int target = 0;      // canal que cambia (0: no es trigger)
    int copy = 0;        // canal del que copia el color (0: no copia)
    bool nb = false;     // sin mezcla aditiva
    std::string tex;     // textura asignada (vacío: ninguna)
    int tex_col = 0;     // canal que tiñe la textura (0: blanco)
};
std::map<uint32_t, LeCfg> g_trig;                  // Level End configurados
LeCfg g_parsed_cfg;
bool g_parsed_le = false;
int g_parsed_tint = 0xffffff;
float g_parsed_dur = 0.5f;
int g_parsed_bg = -1, g_parsed_gr = -1, g_play_bg = -1, g_play_gr = -1;
std::map<uint32_t, int> g_lso_bg, g_lso_gr;        // colores iniciales del nivel (kS1-3 / kS4-6)
bool g_editor_dirty = false;                       // repasar los objetos del editor en el próximo frame
// Color y pulsación por objeto (objetos de color de jugador y los que pulsan con la música).
// Claves: kCH (0 Default, 1 White, 2 BG, 3 GR, 4 P1, 5 P2) y kNP,1 (sin pulsación).
struct ObjCfg { int ch = 0; bool nopulse = false; };
std::map<uint32_t, ObjCfg> g_objcfg;
ObjCfg g_parsed_obj;
bool g_parsed_has_obj = false;
const char* const kObjChName[] = {"Default", "White", "BG", "GR", "P1", "P2"};
int objch_channel(int ch) { return ch == 2 ? kChBG : ch == 3 ? kChGR : ch == 4 ? kChP1 : ch == 5 ? kChP2 : 0; }
bool uses_player_color(uint32_t o) { return *mem::ptr<uint8_t>(o + 0x2ed) || *mem::ptr<uint8_t>(o + 0x2ee); }
bool pulses(uint32_t o) { return *mem::ptr<uint8_t>(o + 0x2f0) != 0; }  // useAudioScale
bool obj_editable(uint32_t o) { return o && mem::r32(o + 0x31c) != 34 && (uses_player_color(o) || pulses(o)); }
bool obj_overridden(uint32_t o) {
    auto it = g_objcfg.find(o);
    return it != g_objcfg.end() && it->second.ch != 0;
}
uint32_t g_draw_grid = 0;                          // DrawGridLayer del editor (dibuja la línea de los triggers)
uint32_t parent(uint32_t n);
// El DrawGridLayer guardado sirve solo si cuelga del editor actual (al salir del editor se destruye).
bool grid_of(uint32_t ed) {
    uint32_t n = g_draw_grid;
    for (int i = 0; i < 4 && n; i++) {
        n = parent(n);
        if (n == ed) return true;
    }
    return false;
}
bool is_texture_le(uint32_t o) {
    if (mem::r32(o + 0x31c) != kLevelEndID) return false;
    auto it = g_trig.find(o);
    return it != g_trig.end() && !it->second.tex.empty();
}
std::vector<uint32_t> g_p1_objs, g_p2_objs;        // objetos de color de jugador del nivel en juego
uint32_t g_play_layer = 0;

std::string cc_file() { return g_dir + "/../custom_colors.txt"; }
void cc_load() {
    std::ifstream f(cc_file());
    int v = 0;
    g_cc_on = f && (f >> v) && v == 1;
}
void cc_save() {
    std::ofstream f(cc_file());
    f << (g_cc_on ? 1 : 0) << "\n";
}

int parse_color(const char* text, const char* key) {  // "kP1,r_g_b" en la cabecera
    if (!text) return -1;
    size_t kl = std::strlen(key);
    for (const char* p = text; *p && *p != ';'; p++)
        if (std::strncmp(p, key, kl) == 0 && (p == text || p[-1] == ',')) {
            int r, g, b;
            if (std::sscanf(p + kl, "%d_%d_%d", &r, &g, &b) == 3)
                return (r & 255) | ((g & 255) << 8) | ((b & 255) << 16);
        }
    return -1;
}
// Colores iniciales de fondo (kS1,kS2,kS3) y suelo (kS4,kS5,kS6) de la cabecera.
void parse_ks(const char* text, int& bg, int& gr) {
    bg = gr = -1;
    if (!text) return;
    int v[7] = {-1, -1, -1, -1, -1, -1, -1};
    std::string head(text, std::strcspn(text, ";"));
    size_t i = 0;
    while (i < head.size()) {
        size_t a = head.find(',', i);
        if (a == std::string::npos) break;
        size_t b = head.find(',', a + 1);
        std::string k = head.substr(i, a - i);
        int val = std::atoi(head.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1).c_str());
        if (k.size() == 3 && k[0] == 'k' && k[1] == 'S' && k[2] >= '1' && k[2] <= '6') v[k[2] - '0'] = val & 255;
        if (b == std::string::npos) break;
        i = b + 1;
    }
    if (v[1] >= 0 && v[2] >= 0 && v[3] >= 0) bg = v[1] | (v[2] << 8) | (v[3] << 16);
    if (v[4] >= 0 && v[5] >= 0 && v[6] >= 0) gr = v[4] | (v[5] << 8) | (v[6] << 16);
}
std::string color_str(int c) {
    return std::to_string(c & 255) + "_" + std::to_string((c >> 8) & 255) + "_" + std::to_string((c >> 16) & 255);
}
int map_get(const std::map<uint32_t, int>& m, uint32_t k) {
    auto it = m.find(k);
    return it == m.end() ? -1 : it->second;
}
uint32_t color_ptr(int c) {  // ccColor3B temporal en memoria del juego
    static uint32_t buf = mem::alloc(16);
    static int i = 0;
    uint32_t p = buf + 4 * (i++ & 3);
    mem::w32(p, uint32_t(c) & 0xffffff);
    return p;
}
void set_color(uint32_t node, int c) { call(mem::r32(mem::r32(node) + 0x134), {node, color_ptr(c)}); }
int player_color(int which) {  // colores del jugador (garaje)
    uint32_t gm = call(G(0x14160c), {});
    uint32_t idx = call(which == 1 ? G(0x140d48) : G(0x140d58), {gm});
    return int(call(G(0x140e9c), {gm, idx}) & 0xffffff);  // colorForIdx
}
uint32_t parent(uint32_t n) { return n ? call(G(0x19c78c), {n}) : 0; }

// Cambia 4 bytes de código por un salto a `h`; al volver, el juego sigue en `resume`.
void patch_jump(uint32_t site, uint32_t resume, const char* name, cpu::Handler h) {
    static uint32_t next = 0x0145ff00;
    uint32_t v = next;
    next -= 16;
    mem::commit(v, 16);
    mem::w32(v, 0xe51ff004);  // ldr pc, [pc, #-4]
    mem::w32(v + 4, cpu::add_thunk(name, [h = std::move(h), resume](cpu::Cpu& c) {
        h(c);
        cpu::reg(c, 14) = resume | 1;
    }));
    write_blx(site, v);
    cpu::invalidate(site, 4);
}

// Abre el selector de color del juego para P1 (etiqueta 2) o P2 (etiqueta 3).
void open_picker(int which) {
    if (!alive(g_cc_layer, g_layer_vt)) return;
    uint32_t lso = mem::r32(g_cc_layer + 0x1dc);
    int cur = map_get(which == 1 ? g_lso_p1 : g_lso_p2, lso);
    if (cur < 0) cur = player_color(which);
    uint32_t pk = call(G(0x16eb14), {});                                       // ColorPickerPopup::create
    call(mem::r32(mem::r32(pk) + 0x1ac), {pk, g_cc_layer + 0x1cc});            // setDelegate
    call(G(0x16e80c), {pk, uint32_t(cur)});                                    // selectColor
    call(mem::r32(mem::r32(pk) + 0x98), {pk, uint32_t(which + 1)});            // setTag
    call(mem::r32(mem::r32(pk) + 0x184), {pk});                                // show
}

void refresh_squares() {
    if (!alive(g_cc_layer, g_layer_vt)) return;
    uint32_t lso = mem::r32(g_cc_layer + 0x1dc);
    int c1 = map_get(g_lso_p1, lso), c2 = map_get(g_lso_p2, lso);
    if (g_cc_sq1) set_color(g_cc_sq1, c1 >= 0 ? c1 : player_color(1));
    if (g_cc_sq2) set_color(g_cc_sq2, c2 >= 0 ? c2 : player_color(2));
}

// Ajustes del nivel: cuadros P1 y P2 a los lados de BG y Ground, con un botón "Reset" debajo.
void decorate_colors(uint32_t layer) {
    g_cc_layer = layer;
    g_cc_sq1 = g_cc_sq2 = 0;
    const uint32_t item_vt = 0x01000000 + 0x4107a8 + 8;
    uint32_t bg = mem::r32(layer + 0x1d4), gr = mem::r32(layer + 0x1d8);
    if (!bg || !gr) return;
    uint32_t bgi = bg, gri = gr;
    for (int i = 0; i < 4 && bgi && mem::r32(bgi) != item_vt; i++) bgi = parent(bgi);
    for (int i = 0; i < 4 && gri && mem::r32(gri) != item_vt; i++) gri = parent(gri);
    if (!bgi || !gri || mem::r32(bgi) != item_vt) {
        logf("[colores] no se encontró el botón de BG");
        return;
    }
    uint32_t menu = parent(bgi);
    float dx = px(gri) - px(bgi);
    uint32_t frame = call(G(0x1e62fc), {bg});  // CCSprite::displayFrame
    float sc = u2f(call(G(0x19d27c), {bg}));
    static uint32_t on1 = cpu::add_thunk("Colores: P1", [](cpu::Cpu&) { open_picker(1); });
    static uint32_t on2 = cpu::add_thunk("Colores: P2", [](cpu::Cpu&) { open_picker(2); });
    static uint32_t rs1 = cpu::add_thunk("Colores: P1 reset", [](cpu::Cpu&) {
        if (alive(g_cc_layer, g_layer_vt)) g_lso_p1.erase(mem::r32(g_cc_layer + 0x1dc));
        refresh_squares();
    });
    static uint32_t rs2 = cpu::add_thunk("Colores: P2 reset", [](cpu::Cpu&) {
        if (alive(g_cc_layer, g_layer_vt)) g_lso_p2.erase(mem::r32(g_cc_layer + 0x1dc));
        refresh_squares();
    });
    float y = py(bgi);
    for (int which = 1; which <= 2; which++) {
        float x = which == 1 ? px(bgi) - dx * 0.43f : px(gri) + dx * 0.43f;
        uint32_t spr = call(G(0x1e5d04), {frame});  // CCSprite::create(frame)
        scale(spr, sc);
        uint32_t item = call(G(0x107b84), {spr, 0, layer, which == 1 ? on1 : on2, 0});
        add(menu, item);
        pos(item, x, y);
        (which == 1 ? g_cc_sq1 : g_cc_sq2) = spr;
        uint32_t t = label(which == 1 ? "P1:" : "P2:", "bigFont.fnt");  // un CCMenu solo admite botones
        scale(t, 0.6f);
        uint32_t inner = parent(menu);
        add(inner, t);
        pos(t, x + px(menu) - px(inner), y + 50 + py(menu) - py(inner));
        button(menu, "Reset", layer, which == 1 ? rs1 : rs2, x, y - 40, 0.35f);
    }
    refresh_squares();
}

// Opciones > Support: el botón "Email Support" (único ButtonSprite de la ventana).
uint32_t find_support_button(uint32_t& layer_out) {
    const uint32_t vt = 0x01000000 + 0x41c848 + 8, bvt = 0x01000000 + 0x40f888 + 8;
    uint32_t d = call(G(0x1a4420), {});
    uint32_t scene = mem::r32(d + 0x70);
    if (!scene) return 0;
    // la ventana de Support cuelga de la escena o de la de Opciones
    uint32_t sup = 0;
    std::vector<uint32_t> ch, ch2;
    children(scene, ch);
    for (uint32_t c : ch) {
        if (mem::r32(c) == vt) { sup = c; break; }
        children(c, ch2);
        for (uint32_t c2 : ch2)
            if (mem::r32(c2) == vt) sup = c2;
        if (sup) break;
    }
    layer_out = sup;
    if (!sup) return 0;
    // búsqueda en anchura (pocos niveles)
    std::vector<uint32_t> cur{sup}, nxt, tmp;
    for (int depth = 0; depth < 5 && !cur.empty(); depth++) {
        nxt.clear();
        for (uint32_t n : cur) {
            children(n, tmp);
            for (uint32_t c : tmp) {
                if (mem::r32(c) == bvt) return c;
                nxt.push_back(c);
            }
        }
        cur.swap(nxt);
    }
    return 0;
}

bool touch_inside(uint32_t node) {
    // En esta versión de cocos2d CCPoint es un CCObject: x, y en +0x18 / +0x1c.
    uint32_t pt = mem::alloc(0x40), out = mem::alloc(0x40);
    float w, h;
    win_size(w, h);
    float cw = width(node), chh = u2f(mem::r32(node + 0xa4 + 0x1c));
    float xs[2], ys[2];
    for (int i = 0; i < 2; i++) {
        call(G(0x19f050), {pt, f2u(i ? cw : 0.f), f2u(i ? chh : 0.f)});  // CCPoint(x, y)
        call(G(0x19c9c8), {out, node, pt});                            // convertToWorldSpace
        xs[i] = u2f(mem::r32(out + 0x18));
        ys[i] = u2f(mem::r32(out + 0x1c));
        call(G(0xfcda0), {out});  // ~CCPoint
        call(G(0xfcda0), {pt});
    }
    mem::free(pt);
    mem::free(out);
    float tx = g_touch_x * w, ty = (1 - g_touch_y) * h;
    return tx >= std::min(xs[0], xs[1]) && tx <= std::max(xs[0], xs[1]) && ty >= std::min(ys[0], ys[1]) && ty <= std::max(ys[0], ys[1]);
}

void support_text() {
    if (g_sup_btn) call(G(0x102448), {g_sup_btn, gstr(g_cc_on ? "email support" : "Email Support")});
}


// Editor (el de Ascella es una subclase de LevelEditorLayer con su propia vtable).
bool is_editor(uint32_t n) {
    uint32_t vt = mem::r32(n);
    return vt == 0x01000000 + 0x417370 + 8 || vt == 0x01b00138;
}
uint32_t find_editor() {
    uint32_t d = call(G(0x1a4420), {}), scene = mem::r32(d + 0x70);
    if (!scene) return 0;
    std::vector<uint32_t> ch;
    children(scene, ch);
    for (uint32_t e : ch)
        if (is_editor(e)) return e;
    return 0;
}

// Colores por canal (P1, P2, BG, GR). Cada trigger es un evento en X; el color de un canal en una
// posición se obtiene repasando sus eventos en orden. Un trigger con kCC toma como destino el color
// del canal fuente (que a su vez puede estar copiando otro). Las copias en ciclo no cambian el color.
int mix(int a, int b, float t);
struct ColEv { float x; int ch, col; float dur; int copy; bool ours; bool nb; };

int eval_channel(const std::vector<ColEv>& ev, int ch, float X, const int* init, unsigned visiting) {
    int col = init[ch];
    for (size_t i = 0; i < ev.size() && ev[i].x <= X; i++) {
        const ColEv& e = ev[i];
        if (e.ch != ch) continue;
        int target = e.col;
        if (e.copy >= kChP1 && e.copy <= kChGR) {
            if (visiting & (1u << e.copy)) {
                target = col;
            } else {
                float until = X;
                for (size_t j = i + 1; j < ev.size() && ev[j].x <= X; j++)
                    if (ev[j].ch == ch) { until = ev[j].x; break; }
                target = eval_channel(ev, e.copy, until, init, visiting | (1u << ch));
            }
        }
        float span = e.dur * kUnitsPerSec;
        col = mix(col, target, span <= 0 ? 1.f : std::min(1.f, (X - e.x) / span));
    }
    return col;
}
const ColEv* last_event(const std::vector<ColEv>& ev, int ch, float X) {
    const ColEv* r = nullptr;
    for (auto& e : ev) {
        if (e.x > X) break;
        if (e.ch == ch) r = &e;
    }
    return r;
}
// Triggers del juego (29 BG, 30 Ground) y Level End configurados.
void collect_event(uint32_t o, std::vector<ColEv>& ev) {
    uint32_t id = mem::r32(o + 0x31c);
    int col = int(mem::r32(o + 0x314) & 0xffffff);
    float dur = u2f(mem::r32(o + 0x318));
    if (id == 29 || id == 30) {
        ev.push_back({px(o), id == 29 ? kChBG : kChGR, col, dur, 0, false, false});
    } else if (id == kLevelEndID) {
        auto it = g_trig.find(o);
        if (it != g_trig.end() && (it->second.target == kChP1 || it->second.target == kChP2))
            ev.push_back({px(o), it->second.target, col, dur, it->second.copy, true, it->second.nb});
    }
}
void sort_events(std::vector<ColEv>& ev) {
    std::stable_sort(ev.begin(), ev.end(), [](const ColEv& a, const ColEv& b) { return a.x < b.x; });
}

// Texturas en Level End: el nombre es un cuadro de GJ_GameSheet; los "_glow" van en la capa aditiva.
uint32_t sprite_frame(const std::string& name) {
    static std::map<std::string, uint32_t> names;
    auto it = names.find(name);
    uint32_t s = it != names.end() ? it->second : (names[name] = mem::strdup(name.c_str()));
    return call(G(0x1ea2f0), {call(G(0x1e8cb0), {}), s});  // CCSpriteFrameCache::spriteFrameByName
}
bool is_glow_tex(const std::string& n) { return n.find("_glow") != std::string::npos; }
void copy_transform(uint32_t from, uint32_t to) {
    call(G(0x1e4e0c), {to, call(G(0x19c6e8), {from})});                 // rotación
    call(G(0x1e4d34), {to, call(G(0x19c718), {from})});                 // escala X
    call(G(0x1e4cec), {to, call(G(0x19c730), {from})});                 // escala Y
    call(G(0x1e442c), {to, call(G(0x1e4458), {from}) & 0xff});          // volteo X
    call(G(0x1e4460), {to, call(G(0x1e448c), {from}) & 0xff});          // volteo Y
}
uint32_t new_batch_like(uint32_t base, int z_offset, bool additive) {
    uint32_t b = call(G(0x1e7cf8), {call(G(0x1e6b98), {base}), 29});    // misma textura que la capa base
    if (additive) call(mem::r32(mem::r32(b) + 0x130), {b, 0x302, 1});   // setBlendFunc(GL_SRC_ALPHA, GL_ONE)
    add(parent(base), b, int(call(G(0x19c6b4), {base})) + z_offset);
    return b;
}
// Colores de los 4 canales en una posición.
void channel_colors(const std::vector<ColEv>& ev, float x, const int* init, int* out) {
    out[0] = 0xffffff;
    for (int ch = kChP1; ch <= kChGR; ch++) out[ch] = eval_channel(ev, ch, x, init, 1u << ch);
}

// Objetos del editor (hijos de las capas de la capa de juego).
void editor_objects(uint32_t gl, std::vector<uint32_t>& out) {
    const uint32_t obj_vt = 0x01000000 + 0x4169c0 + 8;
    out.clear();
    std::vector<uint32_t> lv1, lv2;
    children(gl, lv1);
    for (uint32_t b : lv1) {
        children(b, lv2);
        lv2.push_back(b);
        for (uint32_t o : lv2)
            if (mem::r32(o) == obj_vt) out.push_back(o);
    }
}

void selected_objects(uint32_t ui, std::vector<uint32_t>& out);
constexpr uint32_t kAscellaEditObject = 0x0167756b, kAscellaCopyPaste = 0x0167099d;
constexpr uint32_t kEditorUIvt = 0x014177a0;
uint32_t editor_ui(uint32_t ed) {
    std::vector<uint32_t> ch;
    children(ed, ch);
    for (uint32_t c : ch)
        if (mem::r32(c) == kEditorUIvt) return c;
    return 0;
}
int node_color(uint32_t n) { return int(mem::r32(call(G(0x1e44c8), {n})) & 0xffffff); }

// Modo preview de Ascella: pinta el fondo del editor con el color BG del nivel (sin preview, blanco)
// y aplica los triggers BG/Ground según la posición de la cámara. Nosotros pintamos los objetos de
// color de jugador y aplicamos nuestros triggers con la misma regla que al jugar.
void editor_preview_tick() {
    static uint32_t last_ed = 0, last_count = 0;
    static int last_state = -1, last_c[5] = {0, -2, -2, -2, -2}, settle = 0;
    static std::vector<uint32_t> objs[3];  // [1] = P1, [2] = P2
    static std::vector<uint32_t> tex_objs, ov_objs;
    static std::map<uint32_t, std::string> tex_applied;   // textura y capa ya puestas ("nombre|aditiva")
    static std::map<uint32_t, uint32_t> orig_layer;
    static uint32_t add_batch = 0, add_for = 0;
    static int tex_last = -3;
    static std::vector<ColEv> ev;
    static bool ours = false, ours_bg = false;
    uint32_t ed = find_editor();
    if (!ed) { last_ed = 0; return; }
    std::vector<uint32_t> ch;
    children(ed, ch);
    if (ch.empty()) return;
    uint32_t bg = ch[0];
    uint32_t lso = call(G(0x1501dc), {ed});       // getLevelSettings
    uint32_t count = call(G(0x1501bc), {ed});     // getObjectCount
    uint32_t gl = call(G(0x1501cc), {ed});        // getGameLayer
    int custom[3] = {0, map_get(g_lso_p1, lso), map_get(g_lso_p2, lso)};
    // Repasar la lista de objetos si cambió el editor, el número de objetos o el modo.
    static int rescan_wait = 0;
    int state = (mem::r32(call(G(0x1e44c8), {bg})) & 0xffffff) != 0xffffff ? 1 : 0;  // color del fondo
    if (ed != last_ed || count != last_count || state != last_state || --rescan_wait <= 0 || g_editor_dirty) {
        rescan_wait = 60;  // y cada segundo, por si se editó un trigger
        g_editor_dirty = false;
        objs[1].clear(); objs[2].clear(); ev.clear(); tex_objs.clear(); ov_objs.clear();
        if (ed != last_ed) { tex_applied.clear(); orig_layer.clear(); }
        std::vector<uint32_t> all;
        editor_objects(gl, all);
        for (uint32_t o : all) {
            if (obj_overridden(o)) ov_objs.push_back(o);
            else if (*mem::ptr<uint8_t>(o + 0x2ed)) objs[1].push_back(o);
            else if (*mem::ptr<uint8_t>(o + 0x2ee)) objs[2].push_back(o);
            else collect_event(o, ev);
            if (mem::r32(o + 0x31c) != kLevelEndID) continue;
            auto it = g_trig.find(o);
            if (it == g_trig.end() || it->second.tex.empty()) continue;
            tex_objs.push_back(o);
            bool additive = !it->second.nb;
            std::string want = it->second.tex + (additive ? "|a" : "|n");
            if (tex_applied[o] == want) continue;
            uint32_t fr = sprite_frame(it->second.tex);
            if (!fr) continue;
            call(G(0x1e4930), {o, fr});  // setDisplayFrame: el Level End se ve con la textura
            tex_applied[o] = want;
            if (grid_of(ed)) call(G(0x1510c0), {g_draw_grid, o});  // sin la línea de trigger
            if (!orig_layer.count(o)) orig_layer[o] = mem::r32(o + 0x328) ? mem::r32(o + 0x328) : parent(o);
            uint32_t layer = orig_layer[o];
            if (additive && layer) {
                if (add_for != ed || !add_batch) { add_batch = new_batch_like(layer, -1, true); add_for = ed; }
                layer = add_batch;
            }
            if (layer) {
                mem::w32(o + 0x328, layer);
                if (parent(o) && parent(o) != layer) {
                    int z = int(call(G(0x19c6b4), {o}));
                    call(G(0x1a1ee8), {o});
                    call(G(0x19c7dc), {o, 0});
                    add(layer, o, z);
                    call(G(0x1a1f54), {o});
                }
            }
            tex_last = -3;
        }
        sort_events(ev);
        ours = custom[1] >= 0 || custom[2] >= 0 || !tex_objs.empty() || !ov_objs.empty();
        ours_bg = false;
        for (auto& e : ev) {
            if (e.ours) ours = true;
            if (e.ours && e.ch == kChBG) ours_bg = true;
        }
        if (ed != last_ed || count != last_count || state != last_state) settle = 10;  // Ascella repinta al cambiar
        last_ed = ed; last_count = count; last_state = state;
    }
    // Sin preview Ascella deja el fondo blanco; con preview lo pinta con el color del nivel.
    bool preview = state == 1;
    float w, h;
    win_size(w, h);
    float sc = u2f(call(G(0x19d27c), {gl}));
    float camx = (w / 2 - px(gl)) / (sc > 0 ? sc : 1);  // centro de la pantalla en coordenadas del nivel
    bool force = settle > 0;
    if (settle > 0) settle--;
    // Objetos seleccionados: conservan el color de selección; al soltarlos se vuelven a pintar.
    static std::set<uint32_t> prev_sel;
    std::set<uint32_t> sel;
    if (uint32_t ui = editor_ui(ed)) {
        std::vector<uint32_t> v;
        selected_objects(ui, v);
        sel.insert(v.begin(), v.end());
    }
    bool sel_changed = sel != prev_sel;
    prev_sel = sel;
    // Botón "Edit Object" de Ascella: opaco cuando lo seleccionado se edita con nuestras ventanas.
    {
        static uint32_t btn = 0;
        static bool forced = false;
        uint32_t ui = editor_ui(ed);
        if (ui && (!btn || mem::r32(btn + 0x130) != kAscellaEditObject || parent(parent(btn)) != ui)) {
            btn = 0;
            std::vector<uint32_t> a, b;
            children(ui, a);
            for (uint32_t x : a) {
                children(x, b);
                for (uint32_t y : b)
                    if (mem::r32(y + 0x130) == kAscellaEditObject) btn = y;
            }
        }
        bool ours_sel = false;
        if (g_cc_on)
            for (uint32_t o : sel)
                if (mem::r32(o + 0x31c) == kLevelEndID || obj_editable(o)) { ours_sel = true; break; }
        if (btn && ours_sel) {
            call(G(0x1d0524), {btn, 255});  // CCMenuItemSprite::setOpacity
            forced = true;
        } else if (btn && forced) {
            call(G(0x1d0524), {btn, 125});
            forced = false;
        }
    }
    int init[5] = {0, custom[1] >= 0 ? custom[1] : player_color(1), custom[2] >= 0 ? custom[2] : player_color(2),
                   map_get(g_lso_bg, lso), map_get(g_lso_gr, lso)};
    if (init[kChBG] < 0) init[kChBG] = 0x00ff6628;  // valores del juego por defecto (0xBBGGRR)
    if (init[kChGR] < 0) init[kChGR] = 0x00ff6600;
    for (int wch = 1; wch <= 2; wch++) {
        int col = 0xffffff;
        if (preview && ours) col = eval_channel(ev, wch, camx, init, 1u << wch);
        else if (!(last_c[wch] >= 0 && last_c[wch] != 0xffffff)) continue;  // nada nuestro que quitar
        // El editor también los repinta (al cambiar de pestaña, al seleccionar): se compara uno de muestra.
        bool drift = !objs[wch].empty() && !sel.count(objs[wch][0]) && node_color(objs[wch][0]) != col;
        if (col == last_c[wch] && !force && !sel_changed && !drift) continue;
        last_c[wch] = col;
        for (uint32_t o : objs[wch])
            if (!sel.count(o)) set_color(o, col);
    }
    // Fondo: solo cuando el último trigger BG antes de la cámara es nuestro (si no, lo maneja Ascella).
    const ColEv* le = preview && ours_bg ? last_event(ev, kChBG, camx) : nullptr;
    if (le && le->ours) {
        int col = eval_channel(ev, kChBG, camx, init, 1u << kChBG);
        set_color(bg, col);  // Ascella lo repinta cada frame
        last_c[kChBG] = col;
    } else {
        last_c[kChBG] = -2;
    }
    // Objetos con canal propio: con preview, el color del canal; sin preview, blancos.
    if (!ov_objs.empty()) {
        int cc[5];
        channel_colors(ev, camx, init, cc);
        cc[0] = 0xffffff;
        static int ov_key = -3;
        int key = preview ? (cc[1] ^ (cc[2] << 1) ^ (cc[3] << 2) ^ (cc[4] << 3)) : -1;
        auto want = [&](uint32_t o) {
            auto it = g_objcfg.find(o);
            if (it == g_objcfg.end() || !preview) return 0xffffff;
            return it->second.ch == 1 ? 0xffffff : cc[objch_channel(it->second.ch)];
        };
        bool drift = !sel.count(ov_objs[0]) && node_color(ov_objs[0]) != want(ov_objs[0]);
        if (key != ov_key || force || sel_changed || drift) {
            ov_key = key;
            for (uint32_t o : ov_objs)
                if (!sel.count(o)) set_color(o, want(o));
        }
    }
    // Texturas: con preview, el color de su canal; sin preview, blancas.
    if (!tex_objs.empty()) {
        int cc[5];
        channel_colors(ev, camx, init, cc);
        int key = preview ? (cc[1] ^ (cc[2] << 1) ^ (cc[3] << 2) ^ (cc[4] << 3)) : -1;
        auto want = [&](uint32_t o) {
            auto it = g_trig.find(o);
            return it == g_trig.end() || !preview ? 0xffffff : cc[it->second.tex_col];
        };
        bool drift = !sel.count(tex_objs[0]) && node_color(tex_objs[0]) != want(tex_objs[0]);
        if (key != tex_last || force || sel_changed || drift) {
            tex_last = key;
            for (uint32_t o : tex_objs)
                if (!sel.count(o)) set_color(o, want(o));
        }
    }
}

// Ventana de color de un trigger P1/P2: botones para elegir a quién afecta.
const char* const kChName[] = {"", "P1", "P2", "BG", "GR"};

// ---- Controles: casillas, flechas, botones de color e información
uint32_t cstr(const std::string& t) {
    static std::map<std::string, uint32_t> m;
    auto it = m.find(t);
    return it != m.end() ? it->second : (m[t] = mem::strdup(t.c_str()));
}
uint32_t sprite_button(uint32_t menu, const std::string& frame, uint32_t target, uint32_t cb, float x, float y, float sc = 1.f, bool flip = false) {
    uint32_t spr = call(G(0x1e5d04), {sprite_frame(frame)});
    if (sc != 1.f) scale(spr, sc);
    if (flip) call(G(0x1e442c), {spr, 1});
    uint32_t item = call(G(0x107b84), {spr, 0, target, cb, 0});
    add(menu, item);
    pos(item, x, y);
    return item;
}
uint32_t item_sprite(uint32_t item) {
    std::vector<uint32_t> ch;
    if (item) children(item, ch);
    return ch.empty() ? 0 : ch[0];
}
void check_set(uint32_t item, bool on, bool enabled = true) {
    uint32_t spr = item_sprite(item);
    if (!spr) return;
    call(G(0x1e4930), {spr, sprite_frame(on ? "GJ_checkOn_001.png" : "GJ_checkOff_001.png")});
    set_color(spr, enabled ? 0xffffff : 0x606060);
}
void button_text(uint32_t item, const std::string& t) {
    uint32_t bs = item_sprite(item);  // ButtonSprite del botón
    if (bs) call(G(0x102448), {bs, gstr(t)});
}
void button_bg(uint32_t item, const char* image) {  // GJ_button_01 verde, 02 celeste, 03 rosa, 04 gris
    uint32_t bs = item_sprite(item);
    if (bs) call(G(0x103360), {bs, cstr(image)});
}
uint32_t text_label(uint32_t parent_node, const std::string& t, float x, float y, float sc, const char* font = "bigFont.fnt") {
    uint32_t l = label(t, font);
    scale(l, sc);
    pos(l, x, y);
    add(parent_node, l, 5);
    return l;
}
void show_info(const std::string& title, const std::string& text) {
    uint32_t a = call(G(0x1135fc), {0, cstr(title), cstr(text), cstr("OK"), 0, f2u(340)});
    if (a) call(mem::r32(mem::r32(a) + 0x184), {a});
}
void set_size(uint32_t scale9, float w, float h) {
    uint32_t sz = mem::alloc(0x40);
    call(G(0x19ef20), {sz, f2u(w), f2u(h)});  // CCSize(w, h)
    call(G(0x1ae28c), {scale9, sz});          // CCScale9Sprite::setContentSize
    call(G(0xfcd80), {sz});
    mem::free(sz);
}
uint32_t find_child_vt(uint32_t node, uint32_t vt, int depth = 2) {
    std::vector<uint32_t> ch;
    children(node, ch);
    for (uint32_t c : ch) {
        if (mem::r32(c) == vt) return c;
        if (depth > 0)
            if (uint32_t r = find_child_vt(c, vt, depth - 1)) return r;
    }
    return 0;
}
constexpr uint32_t kScale9vt = 0x01422610;

// ---- Ventana de color del Level End (ColorSelectPopup del juego con controles propios)
uint32_t g_ct_pop = 0, g_ct_obj = 0, g_ct_ch = 0, g_ct_nb = 0, g_ct_copybtn = 0;
std::vector<uint32_t> g_tx_pending;  // Level End para abrir la ventana de textura en el próximo tick
std::vector<uint32_t> g_ct_objs;     // todos los Level End seleccionados al abrir la ventana
void ct_buttons_refresh() {
    LeCfg cfg = g_trig.count(g_ct_obj) ? g_trig[g_ct_obj] : LeCfg{};
    bool p2 = cfg.target == kChP2;
    button_text(g_ct_ch, p2 ? "P2" : "P1");
    button_bg(g_ct_ch, p2 ? "GJ_button_03.png" : "GJ_button_01.png");
    check_set(g_ct_nb, !cfg.nb);
    button_text(g_ct_copybtn, cfg.copy ? std::string("Copy: ") + kChName[cfg.copy] : "Copy Color");
}
void ct_toggle_channel() {
    if (!g_ct_obj) return;
    LeCfg& cfg = g_trig[g_ct_obj];
    cfg.target = cfg.target == kChP2 ? kChP1 : kChP2;
    if (cfg.copy == cfg.target) cfg.copy = 0;
    ct_buttons_refresh();
}
void ct_toggle_nb() {
    if (!g_ct_obj) return;
    LeCfg& cfg = g_trig[g_ct_obj];
    cfg.nb = !cfg.nb;
    ct_buttons_refresh();
}
// Ventana "Copy Color": 4 casillas; se marca una o ninguna. La del propio canal no se puede marcar.
uint32_t g_cc_box[5] = {0, 0, 0, 0, 0};
void cc_refresh() {
    LeCfg cfg = g_trig.count(g_ct_obj) ? g_trig[g_ct_obj] : LeCfg{};
    for (int i = kChP1; i <= kChGR; i++) check_set(g_cc_box[i], cfg.copy == i, i != cfg.target);
    ct_buttons_refresh();
}
void cc_toggle(int ch) {
    if (!g_ct_obj) return;
    LeCfg& cfg = g_trig[g_ct_obj];
    if (ch == cfg.target) return;
    cfg.copy = cfg.copy == ch ? 0 : ch;
    cc_refresh();
}
void cc_open() {
    uint32_t a = call(G(0x1135fc), {0, cstr("Copy Color"), cstr("\n\n\n"), cstr("OK"), 0, f2u(320)});
    if (!a) return;
    uint32_t menu = mem::r32(a + 0x1b0), layer = mem::r32(a + 0x1c8);
    float w, h;
    win_size(w, h);
    float mx = px(menu), my = py(menu);
    static uint32_t on[5] = {0,
        cpu::add_thunk("Copy Color: P1", [](cpu::Cpu&) { cc_toggle(kChP1); }),
        cpu::add_thunk("Copy Color: P2", [](cpu::Cpu&) { cc_toggle(kChP2); }),
        cpu::add_thunk("Copy Color: BG", [](cpu::Cpu&) { cc_toggle(kChBG); }),
        cpu::add_thunk("Copy Color: GR", [](cpu::Cpu&) { cc_toggle(kChGR); })};
    for (int i = kChP1; i <= kChGR; i++) {
        float x = w / 2 - 105 + (i - 1) * 70;
        g_cc_box[i] = sprite_button(menu, "GJ_checkOff_001.png", a, on[i], x - mx, h / 2 + 8 - my, 1.f);
        text_label(layer, kChName[i], x, h / 2 - 26, 0.6f);
    }
    cc_refresh();
    call(mem::r32(mem::r32(a) + 0x184), {a});
}

void decorate_trigger_popup(uint32_t pop, uint32_t obj) {
    g_ct_pop = pop;
    g_ct_obj = obj;
    LeCfg& cfg = g_trig[obj];
    if (cfg.target != kChP1 && cfg.target != kChP2) cfg.target = kChP1;
    float w, h;
    win_size(w, h);
    // Fondo más ancho para que entren los controles.
    std::vector<uint32_t> top;
    children(pop, top);
    for (uint32_t c : top)
        if (uint32_t bg = find_child_vt(c, kScale9vt, 0)) { set_size(bg, 420, 300); break; }
    static uint32_t on_ch = cpu::add_thunk("Trigger: P1/P2", [](cpu::Cpu&) { ct_toggle_channel(); });
    static uint32_t on_copy = cpu::add_thunk("Trigger: Copy Color", [](cpu::Cpu&) { cc_open(); });
    static uint32_t on_nb = cpu::add_thunk("Trigger: Blending", [](cpu::Cpu&) { ct_toggle_nb(); });
    static uint32_t on_info = cpu::add_thunk("Trigger: info", [](cpu::Cpu&) {
        show_info("Color Trigger",
                  "Changes the color of <cg>P1</c> or <cg>P2</c> objects when the player passes it.\n"
                  "<cy>P1/P2</c>: channel to change.\n<cy>Copy Color</c>: follow the color of another channel.\n"
                  "<cy>Blending</c>: additive blending (off = solid colors).\n<cy>Texture</c>: turn this object into a decoration.");
    });
    static uint32_t on_tex = cpu::add_thunk("Trigger: Texture", [](cpu::Cpu&) {
        g_tx_pending = g_ct_objs;
        if (g_tx_pending.empty() && g_ct_obj) g_tx_pending.push_back(g_ct_obj);
        if (g_ct_pop) call(G(0x16dee4), {g_ct_pop});  // closeColorSelect
        g_ct_pop = 0;
    });
    uint32_t menu = new_menu();
    add(pop, menu, 50);
    g_ct_ch = button(menu, "P1", pop, on_ch, w / 2 - 165, 32, 0.8f);
    button(menu, "Texture", pop, on_tex, w / 2 + 155, 32, 0.7f);
    g_ct_copybtn = button(menu, "Copy Color", pop, on_copy, w / 2 + 148, h / 2 + 50, 0.62f);
    g_ct_nb = sprite_button(menu, "GJ_checkOff_001.png", pop, on_nb, w / 2 + 115, h / 2 + 5, 0.85f);
    text_label(pop, "Blending", w / 2 + 165, h / 2 + 5, 0.45f);
    sprite_button(menu, "GJ_infoIcon_001.png", pop, on_info, w / 2 + 188, h / 2 + 128, 1.f);
    ct_buttons_refresh();
}

uint32_t find_play_layer() {
    const uint32_t vt = 0x01000000 + 0x414f08 + 8;
    uint32_t d = call(G(0x1a4420), {}), scene = mem::r32(d + 0x70);
    if (!scene) return 0;
    std::vector<uint32_t> ch;
    children(scene, ch);
    for (uint32_t e : ch)
        if (mem::r32(e) == vt) return e;
    return 0;
}

int mix(int a, int b, float t) {
    int r = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        int x = (a >> sh) & 255, y = (b >> sh) & 255;
        r |= (int(x + (y - x) * t + 0.5f) & 255) << sh;
    }
    return r;
}

// Objetos del nivel en juego (PlayLayer: secciones, cada una un CCArray de objetos).
void play_objects(uint32_t pl, std::vector<uint32_t>& out) {
    out.clear();
    uint32_t secs = mem::r32(pl + 0x184);
    if (!secs) return;
    uint32_t n = call(G(0x1a2e20), {secs});
    for (uint32_t i = 0; i < n; i++) {
        uint32_t arr = call(G(0x1a2e44), {secs, i});
        if (!arr) continue;
        uint32_t m = call(G(0x1a2e20), {arr});
        for (uint32_t j = 0; j < m; j++) out.push_back(call(G(0x1a2e44), {arr, j}));
    }
}

// No Blending: los objetos de color de jugador se dibujan en una capa con mezcla aditiva. Para verlos con
// colores sólidos se pasan a una capa normal, creada justo encima de la aditiva (mismo orden de dibujo
// respecto a los bloques). El objeto guarda su capa en +0x328: el juego lo vuelve a agregar ahí al mostrarlo.
uint32_t g_solid_batch = 0, g_solid_for = 0;
std::map<uint32_t, uint32_t> g_orig_parent;  // objeto -> su capa original
uint32_t solid_batch(uint32_t pl) {
    if (g_solid_for == pl && g_solid_batch) return g_solid_batch;
    uint32_t glow = mem::r32(pl + 0x25c);
    if (!glow) return 0;
    uint32_t b = call(G(0x1e7cf8), {call(G(0x1e6b98), {glow}), 29});  // CCSpriteBatchNode::create(textura de la capa)
    add(parent(glow), b, int(call(G(0x19c6b4), {glow})));
    g_solid_for = pl;
    g_solid_batch = b;
    g_orig_parent.clear();
    return b;
}
void set_object_layer(uint32_t o, uint32_t layer) {
    if (!layer) return;
    if (!g_orig_parent.count(o)) g_orig_parent[o] = mem::r32(o + 0x328);
    mem::w32(o + 0x328, layer);
    uint32_t cur = parent(o);
    if (!cur || cur == layer) return;
    int z = int(call(G(0x19c6b4), {o}));
    call(G(0x1a1ee8), {o});            // retain
    call(G(0x19c7dc), {o, 0});         // removeFromParentAndCleanup(false)
    add(layer, o, z);
    call(G(0x1a1f54), {o});            // release
}
void set_blending(uint32_t pl, const std::vector<uint32_t>& objs, bool solid) {
    uint32_t sb = solid ? solid_batch(pl) : 0;
    for (uint32_t o : objs) {
        if (solid) { set_object_layer(o, sb); continue; }
        auto it = g_orig_parent.find(o);
        if (it != g_orig_parent.end()) set_object_layer(o, it->second);
    }
}

// Al jugar: aplica los triggers según la posición del jugador (sirve igual en practice y checkpoints).
void triggers_tick() {
    uint32_t pl = find_play_layer();
    if (!pl) return;
    if (pl != g_play_layer) { g_play_layer = pl; g_p1_objs.clear(); g_p2_objs.clear(); }  // nivel sin objetos P1/P2
    uint32_t player = mem::r32(pl + 0x22c);
    if (!player) return;
    float x = px(player);
    static std::vector<ColEv> ev;
    static uint32_t ev_for = 0;
    static bool ours[5], any_nb;
    static int last[5], nb_state[5];
    struct TexSpr { uint32_t spr; int ch; };
    static std::vector<TexSpr> tex;
    struct OvObj { uint32_t obj; int ch; };  // ch: 1 White, o un canal
    static std::vector<OvObj> ov;
    static int ov_last[5];
    static int tex_last[5];
    if (ev_for != pl) {  // eventos de color de este nivel, ordenados por X
        ev_for = pl;
        ev.clear();
        std::vector<uint32_t> all;
        play_objects(pl, all);
        tex.clear();
        ov.clear();
        std::set<uint32_t> own;
        for (uint32_t o : all) {
            auto oc = g_objcfg.find(o);
            if (oc == g_objcfg.end()) continue;
            if (oc->second.nopulse) *mem::ptr<uint8_t>(o + 0x2f0) = 0;  // deja de escalar con la música
            if (oc->second.ch) {
                ov.push_back({o, oc->second.ch == 1 ? 0 : objch_channel(oc->second.ch)});
                own.insert(o);
            }
        }
        if (!own.empty()) {  // los que tienen canal propio salen de las listas P1/P2
            auto drop = [&](std::vector<uint32_t>& v) { v.erase(std::remove_if(v.begin(), v.end(), [&](uint32_t o) { return own.count(o) > 0; }), v.end()); };
            drop(g_p1_objs);
            drop(g_p2_objs);
        }
        for (int i = 0; i < 5; i++) ov_last[i] = -1;
        for (uint32_t o : all) {
            collect_event(o, ev);
            if (mem::r32(o + 0x31c) != kLevelEndID) continue;
            auto it = g_trig.find(o);
            if (it == g_trig.end() || it->second.tex.empty()) continue;
            uint32_t fr = sprite_frame(it->second.tex);
            if (!fr) continue;
            // El juego no muestra el Level End: se dibuja un sprite propio con su posición y forma.
            // Con mezcla: en la capa de los glows. Sin mezcla: los glows sobre ella y el resto delante de los bloques.
            bool glow = is_glow_tex(it->second.tex), additive = !it->second.nb;
            uint32_t layer = additive ? mem::r32(pl + 0x25c) : glow ? solid_batch(pl) : mem::r32(pl + 0x258);
            if (!layer) continue;
            uint32_t spr = call(G(0x1e5d04), {fr});
            add(layer, spr, glow || additive ? 0 : 100);
            pos(spr, px(o), py(o));
            copy_transform(o, spr);
            tex.push_back({spr, it->second.tex_col});
        }
        for (int i = 0; i < 5; i++) tex_last[i] = -1;
        if (!tex.empty()) logf("[colores] %zu objetos con textura", tex.size());
        sort_events(ev);
        for (int i = 0; i < 5; i++) { ours[i] = false; last[i] = -1; nb_state[i] = 0; }
        any_nb = false;
        int n = 0;
        for (auto& e : ev)
            if (e.ours) { ours[e.ch] = true; n++; any_nb |= e.nb; }
        if (n) logf("[colores] %d triggers Level End en el nivel (%zu objetos)", n, all.size());
    }
    if (!ours[kChP1] && !ours[kChP2] && !ours[kChBG] && !ours[kChGR] && tex.empty() && ov.empty()) return;
    int init[5] = {0, g_play_p1 >= 0 ? g_play_p1 : player_color(1), g_play_p2 >= 0 ? g_play_p2 : player_color(2),
                   g_play_bg >= 0 ? g_play_bg : 0x00ff6628, g_play_gr >= 0 ? g_play_gr : 0x00ff6600};
    if (!ov.empty()) {  // objetos con canal propio
        int cc[5];
        channel_colors(ev, x, init, cc);
        cc[0] = 0xffffff;
        bool changed = false;
        for (int i = 0; i < 5; i++) if (cc[i] != ov_last[i]) { ov_last[i] = cc[i]; changed = true; }
        if (changed)
            for (auto& o : ov) set_color(o.obj, cc[o.ch]);
    }
    if (!tex.empty()) {  // texturas teñidas con el color de su canal
        int cc[5];
        channel_colors(ev, x, init, cc);
        bool changed = false;
        for (int i = 1; i < 5; i++) if (cc[i] != tex_last[i]) { tex_last[i] = cc[i]; changed = true; }
        if (changed)
            for (auto& t : tex) if (t.ch) set_color(t.spr, cc[t.ch]);
    }
    for (int wch = kChP1; wch <= kChP2; wch++) {
        int col = eval_channel(ev, wch, x, init, 1u << wch);
        if (col == last[wch]) continue;
        last[wch] = col;
        for (uint32_t o : wch == kChP1 ? g_p1_objs : g_p2_objs) set_color(o, col);
    }
    // No Blending: lo decide el último trigger del canal que el jugador pasó.
    for (int wch = kChP1; wch <= kChP2 && any_nb; wch++) {
        const ColEv* le = last_event(ev, wch, x);
        int want = le && le->ours && le->nb ? 1 : 0;
        if (want == nb_state[wch]) continue;
        nb_state[wch] = want;
        set_blending(pl, wch == kChP1 ? g_p1_objs : g_p2_objs, want);
    }
    // Fondo y suelo: los manejamos solo mientras el último trigger del canal sea nuestro.
    for (int wch = kChBG; wch <= kChGR; wch++) {
        if (!ours[wch]) continue;
        const ColEv* le = last_event(ev, wch, x);
        if (!le || !le->ours) { last[wch] = -1; continue; }
        int col = eval_channel(ev, wch, x, init, 1u << wch);
        if (col == last[wch]) continue;
        last[wch] = col;
        call(wch == kChBG ? G(0x13abec) : G(0x13aa68), {pl, uint32_t(col), f2u(0.f)});  // tintBackground / tintGround
    }
}

void colors_tick() {
    // Gesto secreto en Support
    uint32_t sl = 0;
    uint32_t btn = find_support_button(sl);
    if (sl != g_sup_layer) {
        g_sup_layer = sl;
        g_sup_btn = btn;
        g_sup_fired = false;
        if (btn && g_cc_on) support_text();
    }
    if (g_sup_btn && g_touch && !g_sup_fired && SDL_GetTicks() - g_touch_t >= 3000 && touch_inside(g_sup_btn)) {
        g_sup_fired = true;
        g_cc_on = !g_cc_on;
        cc_save();
        support_text();
        logf("[colores] colores P1/P2 %s", g_cc_on ? "activados" : "desactivados");
    }
    if (!g_touch) g_sup_fired = false;
    // Ajustes del nivel en el editor
    if (g_cc_on && layer_alive() && g_cc_layer != g_layer) decorate_colors(g_layer);
    if (!layer_alive()) g_cc_layer = 0;
}

// Después de actualizar el juego (y Ascella) y antes de dibujar: los colores de los triggers.
void after_update() {
    editor_preview_tick();
    triggers_tick();
}

void selected_objects(uint32_t ui, std::vector<uint32_t>& out) {
    out.clear();
    uint32_t arr = mem::r32(ui + 0x180);
    uint32_t n = arr ? call(G(0x1a2e20), {arr}) : 0;
    for (uint32_t i = 0; i < n; i++) out.push_back(call(G(0x1a2e44), {arr, i}));
    if (out.empty() && mem::r32(ui + 0x248)) out.push_back(mem::r32(ui + 0x248));
}
// ---- Ventana de textura (FLAlertLayer con una cuadrícula de cuadros de GJ_GameSheet)
constexpr int kTexCols = 6, kTexRows = 3, kTexPage = kTexCols * kTexRows;
constexpr int kTexCount = int(sizeof(kGameSheetFrames) / sizeof(kGameSheetFrames[0]));
constexpr int kTexPages = (kTexCount + kTexPage - 1) / kTexPage;
struct TexUI {
    uint32_t alert = 0, vt = 0, menu = 0, page_lbl = 0, nb_box = 0, col_btn = 0;
    uint32_t cells[kTexPage] = {};
    std::vector<uint32_t> objs;
    std::string tex;
    int col = 0, page = 0;
    bool nb = false;
} g_tx;
bool tx_alive() { return g_tx.alert && mem::r32(g_tx.alert) == g_tx.vt; }
void tx_refresh() {
    if (!tx_alive()) return;
    label_text(g_tx.page_lbl, std::to_string(g_tx.page + 1) + " / " + std::to_string(kTexPages));
    const char* names[] = {"White", "P1", "P2", "BG", "GR"};
    button_text(g_tx.col_btn, names[g_tx.col]);
    check_set(g_tx.nb_box, !g_tx.nb);
    for (int i = 0; i < kTexPage; i++) {  // la elegida, más grande
        if (!g_tx.cells[i]) continue;
        int k = g_tx.page * kTexPage + i;
        bool sel = k < kTexCount && g_tx.tex == kGameSheetFrames[k];
        scale(g_tx.cells[i], sel ? 1.3f : 1.f);
    }
}
void tx_build_page() {
    if (!tx_alive()) return;
    float w, h;
    win_size(w, h);
    float mx = px(g_tx.menu), my = py(g_tx.menu);
    static uint32_t on_cell[kTexPage];
    static bool init = false;
    if (!init) {
        init = true;
        for (int i = 0; i < kTexPage; i++)
            on_cell[i] = cpu::add_thunk("Texture: cell", [i](cpu::Cpu&) {
                int k = g_tx.page * kTexPage + i;
                if (k >= kTexCount) return;
                g_tx.tex = kGameSheetFrames[k];
                g_tx.nb = !is_glow_tex(g_tx.tex);  // los glows con mezcla; el resto, sólidos
                tx_refresh();
            });
    }
    for (int i = 0; i < kTexPage; i++) {
        if (g_tx.cells[i]) remove(g_tx.cells[i]);
        g_tx.cells[i] = 0;
        int k = g_tx.page * kTexPage + i;
        if (k >= kTexCount) continue;
        uint32_t fr = sprite_frame(kGameSheetFrames[k]);
        if (!fr) continue;
        uint32_t spr = call(G(0x1e5d04), {fr});
        float sw = width(spr), sh = u2f(mem::r32(spr + 0xa4 + 0x1c));
        float m = std::max(sw, sh);
        if (m > 0) scale(spr, std::min(1.f, 34.f / m));
        uint32_t item = call(G(0x107b84), {spr, 0, g_tx.alert, on_cell[i], 0});
        add(g_tx.menu, item);
        int c = i % kTexCols, r = i / kTexCols;
        pos(item, w / 2 - 150 + c * 60 - mx, h / 2 + 62 - r * 45 - my);
        g_tx.cells[i] = item;
    }
    tx_refresh();
}
void tx_apply() {
    if (g_tx.tex.empty()) return;
    for (uint32_t o : g_tx.objs) {
        if (mem::r32(o + 0x31c) != kLevelEndID) continue;
        LeCfg& cfg = g_trig[o];
        cfg.target = 0;   // la textura es permanente: deja de ser trigger
        cfg.copy = 0;
        cfg.tex = g_tx.tex;
        cfg.tex_col = g_tx.col;
        cfg.nb = g_tx.nb;
    }
    g_editor_dirty = true;
    logf("[colores] textura %s en %zu Level End", g_tx.tex.c_str(), g_tx.objs.size());
}
void tx_open(const std::vector<uint32_t>& objs) {
    if (tx_alive() || objs.empty()) return;
    static uint32_t proto = [] {
        uint32_t vt = mem::alloc(16), obj = mem::alloc(8);
        uint32_t th = cpu::add_thunk("Texture: FLAlert_Clicked", [](cpu::Cpu& c) {
            if (cpu::reg(c, 2)) tx_apply();  // botón 2 = OK
        });
        for (int i = 0; i < 4; i++) mem::w32(vt + 4 * i, th);
        mem::w32(obj, vt);
        return obj;
    }();
    g_tx = TexUI{};
    g_tx.objs = objs;
    auto it = g_trig.find(objs[0]);
    if (it != g_trig.end() && !it->second.tex.empty()) { g_tx.tex = it->second.tex; g_tx.col = it->second.tex_col; g_tx.nb = it->second.nb; }
    for (int k = 0; k < kTexCount; k++)
        if (g_tx.tex == kGameSheetFrames[k]) { g_tx.page = k / kTexPage; break; }
    uint32_t a = call(G(0x1135fc), {proto, cstr("Select Texture"), cstr("\n\n\n\n\n\n\n\n\n\n"), cstr("Cancel"), cstr("OK"), f2u(440)});
    if (!a) return;
    g_tx.alert = a;
    g_tx.vt = mem::r32(a);
    g_tx.menu = mem::r32(a + 0x1b0);
    uint32_t layer = mem::r32(a + 0x1c8);
    float w, h;
    win_size(w, h);
    float cx = w / 2, cy = h / 2, mx = px(g_tx.menu), my = py(g_tx.menu);
    // Bordes del fondo de la ventana.
    float bw = 440, bh = 280;
    if (uint32_t bg = find_child_vt(layer, kScale9vt, 1)) { bw = width(bg); bh = u2f(mem::r32(bg + 0xa4 + 0x1c)); }
    float left = cx - bw / 2, right = cx + bw / 2, top = cy + bh / 2, bottom = cy - bh / 2;
    static uint32_t on_prev = cpu::add_thunk("Texture: prev", [](cpu::Cpu&) {
        g_tx.page = (g_tx.page + kTexPages - 1) % kTexPages;
        tx_build_page();
    });
    static uint32_t on_next = cpu::add_thunk("Texture: next", [](cpu::Cpu&) {
        g_tx.page = (g_tx.page + 1) % kTexPages;
        tx_build_page();
    });
    static uint32_t on_col = cpu::add_thunk("Texture: color", [](cpu::Cpu&) { g_tx.col = (g_tx.col + 1) % 5; tx_refresh(); });
    static uint32_t on_nb = cpu::add_thunk("Texture: Blending", [](cpu::Cpu&) { g_tx.nb = !g_tx.nb; tx_refresh(); });
    static uint32_t on_info = cpu::add_thunk("Texture: info", [](cpu::Cpu&) {
        show_info("Select Texture",
                  "The object becomes a decoration with this texture, without collision.\n"
                  "<cy>Color</c>: White, or follow the color of P1, P2, BG or GR.\n"
                  "<cy>Blending</c>: additive blending, black becomes transparent (off = solid).\n"
                  "Delete the object to remove the texture.");
    });
    sprite_button(g_tx.menu, "GJ_arrow_01_001.png", a, on_prev, left + 28 - mx, cy + 17 - my, 0.85f);
    sprite_button(g_tx.menu, "GJ_arrow_01_001.png", a, on_next, right - 28 - mx, cy + 17 - my, 0.85f, true);
    g_tx.col_btn = button(g_tx.menu, "White", a, on_col, left + 62 - mx, bottom + 28 - my, 0.75f);
    button_bg(g_tx.col_btn, "GJ_button_02.png");
    g_tx.nb_box = sprite_button(g_tx.menu, "GJ_checkOff_001.png", a, on_nb, right - 112 - mx, bottom + 28 - my, 0.85f);
    text_label(layer, "Blending", right - 55, bottom + 28, 0.45f);
    sprite_button(g_tx.menu, "GJ_infoIcon_001.png", a, on_info, right - 24 - mx, top - 24 - my, 1.f);
    g_tx.page_lbl = text_label(layer, " ", cx, bottom + 62, 0.45f, "goldFont.fnt");
    tx_build_page();
    call(mem::r32(mem::r32(a) + 0x184), {a});  // show
}

// ---- Ventana "Object Color": canal de color y pulsación de los objetos seleccionados
struct ObjUI { uint32_t col_btn = 0, pulse_box = 0; std::vector<uint32_t> objs; bool any_pulse = false; } g_ou;
void ou_refresh() {
    if (g_ou.objs.empty()) return;
    ObjCfg cfg = g_objcfg.count(g_ou.objs[0]) ? g_objcfg[g_ou.objs[0]] : ObjCfg{};
    button_text(g_ou.col_btn, kObjChName[cfg.ch]);
    if (g_ou.pulse_box) check_set(g_ou.pulse_box, !cfg.nopulse);
}
void ou_set(int ch, int nopulse) {  // -1: no cambia
    for (uint32_t o : g_ou.objs) {
        ObjCfg& cfg = g_objcfg[o];
        if (ch >= 0) cfg.ch = ch;
        if (nopulse >= 0 && pulses(o)) cfg.nopulse = nopulse != 0;
        if (!cfg.ch && !cfg.nopulse) g_objcfg.erase(o);
    }
    g_editor_dirty = true;
    ou_refresh();
}
void ou_open(const std::vector<uint32_t>& objs) {
    if (objs.empty()) return;
    g_ou = ObjUI{};
    g_ou.objs = objs;
    for (uint32_t o : objs) g_ou.any_pulse |= pulses(o);
    uint32_t a = call(G(0x1135fc), {0, cstr("Object Color"), cstr("\n\n\n"), cstr("OK"), 0, f2u(320)});
    if (!a) return;
    uint32_t menu = mem::r32(a + 0x1b0), layer = mem::r32(a + 0x1c8);
    float w, h;
    win_size(w, h);
    float cx = w / 2, cy = h / 2, mx = px(menu), my = py(menu);
    float bw = 320, bh = 160;
    if (uint32_t bg = find_child_vt(layer, kScale9vt, 1)) { bw = width(bg); bh = u2f(mem::r32(bg + 0xa4 + 0x1c)); }
    static uint32_t on_col = cpu::add_thunk("Object Color: color", [](cpu::Cpu&) {
        int ch = g_objcfg.count(g_ou.objs[0]) ? g_objcfg[g_ou.objs[0]].ch : 0;
        ou_set((ch + 1) % 6, -1);
    });
    static uint32_t on_pulse = cpu::add_thunk("Object Color: Pulse", [](cpu::Cpu&) {
        bool np = g_objcfg.count(g_ou.objs[0]) && g_objcfg[g_ou.objs[0]].nopulse;
        ou_set(-1, np ? 0 : 1);
    });
    static uint32_t on_info = cpu::add_thunk("Object Color: info", [](cpu::Cpu&) {
        show_info("Object Color",
                  "<cy>Color</c>: Default keeps the original color. White, BG, GR, P1 or P2 make the object follow that color.\n"
                  "<cy>Pulse</c>: the object pulses with the music (off = static).");
    });
    float row = cy + 8;
    g_ou.col_btn = button(menu, "Default", a, on_col, (g_ou.any_pulse ? cx - 60 : cx) - mx, row - my, 0.75f);
    button_bg(g_ou.col_btn, "GJ_button_02.png");
    if (g_ou.any_pulse) {
        g_ou.pulse_box = sprite_button(menu, "GJ_checkOff_001.png", a, on_pulse, cx + 60 - mx, row - my, 0.85f);
        text_label(layer, "Pulse", cx + 100, row, 0.45f);
    }
    sprite_button(menu, "GJ_infoIcon_001.png", a, on_info, cx + bw / 2 - 24 - mx, cy + bh / 2 - 24 - my, 1.f);
    ou_refresh();
    call(mem::r32(mem::r32(a) + 0x184), {a});
}

// Level End seleccionados (uno o varios).
std::vector<uint32_t> selected_level_ends(uint32_t ui) {
    std::vector<uint32_t> sel, out;
    selected_objects(ui, sel);
    for (uint32_t o : sel)
        if (o && mem::r32(o + 0x31c) == kLevelEndID) out.push_back(o);
    return out;
}

// Abre la ventana del Level End seleccionado. Devuelve false si lo seleccionado no es un Level End.
bool open_le_editor(uint32_t ui) {
    if (!g_cc_on) return false;
    std::vector<uint32_t> les = selected_level_ends(ui);
    if (les.empty()) {
        std::vector<uint32_t> sel, eds;
        selected_objects(ui, sel);
        for (uint32_t o : sel)
            if (obj_editable(o)) eds.push_back(o);
        if (eds.empty()) return false;
        ou_open(eds);
        return true;
    }
    uint32_t obj = les[0];
    g_ct_objs = les;
    auto tit = g_trig.find(obj);
    if (tit != g_trig.end() && !tit->second.tex.empty()) {
        tx_open(les);
        return true;
    }
    if (!g_trig.count(obj)) {
        g_trig[obj].target = kChP1;
        call(G(0x14a57c), {obj, uint32_t(player_color(1))});  // setTintColor (por valor): empieza con el color del jugador
        call(G(0x14a5a8), {obj, f2u(0.5f)});                  // setTintDuration
    }
    uint32_t pop = call(G(0x16e758), {obj});                   // ColorSelectPopup::create
    decorate_trigger_popup(pop, obj);
    call(mem::r32(mem::r32(pop) + 0x184), {pop});              // show
    return true;
}

// Copy + Paste de Ascella crea copias con el mismo ID y posición, pero sin nuestras claves: se anota la
// configuración de los Level End seleccionados y, tras pegar, se pasa a las copias (misma posición).
void copy_paste_keep(uint32_t ui, uint32_t item, uint32_t orig) {
    struct Src { float x, y; LeCfg cfg; uint32_t tint; uint32_t dur; };
    struct OSrc { float x, y; uint32_t id; ObjCfg cfg; };
    std::vector<Src> src;
    std::vector<OSrc> osrc;
    std::vector<uint32_t> sel;
    selected_objects(ui, sel);
    for (uint32_t o : sel) {
        auto oc = g_objcfg.find(o);
        if (oc != g_objcfg.end()) osrc.push_back({px(o), py(o), mem::r32(o + 0x31c), oc->second});
        auto it = g_trig.find(o);
        if (it != g_trig.end() && mem::r32(o + 0x31c) == kLevelEndID)
            src.push_back({px(o), py(o), it->second, mem::r32(o + 0x314), mem::r32(o + 0x318)});
    }
    cpu::call(orig, {item});
    if (src.empty() && osrc.empty()) return;
    uint32_t ed = find_editor();
    if (!ed) return;
    std::vector<uint32_t> all;
    editor_objects(call(G(0x1501cc), {ed}), all);
    int done = 0;
    g_editor_dirty = true;
    for (uint32_t o : all) {
        if (g_objcfg.count(o)) continue;
        for (auto it = osrc.begin(); it != osrc.end(); ++it) {
            if (it->id != mem::r32(o + 0x31c) || std::fabs(px(o) - it->x) > 0.5f || std::fabs(py(o) - it->y) > 0.5f) continue;
            g_objcfg[o] = it->cfg;
            osrc.erase(it);
            done++;
            break;
        }
    }
    for (uint32_t o : all) {
        if (mem::r32(o + 0x31c) != kLevelEndID || g_trig.count(o)) continue;
        for (auto it = src.begin(); it != src.end(); ++it) {
            if (std::fabs(px(o) - it->x) > 0.5f || std::fabs(py(o) - it->y) > 0.5f) continue;
            g_trig[o] = it->cfg;
            call(G(0x14a57c), {o, it->tint & 0xffffff});  // setTintColor
            mem::w32(o + 0x318, it->dur);
            src.erase(it);
            done++;
            break;
        }
    }
    if (done) logf("[colores] copiar y pegar: %d objetos con su configuración", done);
}

void install_colors() {
    cc_load();
    // CCScheduler::update: Ascella ya la redirige ("ldr.w pc, [pc]" + dirección); se encadena su salto.
    constexpr uint32_t kSchedUpdate = 0x0118a96c;
    static uint32_t orig_sched = 0;
    cpu::Handler sched = [](cpu::Cpu& c) {
        cpu::call(orig_sched, {cpu::reg(c, 0), cpu::reg(c, 1)});
        after_update();
    };
    if (mem::r32(kSchedUpdate) == 0xf000f85f) {
        orig_sched = mem::r32(kSchedUpdate + 4);
        mem::w32(kSchedUpdate + 4, cpu::add_thunk("CCScheduler::update (colores)", std::move(sched)));
        cpu::invalidate(kSchedUpdate, 8);
    } else {
        orig_sched = hook_entry(kSchedUpdate, "CCScheduler::update (colores)", std::move(sched));
    }
    // Al armar el nivel: objetos con color de jugador 1 (obj->setColor(jugador->getColor())).
    patch_jump(0x0113e9fe, 0x0113ea18, "Objetos P1 (color del nivel)", [](cpu::Cpu& c) {
        uint32_t obj = cpu::reg(c, 5), pl = cpu::reg(c, 4);
        uint32_t setc = mem::r32(mem::r32(obj) + 0x134);
        if (pl != g_play_layer) { g_play_layer = pl; g_p1_objs.clear(); g_p2_objs.clear(); }
        g_p1_objs.push_back(obj);
        uint32_t col;
        if (g_play_p1 >= 0) col = color_ptr(g_play_p1);
        else {
            uint32_t player = mem::r32(pl + 0x22c);
            col = call(mem::r32(mem::r32(player) + 0x130), {player});  // getColor
        }
        call(setc, {obj, col});
        cpu::reg(c, 7) = setc;
    });
    // ... y de jugador 2 (obj->setColor(colorForIdx(getPlayerColor2()))).
    patch_jump(0x0113eed6, 0x0113ea18, "Objetos P2 (color del nivel)", [](cpu::Cpu& c) {
        uint32_t obj = cpu::reg(c, 5), pl = cpu::reg(c, 4);
        uint32_t setc = mem::r32(mem::r32(obj) + 0x134);
        if (pl != g_play_layer) { g_play_layer = pl; g_p1_objs.clear(); g_p2_objs.clear(); }
        g_p2_objs.push_back(obj);
        call(setc, {obj, color_ptr(g_play_p2 >= 0 ? g_play_p2 : player_color(2))});
        cpu::reg(c, 7) = setc;
    });
    // Selector de color cerrado: etiquetas 2/3 son nuestras (P1/P2); el resto, BG/Ground del juego.
    static uint32_t orig = 0;
    orig = hook_entry(0x011504b0, "colorPickerClosed (P1/P2)", [](cpu::Cpu& c) {
        uint32_t layer = cpu::reg(c, 0), pk = cpu::reg(c, 1);
        uint32_t tag = call(G(0x19c7bc), {pk});
        if (tag == 2 || tag == 3) {
            int col = int(call(G(0x16e828), {pk}) & 0xffffff);  // getColorValue
            uint32_t lso = mem::r32(layer + 0x1dc);
            (tag == 2 ? g_lso_p1 : g_lso_p2)[lso] = col;
            logf("[colores] %s = %s", tag == 2 ? "P1" : "P2", color_str(col).c_str());
            refresh_squares();
            return;
        }
        cpu::call(orig, {layer, pk});
    });
    // Texto de cada objeto al guardar: justo antes de unir las piezas (r4 = objeto, r5 = piezas).
    // Se reemplazan "ldr r2, [pc, #0x108]; movs r4, #0".
    patch_jump(0x0114f7dc, 0x0114f7e0, "GameObject::getSaveString (kCT)", [](cpu::Cpu& c) {
        uint32_t obj = cpu::reg(c, 4), arr = cpu::reg(c, 5);
        auto it = g_trig.find(obj);
        if (it != g_trig.end() && mem::r32(obj + 0x31c) == kLevelEndID) {
            static uint32_t fmt = mem::strdup("%s");
            const LeCfg& cfg = it->second;
            std::vector<std::string> kv;
            if (cfg.target) {
                uint32_t tc = mem::r32(obj + 0x314) & 0xffffff;           // color del trigger (tint)
                float dur = u2f(mem::r32(obj + 0x318));                  // duración
                char d[32];
                std::snprintf(d, sizeof d, "%.2f", dur);
                kv = {"7", std::to_string(tc & 255), "8", std::to_string((tc >> 8) & 255), "9", std::to_string((tc >> 16) & 255),
                      "10", d, "kCT", std::to_string(cfg.target)};
                if (cfg.copy) kv.insert(kv.end(), {"kCC", std::to_string(cfg.copy)});
            }
            if (cfg.nb) kv.insert(kv.end(), {"kNB", "1"});
            if (!cfg.tex.empty()) {
                kv.insert(kv.end(), {"kTX", cfg.tex});
                if (cfg.tex_col) kv.insert(kv.end(), {"kTC", std::to_string(cfg.tex_col)});
            }
            for (auto& v : kv) call(G(0x1a3108), {arr, call(G(0x1a2d1c), {fmt, gstr(v)})});
        }
        auto oc = g_objcfg.find(obj);
        if (oc != g_objcfg.end()) {
            static uint32_t fmt2 = mem::strdup("%s");
            std::vector<std::string> kv;
            if (oc->second.ch) kv.insert(kv.end(), {"kCH", std::to_string(oc->second.ch)});
            if (oc->second.nopulse) kv.insert(kv.end(), {"kNP", "1"});
            for (auto& v : kv) call(G(0x1a3108), {arr, call(G(0x1a2d1c), {fmt2, gstr(v)})});
        }
        cpu::reg(c, 2) = mem::r32(0x0114f8e8);
        cpu::reg(c, 4) = 0;
    });
    // Al leer un objeto: kCT del texto (bl stringSetupToDict) ...
    hook_bl(0x0114fe3a, "GameObject::objectFromString (leer kCT)", [](cpu::Cpu& c) {
        const char* t = mem::str(mem::r32(cpu::reg(c, 0)));
        g_parsed_le = false;
        g_parsed_cfg = LeCfg{};
        g_parsed_has_obj = false;
        g_parsed_obj = ObjCfg{};
        if (t && (std::strstr(t, "kCH,") || std::strstr(t, "kNP,"))) g_parsed_has_obj = true;
        if (t && (std::strstr(t, "kCT,") || std::strstr(t, "kTX,") || std::strstr(t, "kNB,") || g_parsed_has_obj)) {  // pares clave,valor
            g_parsed_le = std::strstr(t, "kCT,") || std::strstr(t, "kTX,") || std::strstr(t, "kNB,");
            int rgb[3] = {255, 255, 255};
            float dur = 0.5f;
            std::string str(t);
            size_t i = 0;
            while (i < str.size()) {
                size_t a = str.find(',', i);
                if (a == std::string::npos) break;
                size_t b = str.find(',', a + 1);
                std::string k = str.substr(i, a - i), v = str.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
                if (k == "kCT") g_parsed_cfg.target = std::clamp(std::atoi(v.c_str()), 0, int(kChGR));
                else if (k == "kCC") g_parsed_cfg.copy = std::clamp(std::atoi(v.c_str()), 0, int(kChGR));
                else if (k == "kNB") g_parsed_cfg.nb = std::atoi(v.c_str()) != 0;
                else if (k == "kTX") g_parsed_cfg.tex = v;
                else if (k == "kTC") g_parsed_cfg.tex_col = std::clamp(std::atoi(v.c_str()), 0, int(kChGR));
                else if (k == "kCH") g_parsed_obj.ch = std::clamp(std::atoi(v.c_str()), 0, 5);
                else if (k == "kNP") g_parsed_obj.nopulse = std::atoi(v.c_str()) != 0;
                else if (k == "7" || k == "8" || k == "9") rgb[k[0] - '7'] = std::atoi(v.c_str());
                else if (k == "10") dur = float(std::atof(v.c_str()));
                if (b == std::string::npos) break;
                i = b + 1;
            }
            g_parsed_tint = (rgb[0] & 255) | ((rgb[1] & 255) << 8) | ((rgb[2] & 255) << 16);
            g_parsed_dur = dur;
        }
        cpu::reg(c, 0) = uint32_t(cpu::call(G(0x182f18), {cpu::reg(c, 0), cpu::reg(c, 1), cpu::reg(c, 2), cpu::reg(c, 3)}));
    });
    // ... y se asocia al objeto (bl customSetup, r0 = objeto).
    hook_bl(0x0114ff96, "GameObject::objectFromString (kCT)", [](cpu::Cpu& c) {
        uint32_t obj = cpu::reg(c, 0);
        call(G(0x14ea74), {obj});  // customSetup
        if (g_parsed_le && mem::r32(obj + 0x31c) == kLevelEndID && (g_parsed_cfg.target || !g_parsed_cfg.tex.empty())) {
            if (g_parsed_cfg.copy == g_parsed_cfg.target) g_parsed_cfg.copy = 0;
            g_trig[obj] = g_parsed_cfg;
            call(G(0x14a57c), {obj, uint32_t(g_parsed_tint)});  // setTintColor
            mem::w32(obj + 0x318, f2u(g_parsed_dur));            // duración
        } else {
            g_trig.erase(obj);
        }
        if (g_parsed_has_obj && mem::r32(obj + 0x31c) != kLevelEndID) g_objcfg[obj] = g_parsed_obj;
        else g_objcfg.erase(obj);
        g_parsed_le = false;
        g_parsed_has_obj = false;
    });
    // Editor: "Edit Object" también para el Level End (solo con la función activada).
    static uint32_t orig_menu = 0;
    orig_menu = hook_entry(0x01156bdc, "EditorUI::updateEditMenu (Level End)", [](cpu::Cpu& c) {
        uint32_t ui = cpu::reg(c, 0);
        cpu::call(orig_menu, {ui});
        uint32_t obj = mem::r32(ui + 0x248);
        uint32_t btn = mem::r32(ui + 0x1ac);
        if (g_cc_on && obj && btn && (mem::r32(obj + 0x31c) == kLevelEndID || obj_editable(obj)) && mem::r32(ui + 0x1c0) == 3)
            call(mem::r32(mem::r32(btn) + 0x74), {btn, 1});  // setVisible
    });
    static uint32_t orig_edit = 0;
    orig_edit = hook_entry(0x01154458, "EditorUI::editObject (Level End)", [](cpu::Cpu& c) {
        uint32_t ui = cpu::reg(c, 0);
        if (!open_le_editor(ui)) cpu::call(orig_edit, {ui});
    });
    // Objeto nuevo en el editor: puede ocupar la memoria de uno borrado; no hereda su configuración.
    hook_bl(0x01151846, "LevelEditorLayer::createObject (sin configuración previa)", [](cpu::Cpu& c) {
        uint32_t o = call(G(0x14fcb0), {cpu::reg(c, 0)});  // GameObject::createObject
        if (o) { g_trig.erase(o); g_objcfg.erase(o); }
        cpu::reg(c, 0) = o;
    });
    // Línea vertical de los triggers en el editor: no para los Level End con textura.
    static uint32_t orig_fx = 0;
    orig_fx = hook_entry(0x01151104, "DrawGridLayer::addToEffects (texturas)", [](cpu::Cpu& c) {
        g_draw_grid = cpu::reg(c, 0);
        if (is_texture_le(cpu::reg(c, 1))) return;
        cpu::call(orig_fx, {cpu::reg(c, 0), cpu::reg(c, 1)});
    });
    // Al cerrar la ventana de color: la configuración pasa a los demás Level End seleccionados.
    static uint32_t orig_closed = 0;
    orig_closed = hook_entry(0x01153c90, "EditorUI::colorSelectClosed (varios Level End)", [](cpu::Cpu& c) {
        cpu::call(orig_closed, {cpu::reg(c, 0), cpu::reg(c, 1)});
        auto src = g_trig.find(g_ct_obj);
        if (!g_ct_obj || src == g_trig.end() || g_ct_objs.size() < 2) return;
        uint32_t tint = mem::r32(g_ct_obj + 0x314) & 0xffffff, dur = mem::r32(g_ct_obj + 0x318);
        for (uint32_t o : g_ct_objs) {
            if (o == g_ct_obj || mem::r32(o + 0x31c) != kLevelEndID) continue;
            auto it = g_trig.find(o);
            if (it != g_trig.end() && !it->second.tex.empty()) continue;  // las texturas no se convierten en trigger
            g_trig[o] = src->second;
            call(G(0x14a57c), {o, tint});
            mem::w32(o + 0x318, dur);
        }
    });
    // Botones de Ascella en el editor: "Edit Object" (fuera de la pestaña Edit) y "Copy + Paste".
    static uint32_t orig_act = 0;
    orig_act = hook_entry(0x011d0f3c, "CCMenuItem::activate (botones de Ascella)", [](cpu::Cpu& c) {
        uint32_t item = cpu::reg(c, 0);
        uint32_t sel = mem::r32(item + 0x130), ui = mem::r32(item + 0x12c);
        bool enabled = (mem::r32(item + 0x128) & 0xff) != 0;
        if (g_cc_on && enabled && ui && sel == kAscellaEditObject && open_le_editor(ui)) return;
        if (enabled && ui && sel == kAscellaCopyPaste) {
            copy_paste_keep(ui, item, orig_act);
            return;
        }
        cpu::call(orig_act, {item});
    });
}
}  // namespace

void touch(bool down, float fx, float fy) {
    if (!g_on) return;
    if (down && !g_touch) g_touch_t = SDL_GetTicks();
    g_touch = down;
    g_touch_x = fx;
    g_touch_y = fy;
}

void install_level_header() {
    // getSaveString: justo antes de unir las piezas (bl CCArray::count) se añade "kNG", "<id>".
    hook_bl(0x01152638, "getSaveString (kNG)", [](cpu::Cpu& c) {
        uint32_t lso = cpu::reg(c, 6), arr = cpu::reg(c, 5);
        if (int ng = lso_ng(lso)) {
            static uint32_t fmt = mem::strdup("%s");
            call(G(0x1a3108), {arr, call(G(0x1a2d1c), {fmt, gstr("kNG")})});
            call(G(0x1a3108), {arr, call(G(0x1a2d1c), {fmt, gstr(std::to_string(ng))})});
        }
        static uint32_t fmt2 = mem::strdup("%s");
        for (int which = 1; which <= 2; which++) {
            int col = map_get(which == 1 ? g_lso_p1 : g_lso_p2, lso);
            if (col < 0) continue;
            call(G(0x1a3108), {arr, call(G(0x1a2d1c), {fmt2, gstr(which == 1 ? "kP1" : "kP2")})});
            call(G(0x1a3108), {arr, call(G(0x1a2d1c), {fmt2, gstr(color_str(col))})});
        }
        cpu::reg(c, 0) = call(G(0x1a2e20), {arr});  // CCArray::count
    });
    // objectFromString: se lee kNG del texto (bl stringSetupToDict) ...
    hook_bl(0x01152908, "objectFromString (leer kNG)", [](cpu::Cpu& c) {
        uint32_t str = cpu::reg(c, 0);
        g_parsed_ng = parse_ng(mem::str(mem::r32(str)));
        g_parsed_p1 = parse_color(mem::str(mem::r32(str)), "kP1,");
        g_parsed_p2 = parse_color(mem::str(mem::r32(str)), "kP2,");
        parse_ks(mem::str(mem::r32(str)), g_parsed_bg, g_parsed_gr);
        cpu::reg(c, 0) = uint32_t(cpu::call(G(0x182f18), {cpu::reg(c, 0), cpu::reg(c, 1), cpu::reg(c, 2), cpu::reg(c, 3)}));
    });
    // ... y se asocia al objeto creado (bl LevelSettingsObject::create).
    hook_bl(0x01152920, "objectFromString (kNG)", [](cpu::Cpu& c) {
        uint32_t lso = call(G(0x1508c0), {});
        if (g_parsed_ng) g_lso_ng[lso] = g_parsed_ng;
        else g_lso_ng.erase(lso);
        g_lso_track.erase(lso);
        g_play_ng = g_parsed_ng;
        if (g_parsed_p1 >= 0) g_lso_p1[lso] = g_parsed_p1; else g_lso_p1.erase(lso);
        if (g_parsed_p2 >= 0) g_lso_p2[lso] = g_parsed_p2; else g_lso_p2.erase(lso);
        g_play_p1 = g_parsed_p1;
        g_play_p2 = g_parsed_p2;
        if (g_parsed_bg >= 0) g_lso_bg[lso] = g_parsed_bg; else g_lso_bg.erase(lso);
        if (g_parsed_gr >= 0) g_lso_gr[lso] = g_parsed_gr; else g_lso_gr.erase(lso);
        g_play_bg = g_parsed_bg;
        g_play_gr = g_parsed_gr;
        if (g_parsed_p1 >= 0 || g_parsed_p2 >= 0)
            logf("[colores] nivel con colores propios: P1=%s P2=%s", g_parsed_p1 >= 0 ? color_str(g_parsed_p1).c_str() : "-",
                 g_parsed_p2 >= 0 ? color_str(g_parsed_p2).c_str() : "-");
        if (g_parsed_ng) logf("[canciones] nivel con canción de Newgrounds %d", g_parsed_ng);
        cpu::reg(c, 0) = lso;
    });
}

bool editor_open() { return g_on && popup_alive(); }

bool key(int ch) {
    if (!g_on) return false;
    if (!popup_alive()) return false;
    if (ch >= '0' && ch <= '9') {
        if (g_pop.typed.size() < 9) g_pop.typed += char(ch);
    } else if (ch == '\b') {
        if (!g_pop.typed.empty()) g_pop.typed.pop_back();
    } else if (ch == '\n') {
        g_action = kSearch;
        return true;
    } else {
        return false;  // Esc: lo cierra el propio juego
    }
    label_text(g_pop.id_label, g_pop.typed.empty() ? " " : g_pop.typed);
    return true;
}

void trace_player(int frame) {
    if (!g_on) return;
    static std::FILE* f = [] {
        const char* p = std::getenv("GD_TRACE_PLAYER");
        return p ? std::fopen(p, "w") : nullptr;
    }();
    if (!f) return;
    uint32_t pl = find_play_layer();
    uint32_t player = pl ? mem::r32(pl + 0x22c) : 0;
    if (!player) return;
    uint64_t yv;
    std::memcpy(&yv, mem::ptr(player + 0x378), 8);
    std::fprintf(f, "%d %08x %08x %08x %016llx %d%d%d%d\n", frame, mem::r32(player + 0x44), mem::r32(player + 0x48), mem::r32(player + 0x20), (unsigned long long)yv,
                 *mem::ptr<uint8_t>(player + 0x365), *mem::ptr<uint8_t>(player + 0x366), *mem::ptr<uint8_t>(player + 0x367),
                 *mem::ptr<uint8_t>(player + 0x430));
    if (std::getenv("GD_TRACE_OBJECTS")) {
        std::vector<uint32_t> objs;
        play_objects(pl, objs);
        uint64_t h = 1469598103934665603ull;
        for (uint32_t o : objs) {
            // Se saltan los punteros y contadores globales, que cambian entre ejecuciones.
            static const uint16_t skip[] = {0x004, 0x030, 0x054, 0x068, 0x088, 0x0a8, 0x0c8, 0x134, 0x13c, 0x164, 0x16c,
                                            0x184, 0x1a4, 0x1c8, 0x1e8, 0x26c, 0x280, 0x284, 0x290, 0x2cc, 0x2e8, 0x328};
            for (int w = 0; w < 0x32c; w += 4) {
                if (std::find(std::begin(skip), std::end(skip), w) != std::end(skip)) continue;
                h = (h ^ mem::r32(o + w)) * 1099511628211ull;
            }
        }
        std::fprintf(f, "  objetos %zu %016llx\n", objs.size(), (unsigned long long)h);
        if (frame == 900 && std::getenv("GD_TRACE_WORDS")) {
            for (int w = 0; w < 0x32c / 4; w++) {
                uint32_t x = 0;
                for (uint32_t o : objs) x = x * 31 + mem::r32(o + w * 4);
                std::fprintf(f, "w %03x %08x\n", w * 4, x);
            }
        }
    }
    std::fflush(f);
}

void tick() {
    if (!g_on) return;
    namecol::tick();
    streak::tick();
    if (!g_tx_pending.empty()) {
        std::vector<uint32_t> objs;
        objs.swap(g_tx_pending);
        tx_open(objs);
    }
    colors_tick();
    // Opciones: botón FPS Bypass en lugar del de Google Play
    if (uint32_t opt = find_options_layer()) {
        if (opt != g_opt_layer || mem::r32(opt) != g_opt_vt) decorate_options(opt);
    } else {
        g_opt_layer = 0;
    }
    if (g_fps_clicked) {
        g_fps_clicked = false;
        if (g_fps_cycle) g_fps_cycle();
        if (alive(g_opt_layer, g_opt_vt) && g_fps_sprite) {
            // ButtonSprite del botón: primer hijo del item
            std::vector<uint32_t> ch;
            children(g_fps_sprite, ch);
            if (!ch.empty()) call(G(0x102448), {ch[0], gstr(fps_text())});  // ButtonSprite::setString
        }
    }
    // Página de un nivel con canción de Newgrounds
    uint32_t info = find_info_layer();
    if (info && info != g_info_layer) decorate_info(info);
    if (!info) g_info_layer = 0;
    if (g_action == kInfoDownload) {
        g_action = kNone;
        if (alive(g_info_layer, g_info_vt) && g_info_id >= kFirstNG) start_download(g_info_id, true);
    }
    if (alive(g_info_layer, g_info_vt) && g_info_status) {
        static bool was_busy = false;
        bool b = busy(g_info_id);
        if (b != was_busy || !b) {
            info_status();
        }
        was_busy = b;
    }

    if (!layer_alive()) {
        g_decorated = 0;
        if (g_action != kSelect) g_action = kNone;
        return;
    }
    if (g_decorated != g_layer) decorate();
    Action a = g_action;
    g_action = kNone;
    switch (a) {
    case kOpen: popup_open(); break;
    case kSearch:
        if (popup_alive() && !g_pop.searching) {
            int id = std::atoi(g_pop.typed.c_str());
            g_pop.found = 0;
            if (id < kFirstNG) { popup_error(); break; }
            if (have_song(id)) {
                g_pop.found = id;
            } else {
                start_download(id, true);
                g_pop.searching = id;
            }
            popup_refresh();
        }
        break;
    case kSelect:  // la ventana ya se cerró sola
        if (g_pop.found) set_ng(g_pop.found);
        g_pop = Popup{};
        break;
    default: break;
    }
    if (popup_alive() && g_pop.searching && !busy(g_pop.searching)) {
        int id = g_pop.searching;
        g_pop.searching = 0;
        if (have_song(id)) {
            g_pop.found = id;
            popup_refresh();
        } else {
            popup_error();
        }
    }
}

std::string resolve(const std::string& base_path, int track) {
    if (!g_on) return base_path;
    if (track < kFirstNG) return base_path;
    std::string p = mp3_path(track);
    if (have_song(track)) return p;
    start_download(track);
    // Esperar a la descarga (la llamada viene de la carga del nivel).
    std::unique_lock<std::mutex> l(g_mu);
    g_cv.wait_for(l, std::chrono::seconds(90), [&] { return !g_busy.count(track); });
    l.unlock();
    if (!have_song(track)) g_song_failed = true;  // modo Streak: el nivel se cambia por otro
    return p;
}


// ---------------------------------------------------------------- colores de nombre
// Cada jugador elige en el garaje un color (de sus colores P1 desbloqueados) para su nombre.
// Los colores se guardan por userID en un servidor de Cloudflare (cloudflare/colores). Los userID
// de los demás salen de las respuestas del servidor del GDPS (creadores, puntuaciones); el propio,
// de la respuesta de updateGJUserScore. Las etiquetas con un nombre conocido se colorean al escribirse.
namespace namecol {

std::mutex mu;
std::map<std::string, int> name_id;   // nombre -> userID
std::map<int, int> colors;            // userID -> índice de color (-1: ninguno)
std::set<int> pending, asked;         // userIDs por consultar / ya consultados
int my_id = 0;
std::string key;                      // clave secreta de este PC para cambiar el color propio
std::atomic<bool> busy{false};

std::string data_dir() { return std::filesystem::path(g_dir).parent_path().string(); }
std::string server() {
    if (const char* e = std::getenv("GD_COLOR_SERVER")) return e;
    std::ifstream f(data_dir() + "/colores_url.txt");
    std::string u;
    std::getline(f, u);
    while (!u.empty() && (u.back() == '/' || u.back() == '\r' || u.back() == ' ')) u.pop_back();
    return u.empty() ? kColorServer : u;
}
bool is_num(const std::string& t) { return !t.empty() && t.size() < 10 && std::all_of(t.begin(), t.end(), ::isdigit); }

void load_local() {
    std::ifstream f(data_dir() + "/mi_userid.txt");
    f >> my_id;
    std::ifstream k(data_dir() + "/clave_color.txt");
    std::getline(k, key);
    if (key.size() < 32) {
        std::random_device rd;
        static const char* hx = "0123456789abcdef";
        key.clear();
        for (int i = 0; i < 48; i++) key += hx[rd() & 15];
        std::ofstream(data_dir() + "/clave_color.txt") << key << "\n";
    }
}

void learn(int id, const std::string& name) {
    if (id <= 0 || name.empty()) return;
    name_id[name] = id;
    if (!asked.count(id)) pending.insert(id);
}

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> v;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(c, a);
        v.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return v;
}

// Respuestas del servidor del GDPS (hilo de red del juego).
void on_response(const std::string& url, const std::string& body) {
    std::lock_guard<std::mutex> l(mu);
    if (url.find("updateGJUserScore") != std::string::npos) {
        std::string t = body;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
        if (is_num(t) && std::stoi(t) > 0 && std::stoi(t) != my_id) {
            my_id = std::stoi(t);
            std::ofstream(data_dir() + "/mi_userid.txt") << my_id << "\n";
            logf("[nombres] mi userID: %d", my_id);
            uint32_t gm = call(G(0x14160c), {});
            const char* nm = mem::str(mem::r32(gm + 0x168));  // GameManager: nombre del jugador (std::string)
            if (nm) learn(my_id, nm);
        }
        return;
    }
    auto sections = split(body, '#');
    // Puntuaciones: "1:nombre:2:userID:..." separadas por '|'
    if (url.find("getGJScores") != std::string::npos) {
        for (auto& item : split(sections[0], '|')) {
            auto f = split(item, ':');
            std::string name;
            int id = 0;
            for (size_t i = 0; i + 1 < f.size(); i += 2) {
                if (f[i] == "1") name = f[i + 1];
                if (f[i] == "2" && is_num(f[i + 1])) id = std::stoi(f[i + 1]);
            }
            learn(id, name);
        }
    }
    // Creadores de niveles y autores de comentarios: "userID:nombre[:accountID]" separados por '|'
    for (size_t s = 1; s < sections.size(); s++)
        for (auto& item : split(sections[s], '|')) {
            auto f = split(item, ':');
            if ((f.size() == 2 || f.size() == 3) && is_num(f[0]) && !is_num(f[1])) learn(std::stoi(f[0]), f[1]);
        }
    // Comentarios: "...~3:userID~..." con el nombre en la misma entrada ("~1:nombre" en algunos servidores)
    if (url.find("Comments") != std::string::npos)
        for (auto& item : split(sections[0], '|')) {
            std::string name;
            int id = 0;
            for (auto& part : split(item, '~')) {
                auto kv = split(part, ':');
                if (kv.size() < 2) continue;
                if (kv[0] == "3" && is_num(kv[1])) id = std::stoi(kv[1]);
                if (kv[0] == "1" && !is_num(kv[1])) name = kv[1];
            }
            learn(id, name);
        }
}

// Consulta al servidor de colores los userIDs pendientes (en un hilo).
void fetch() {
    std::string srv = server();
    if (srv.empty() || busy) return;
    std::string ids;
    {
        std::lock_guard<std::mutex> l(mu);
        int n = 0;
        for (auto it = pending.begin(); it != pending.end() && n < 150; n++) {
            ids += (ids.empty() ? "" : ",") + std::to_string(*it);
            asked.insert(*it);
            it = pending.erase(it);
        }
    }
    if (ids.empty()) return;
    busy = true;
    std::thread([srv, ids] {
        std::string out;
        int st = 0;
        bool ok = http_get(srv + "/colores?ids=" + ids, out, st) && st == 200;
        std::lock_guard<std::mutex> l(mu);
        for (auto& id : split(ids, ',')) colors.emplace(std::stoi(id), -1);
        if (ok) {
            // {"12":3,"678":15}
            size_t p = 0;
            while ((p = out.find('"', p)) != std::string::npos) {
                size_t e = out.find('"', p + 1);
                if (e == std::string::npos) break;
                std::string id = out.substr(p + 1, e - p - 1);
                size_t c = out.find(':', e);
                if (c == std::string::npos) break;
                if (is_num(id)) colors[std::stoi(id)] = std::atoi(out.c_str() + c + 1);
                p = c;
            }
        } else {
            for (auto& id : split(ids, ',')) asked.erase(std::stoi(id)), pending.insert(std::stoi(id));  // reintentar
            logf("[nombres] servidor de colores sin respuesta (HTTP %d)", st);
        }
        busy = false;
    }).detach();
}

int rgb(int idx) { return int(call(G(0x140e9c), {call(G(0x14160c), {}), uint32_t(idx)}) & 0xffffff); }

// Color para el texto de una etiqueta ("Nombre" o "By Nombre"); -1 si no tiene, -2 si aún no se sabe.
int color_for(const std::string& text) {
    std::string name = text.rfind("By ", 0) == 0 ? text.substr(3) : text;
    auto it = name_id.find(name);
    if (it == name_id.end()) return -1;
    auto c = colors.find(it->second);
    if (c == colors.end()) return -2;
    return c->second;
}

std::vector<std::pair<uint32_t, std::string>> waiting;  // etiquetas a la espera de su color (retenidas)
bool in_hook = false;
const uint32_t kLabelColor = 0x145;  // color (ccColor3B) dentro de CCLabelBMFont

void paint(uint32_t lbl, int idx) {
    int c = rgb(idx);
    color(lbl, c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff);
}

void install() {
    load_local();
    static uint32_t orig = 0;
    // CCLabelBMFont::setString(const char*, bool): todas las etiquetas de texto pasan por aquí.
    orig = hook_entry(0x011c4f0c, "CCLabelBMFont::setString (colores de nombre)", [](cpu::Cpu& c) {
        uint32_t self = cpu::reg(c, 0), str = cpu::reg(c, 1), upd = cpu::reg(c, 2);
        cpu::call(orig, {self, str, upd});
        if (in_hook || !str) return;
        const char* t = mem::str(str);
        if (!t || !*t || std::strlen(t) > 24) return;
        int col;
        {
            std::lock_guard<std::mutex> l(mu);
            if (name_id.empty()) return;
            col = color_for(t);
        }
        in_hook = true;
        if (col >= 0) paint(self, col);
        if (col != -1) {
            // Se sigue mientras exista: llega el color si aún no se sabía, o se repone si el juego lo cambia.
            bool known = false;
            for (auto& w : waiting)
                if (w.first == self) w.second = t, known = true;
            if (!known) {
                call(G(0x1a1ee8), {self});  // retain
                waiting.push_back({self, t});
            }
        }
        in_hook = false;
    });
    logf("[nombres] colores de nombre activos (servidor: %s)", server().empty() ? "sin configurar" : server().c_str());
}

// ---- garaje: botón "Name" y ventana con los colores desbloqueados

uint32_t g_garage = 0, g_garage_vt = 0, g_pick = 0, g_pick_vt = 0;
std::vector<std::pair<uint32_t, int>> g_squares;  // botón -> índice de color (-1 = sin color)
int g_clicked = -100;

uint32_t find_garage() {
    const uint32_t vt = 0x01000000 + 0x41ad98 + 8;
    uint32_t d = call(G(0x1a4420), {});
    uint32_t scene = mem::r32(d + 0x70);
    if (!scene) return 0;
    std::vector<uint32_t> ch;
    children(scene, ch);
    for (uint32_t x : ch)
        if (mem::r32(x) == vt) return x;
    return 0;
}

int my_color() {
    std::lock_guard<std::mutex> l(mu);
    auto it = colors.find(my_id);
    return it == colors.end() ? -1 : it->second;
}

void set_my_color(int idx) {
    std::string srv = server();
    if (!my_id) {
        g_clicked = -100;
        uint32_t a = call(G(0x1135fc), {0, gstr("Name color"), gstr("Your user ID is not known yet. Open the leaderboards once and try again."), gstr("OK"), 0, f2u(320)});
        if (a) call(mem::r32(mem::r32(a) + 0x184), {a});
        return;
    }
    {
        std::lock_guard<std::mutex> l(mu);
        colors[my_id] = idx;
    }
    if (srv.empty()) return;
    std::string body = "{\"userID\":" + std::to_string(my_id) + ",\"color\":" + std::to_string(idx) + ",\"clave\":\"" + key + "\"}";
    std::thread([srv, body] {
        std::string out;
        int st = 0;
        http_get(srv + "/color", out, st, body);
        logf("[nombres] cambio de color: HTTP %d %s", st, out.c_str());
    }).detach();
}

void open_picker() {
    static uint32_t proto = [] {
        uint32_t vt = mem::alloc(16), obj = mem::alloc(8);
        uint32_t th = cpu::add_thunk("Color de nombre: FLAlert_Clicked", [](cpu::Cpu&) {});
        for (int i = 0; i < 4; i++) mem::w32(vt + 4 * i, th);
        mem::w32(obj, vt);
        return obj;
    }();
    uint32_t a = call(G(0x1135fc), {proto, gstr("Name color"), gstr("\n\n\n\n\n"), gstr("Close"), 0, f2u(380)});
    if (!a) return;
    g_pick = a;
    g_pick_vt = mem::r32(a);
    g_squares.clear();
    uint32_t bmenu = mem::r32(a + 0x1b0);
    float w, h;
    win_size(w, h);
    float mx = px(bmenu), my = py(bmenu);
    static uint32_t on_sq = cpu::add_thunk("Color de nombre: elegir", [](cpu::Cpu& c) {
        uint32_t item = cpu::reg(c, 1);
        for (auto& [b, i] : g_squares)
            if (b == item) g_clicked = i;
    });
    std::vector<int> opts{-1};
    uint32_t gm = call(G(0x14160c), {});
    for (int i = 0; i < 12; i++)
        if (call(G(0x141b8c), {gm, uint32_t(i), 1})) opts.push_back(i);  // isColorUnlocked(i, P1)
    int cur = my_color();
    int per_row = 7;
    for (size_t k = 0; k < opts.size(); k++) {
        int idx = opts[k];
        uint32_t spr = call(G(0x1e5d44), {gstr("playerSquare_001.png")});
        if (idx >= 0) set_color(spr, rgb(idx));
        else set_color(spr, 0x808080);
        scale(spr, idx == cur ? 1.0f : 0.8f);
        uint32_t item = call(G(0x107b84), {spr, 0, a, on_sq, 0});
        add(bmenu, item);
        float x = w / 2 + (float(k % per_row) - (std::min<int>(opts.size(), per_row) - 1) / 2.f) * 42;
        float y = h / 2 + 30 - float(k / per_row) * 42;
        pos(item, x - mx, y - my);
        g_squares.push_back({item, idx});
    }
    uint32_t hint = label("Gray = default", "goldFont.fnt");
    scale(hint, 0.5f);
    pos(hint, w / 2, h / 2 - 28);
    add(mem::r32(a + 0x1c8), hint, 5);
    call(mem::r32(mem::r32(a) + 0x184), {a});  // show
}

void decorate_garage(uint32_t gl) {
    g_garage = gl;
    g_garage_vt = mem::r32(gl);
    static uint32_t on_btn = cpu::add_thunk("Color de nombre: abrir", [](cpu::Cpu&) { g_clicked = -50; });
    float w, h;
    win_size(w, h);
    uint32_t m = new_menu();
    add(gl, m, 10);
    button(m, "Name", gl, on_btn, w - 50, h - 70, 0.6f);
}

void tick() {
    fetch();
    // etiquetas con nombre: color al llegar la respuesta, y repuesto si el juego lo cambia
    for (size_t i = 0; i < waiting.size();) {
        auto [lbl, text] = waiting[i];
        if (call(G(0x1a1eb8), {lbl}) <= 1) {  // solo la retenemos nosotros: ya no está en pantalla
            call(G(0x1a1f54), {lbl});
            waiting.erase(waiting.begin() + long(i));
            continue;
        }
        int col;
        {
            std::lock_guard<std::mutex> l(mu);
            col = color_for(text);
        }
        if (col >= -1) {  // -1: sin color (blanco, el normal de las etiquetas)
            int c = col >= 0 ? rgb(col) : 0xffffff;
            uint32_t want = uint32_t(c & 0xff) | uint32_t((c >> 8) & 0xff) << 8 | uint32_t((c >> 16) & 0xff) << 16;
            uint32_t cur = mem::r32(lbl + kLabelColor) & 0xffffff;
            if (cur != want) {
                in_hook = true;
                color(lbl, c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff);
                in_hook = false;
            }
        }
        i++;
    }
    uint32_t gl = find_garage();
    if (gl && (gl != g_garage || mem::r32(gl) != g_garage_vt)) decorate_garage(gl);
    if (!gl) g_garage = 0;
    if (g_clicked == -50) {
        g_clicked = -100;
        open_picker();
    } else if (g_clicked != -100) {
        int idx = g_clicked;
        g_clicked = -100;
        set_my_color(idx);
        if (alive(g_pick, g_pick_vt)) call(G(0x112ea0), {g_pick});  // FLAlertLayer::onBtn1 (cerrar)
    }
}

}  // namespace namecol


// ---------------------------------------------------------------- modo Streak
// Niveles al azar (Normal, Hard o Harder) que hay que superar al primer intento.
// Fase 1: botón en la pantalla de online y ventana con la mejor racha.
namespace streak {

// Fase 2: Start busca un nivel al azar (dificultad al azar entre Normal, Hard y Harder; sin Tiny)
// con la búsqueda del propio juego y abre su página; en ella se muestran la racha y "Skip".
int g_best = 0;
uint32_t g_creator = 0, g_creator_vt = 0;
int g_action = 0;  // 1: abrir ventana, 2: Start, 3: Skip

bool g_active = false;
int g_streak = 0, g_skips = 1;
std::set<int> g_played;
std::map<int, int> g_pages;  // dificultad -> páginas de resultados conocidas
std::string g_key;           // búsqueda en curso ("" = ninguna)
int g_diff = 0, g_tries = 0;
bool g_first_page = false;
Uint32 g_since = 0;
uint32_t g_info = 0, g_info_vt = 0, g_skip_btn = 0;
uint32_t g_level = 0;        // GJGameLevel abierto (retenido)
uint32_t g_play = 0;          // PlayLayer del nivel de la racha
bool g_dead = false, g_won = false, g_quitting = false;
Uint32 g_dead_at = 0;
int g_gone_frames = 0;        // fotogramas sin página del nivel ni nivel en juego
bool g_dl_failed = false;
enum Result { kNone, kWin, kLose, kLeft } g_result = kNone;
int g_choice = 0;             // botón pulsado en la ventana de resultado (1 o 2)
bool g_result_pending = false;
bool g_reroll = false;        // el nivel se cambia por otro sin contar (canción no disponible)

constexpr uint32_t kGLM = 0x166458;  // GameLevelManager::sharedState
constexpr uint32_t kSceneVt = 0x014251f0 + 8;  // vtable de cocos2d::CCScene

std::string file() { return namecol::data_dir() + "/streak.txt"; }
void load() {
    std::ifstream f(file());
    f >> g_best;
}
void submit_best();
void save_best() {
    if (g_streak > g_best) {
        g_best = g_streak;
        std::ofstream(file()) << g_best << "\n";
    }
    submit_best();
}

uint32_t find_creator() {
    const uint32_t vt = 0x01000000 + 0x417ec0 + 8;
    uint32_t d = call(G(0x1a4420), {});
    uint32_t scene = mem::r32(d + 0x70);
    if (!scene) return 0;
    std::vector<uint32_t> ch;
    children(scene, ch);
    for (uint32_t x : ch)
        if (mem::r32(x) == vt) return x;
    return 0;
}

uint32_t g_msg = 0, g_msg_vt = 0;
void show(const std::string& title, const std::string& text, const char* b1, const char* b2, uint32_t proto = 0) {
    uint32_t a = call(G(0x1135fc), {proto, gstr(title), gstr(text), gstr(b1), b2 ? gstr(b2) : 0, f2u(360)});
    if (a) call(mem::r32(mem::r32(a) + 0x184), {a});
    g_msg = a;
    g_msg_vt = a ? mem::r32(a) : 0;
}
void close_msg() {
    if (alive(g_msg, g_msg_vt)) call(G(0x112ea0), {g_msg});  // FLAlertLayer::onBtn1
    g_msg = 0;
}

// ---- Fase 5: top 10 global (servidor de Cloudflare, el mismo de los colores de nombre)
std::mutex g_top_mu;
std::string g_top = "Loading...";
std::atomic<bool> g_top_busy{false};
int g_sent = -1;  // última racha enviada al servidor

void fetch_top() {
    std::string srv = namecol::server();
    if (srv.empty() || g_top_busy) return;
    g_top_busy = true;
    std::thread([srv] {
        std::string out;
        int st = 0;
        std::string text;
        if (http_get(srv + "/top", out, st) && st == 200) {
            // [{"userID":"2","nombre":"Dos","racha":9,"color":-1},...]
            int n = 0;
            for (size_t p = 0; (p = out.find("{", p)) != std::string::npos; p++) {
                auto field = [&](const char* k) {
                    size_t a = out.find(std::string("\"") + k + "\":", p);
                    if (a == std::string::npos) return std::string();
                    a += std::strlen(k) + 3;
                    if (out[a] == '"') return out.substr(a + 1, out.find('"', a + 1) - a - 1);
                    return out.substr(a, out.find_first_of(",}", a) - a);
                };
                std::string id = field("userID"), name = field("nombre"), r = field("racha"), col = field("color");
                if (id.empty()) continue;
                ++n;  // dos por línea para que quepan los 10
                text += std::to_string(n) + ". " + ascii(name) + " - " + r + (n % 2 ? "      " : "\n");
                std::lock_guard<std::mutex> l(namecol::mu);
                namecol::learn(std::atoi(id.c_str()), name);
                namecol::colors[std::atoi(id.c_str())] = col.empty() ? -1 : std::atoi(col.c_str());
            }
            if (text.empty()) text = "No streaks yet.";
        } else {
            text = "Could not load the top 10.";
        }
        {
            std::lock_guard<std::mutex> l(g_top_mu);
            g_top = text;
        }
        g_top_busy = false;
    }).detach();
}

// Envía la mejor racha (si se conoce el userID propio y no se ha enviado ya).
void submit_best() {
    std::string srv = namecol::server();
    int id;
    {
        std::lock_guard<std::mutex> l(namecol::mu);
        id = namecol::my_id;
    }
    if (srv.empty() || !id || g_best <= 0 || g_best == g_sent) return;
    g_sent = g_best;
    const char* nm = mem::str(mem::r32(call(G(0x14160c), {}) + 0x168));
    std::string name = nm ? nm : "";
    std::string esc;
    for (char c : name)
        if (c != '"' && c != '\\' && uint8_t(c) >= 32) esc += c;
    std::string body = "{\"userID\":" + std::to_string(id) + ",\"nombre\":\"" + esc + "\",\"racha\":" + std::to_string(g_best) +
                       ",\"clave\":\"" + namecol::key + "\"}";
    std::thread([srv, body] {
        std::string out;
        int st = 0;
        http_get(srv + "/racha", out, st, body);
        logf("[streak] racha enviada: HTTP %d %s", st, out.c_str());
        fetch_top();
    }).detach();
}

void open_window() {
    static uint32_t proto = [] {
        uint32_t vt = mem::alloc(16), obj = mem::alloc(8);
        uint32_t th = cpu::add_thunk("Streak: FLAlert_Clicked", [](cpu::Cpu& c) {
            if (cpu::reg(c, 2)) g_action = 2;  // botón 2: Start
        });
        for (int i = 0; i < 4; i++) mem::w32(vt + 4 * i, th);
        mem::w32(obj, vt);
        return obj;
    }();
    std::string top;
    {
        std::lock_guard<std::mutex> l(g_top_mu);
        top = g_top;
    }
    fetch_top();  // para la próxima vez que se abra
    show("Streak", "Best streak: " + std::to_string(g_best) + "\n\nGlobal top 10:\n" + top, "Close", "Start", proto);
}

void stop(const char* why) {
    if (g_active) logf("[streak] fin (%s), racha %d", why, g_streak);
    g_active = false;
    g_key.clear();
    if (g_level) call(G(0x1a1f54), {g_level}), g_level = 0;
}

// Pide una página de niveles con la búsqueda del juego. Clave: tipo_texto_dificultad_duración_página_estrellas.
void search(bool same_diff = false) {
    static const int diffs[] = {2, 3, 4};  // Normal, Hard, Harder
    std::random_device rd;
    if (!same_diff) g_diff = diffs[rd() % 3];
    int page = 0;
    g_first_page = !g_pages.count(g_diff);
    if (!g_first_page) page = int(rd() % uint32_t(std::max(1, g_pages[g_diff])));
    g_key = "0__" + std::to_string(g_diff) + "_1,2,3_" + std::to_string(page) + "_0";
    uint32_t glm = call(G(kGLM), {});
    mem::w32(glm + 0x15c, 0);  // sin delegado: el resultado se lee de la caché del juego
    uint32_t obj = call(G(0x16bec4), {gstr(g_key)});  // GJSearchObject::createFromKey
    call(G(0x16b3f4), {glm, obj});                    // GameLevelManager::getOnlineLevels
    g_since = SDL_GetTicks();
    logf("[streak] buscando %s", g_key.c_str());
}

void start() {
    close_msg();
    g_active = true;
    g_streak = 0;
    g_skips = 1;
    g_played.clear();
    g_tries = 0;
    g_result = kNone;
    g_play = 0;
    g_info = 0;
    g_song_failed = false;
    show("Streak", "Finding a level...", "Cancel", nullptr);
    search();
}

uint32_t g_pending_open = 0;  // nivel a abrir cuando termine la transición en curso

void open_level(uint32_t level) {
    uint32_t cur = mem::r32(call(G(0x1a4420), {}) + 0x70);
    if (!cur || mem::r32(cur) != kSceneVt) {
        if (g_pending_open != level) {
            if (g_pending_open) call(G(0x1a1f54), {g_pending_open});
            call(G(0x1a1ee8), {level});
            g_pending_open = level;
        }
        return;
    }
    if (g_pending_open == level) {
        g_pending_open = 0;
        call(G(0x1a1f54), {level});
    }
    close_msg();
    if (g_level) call(G(0x1a1f54), {g_level});
    g_level = level;
    call(G(0x1a1ee8), {level});  // retain
    g_played.insert(int(mem::r32(level + 0x128)));
    g_song_failed = false;
    uint32_t scene = call(G(0x161164), {level});  // LevelInfoLayer::scene
    static uint32_t black = mem::zalloc(4);
    uint32_t tr = call(G(0x1ccd10), {f2u(0.5f), scene, black});  // CCTransitionFade::create
    call(G(0x1a3e30), {call(G(0x1a4420), {}), tr});            // replaceScene
    g_info = 0;
}

// Resultado de la búsqueda (caché del juego).
void poll_search() {
    if (g_key.empty()) return;
    if (!alive(g_msg, g_msg_vt) && g_msg) {  // "Cancel"
        stop("cancelado");
        return;
    }
    uint32_t glm = call(G(kGLM), {});
    uint32_t arr = call(G(0x167264), {glm, gstr(g_key)});  // getStoredOnlineLevels
    if (!arr) {
        if (SDL_GetTicks() - g_since > 20000) {
            stop("sin respuesta");
            close_msg();
            show("Streak", "Could not get levels from the server.", "OK", nullptr);
        }
        return;
    }
    std::string key = g_key;
    g_key.clear();
    if (g_first_page) {
        // "total:inicio:cantidad"
        const char* info = mem::str(call(G(0x166c7c), {glm, gstr(key)}));
        int total = info ? std::atoi(info) : 0;
        g_pages[g_diff] = std::max(1, (total + 9) / 10);
        logf("[streak] dificultad %d: %d niveles (%s)", g_diff, total, info ? info : "?");
        if (g_pages[g_diff] > 1) {  // ahora una página al azar
            search(true);
            return;
        }
    }
    uint32_t n = call(G(0x1a2e20), {arr});
    std::vector<uint32_t> ok;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t lv = call(G(0x1a2e44), {arr, i});
        if (lv && !g_played.count(int(mem::r32(lv + 0x128)))) ok.push_back(lv);
    }
    if (ok.empty()) {
        if (++g_tries > 8) {
            stop("sin niveles");
            close_msg();
            show("Streak", "No levels found.", "OK", nullptr);
        } else {
            search();
        }
        return;
    }
    g_tries = 0;
    std::random_device rd;
    uint32_t lv = ok[rd() % ok.size()];
    logf("[streak] nivel %d (racha %d)", int(mem::r32(lv + 0x128)), g_streak);
    open_level(lv);
}

// Página del nivel: racha actual y botón Skip.
uint32_t find_info() {
    const uint32_t vt = 0x01000000 + 0x418848 + 8;
    uint32_t d = call(G(0x1a4420), {});
    uint32_t scene = mem::r32(d + 0x70);
    if (!scene) return 0;
    std::vector<uint32_t> ch;
    children(scene, ch);
    for (uint32_t x : ch)
        if (mem::r32(x) == vt) return x;
    return 0;
}

void decorate_info(uint32_t info) {
    g_info = info;
    g_info_vt = mem::r32(info);
    float w, h;
    win_size(w, h);
    uint32_t t = label("Streak: " + std::to_string(g_streak), "goldFont.fnt");
    scale(t, 0.6f);
    pos(t, 60, 30);
    add(info, t, 20);
    static uint32_t on_skip = cpu::add_thunk("Streak: Skip", [](cpu::Cpu&) { g_action = 3; });
    uint32_t m = new_menu();
    add(info, m, 20);
    g_skip_btn = g_skips > 0 ? button(m, "Skip (" + std::to_string(g_skips) + " left)", info, on_skip, w - 70, 30, 0.6f) : 0;
}

void decorate(uint32_t cl) {
    fetch_top();
    submit_best();  // por si había una racha sin enviar (userID aún desconocido)
    g_creator = cl;
    g_creator_vt = mem::r32(cl);
    static uint32_t on_btn = cpu::add_thunk("Streak: abrir", [](cpu::Cpu&) { g_action = 1; });
    float w, h;
    win_size(w, h);
    uint32_t m = new_menu();
    add(cl, m, 10);
    button(m, "Streak", cl, on_btn, w - 52, h - 58, 0.7f);
}

// ---- Fase 3: jugar

uint32_t find_play() {
    const uint32_t vt = 0x01414f08 + 8;
    uint32_t d = call(G(0x1a4420), {});
    uint32_t scene = mem::r32(d + 0x70);
    if (!scene) return 0;
    std::vector<uint32_t> ch;
    children(scene, ch);
    for (uint32_t x : ch)
        if (mem::r32(x) == vt) return x;
    return 0;
}

uint32_t result_proto() {
    static uint32_t proto = [] {
        uint32_t vt = mem::alloc(16), obj = mem::alloc(8);
        uint32_t th = cpu::add_thunk("Streak: resultado", [](cpu::Cpu& c) { g_choice = cpu::reg(c, 2) ? 2 : 1; });
        for (int i = 0; i < 4; i++) mem::w32(vt + 4 * i, th);
        mem::w32(obj, vt);
        return obj;
    }();
    return proto;
}

void show_result() {
    close_msg();
    g_choice = 0;
    if (g_result == kWin) {
        show("Streak", "Level complete!\n\nStreak: " + std::to_string(g_streak), "Exit", "Next", result_proto());
    } else {
        save_best();
        std::string why = g_result == kLeft ? "You left the level.\n\n" : "";
        show("Streak over", why + "Final streak: " + std::to_string(g_streak) + "\nBest: " + std::to_string(g_best), "Exit", "Retry",
             result_proto());
    }
}

void install_hooks() {
    static uint32_t o_destroy = 0, o_complete = 0, o_practice = 0, o_restart = 0;
    o_destroy = hook_entry(0x011399cc, "Streak: destroyPlayer", [](cpu::Cpu& c) {
        uint32_t pl = cpu::reg(c, 0);
        cpu::call(o_destroy, {pl});
        if (g_active && pl == g_play && !g_won && !g_dead) {
            g_dead = true;
            g_dead_at = SDL_GetTicks();
            logf("[streak] muerte (racha %d)", g_streak);
        }
    });
    o_complete = hook_entry(0x0113bc08, "Streak: levelComplete", [](cpu::Cpu& c) {
        uint32_t pl = cpu::reg(c, 0);
        cpu::call(o_complete, {pl});
        // Al terminar el nivel el juego también llama a destroyPlayer justo antes: no es una muerte.
        if (g_active && pl == g_play && !g_won && (!g_dead || SDL_GetTicks() - g_dead_at < 600) && !g_quitting) {
            g_dead = false;
            g_won = true;
            g_streak++;
            save_best();
            logf("[streak] nivel superado, racha %d", g_streak);
        }
    });
    o_practice = hook_entry(0x0113d490, "Streak: togglePracticeMode", [](cpu::Cpu& c) {
        uint32_t pl = cpu::reg(c, 0), on = cpu::reg(c, 1);
        if (g_active && on && pl == g_play && !g_won) return;  // sin modo práctica en la racha
        cpu::call(o_practice, {pl, on});
    });
    o_restart = hook_entry(0x0113d44c, "Streak: resumeAndRestart", [](cpu::Cpu& c) {
        uint32_t pl = cpu::reg(c, 0);
        if (g_active && pl == g_play && !g_won && !g_dead) {  // reiniciar = segundo intento
            g_dead = true;
            g_dead_at = SDL_GetTicks() - 1000;
            call(G(0x139138), {pl});  // PlayLayer::resume (quita la pausa antes de salir)
            return;
        }
        cpu::call(o_restart, {pl});
    });
    // LevelInfoLayer::levelDownloadFailed es virtual (LevelDownloadDelegate): se cambia su entrada en la vtable.
    static uint32_t o_dlfail = 0;
    const uint32_t target = 0x0115f03c | 1;
    for (uint32_t a = 0x01418848; a < 0x01418848 + 0x600; a += 4)
        if (mem::r32(a) == target) {
            o_dlfail = target;
            mem::w32(a, cpu::add_thunk("Streak: levelDownloadFailed", [](cpu::Cpu& c) {
                if (g_active && !g_play) {
                    g_dl_failed = true;  // nivel no disponible: se busca otro sin gastar el Skip
                    return;
                }
                cpu::call(o_dlfail, {cpu::reg(c, 0), cpu::reg(c, 1)});
            }));
        }
}

void install() {
    load();
    install_hooks();
}

void tick() {
    if (g_pending_open && g_active) open_level(g_pending_open);
    uint32_t cl = find_creator();
    if (cl && (cl != g_creator || mem::r32(cl) != g_creator_vt)) decorate(cl);
    if (!cl) g_creator = 0;
    poll_search();
    if (g_active && g_key.empty()) {
        uint32_t info = find_info();
        if (info && g_result == kNone && !g_result_pending && (info != g_info || mem::r32(info) != g_info_vt)) decorate_info(info);
    }
    if (g_active && g_key.empty()) {
        uint32_t pl = find_play();
        if (pl && pl != g_play) {  // empieza el nivel
            g_play = pl;
            g_dead = g_won = g_quitting = g_reroll = false;
            close_msg();
        }
        if (g_play && pl == g_play && g_song_failed && !g_quitting && !g_won) {
            // Sin canción: el nivel no cuenta y se busca otro sin gastar el Skip.
            g_song_failed = false;
            g_reroll = g_quitting = true;
            call(G(0x1390e8), {pl});
        }
        if (g_play && pl == g_play && g_dead && !g_quitting && SDL_GetTicks() - g_dead_at > 650) {
            g_quitting = true;  // fuera antes de que el juego reinicie el nivel
            call(G(0x1390e8), {pl});  // PlayLayer::onQuit
        }
        if (g_play && !pl && g_reroll) {
            g_play = 0;
            g_info = 0;
            g_reroll = false;
            show("Streak", "The song of this level is not available.\nFinding another level...", "Cancel", nullptr);
            search();
        }
        if (g_play && !pl) {  // se dejó el nivel
            g_result = g_won ? kWin : g_dead ? kLose : kLeft;
            g_play = 0;
            g_info = 0;
            if (g_result != kWin) g_active = false;
            g_result_pending = true;  // se muestra cuando termine la transición
        }
        if (g_dl_failed) {
            g_dl_failed = false;
            show("Streak", "Level not available, finding another one...", "Cancel", nullptr);
            search();
        }
        // Salir de la página del nivel sin jugar termina la racha.
        if (!pl && !g_play && g_result == kNone) {
            bool on_info = find_info() != 0;
            // Durante una transición la escena actual no es un CCScene normal: no cuenta como salir.
            uint32_t scene = mem::r32(call(G(0x1a4420), {}) + 0x70);
            bool plain = scene && mem::r32(scene) == kSceneVt;
            g_gone_frames = (on_info || !plain) ? 0 : g_gone_frames + 1;
            if (g_info && g_gone_frames > 30) {
                g_result = kLeft;
                g_active = false;
                g_info = 0;
                g_result_pending = true;
            }
        }
    }
    if (g_result_pending) {
        uint32_t scene = mem::r32(call(G(0x1a4420), {}) + 0x70);
        if (scene && mem::r32(scene) == kSceneVt) {
            g_result_pending = false;
            show_result();
        }
    }
    if (g_choice && alive(g_msg, g_msg_vt) == false) {
        int ch = g_choice;
        g_choice = 0;
        Result r = g_result;
        g_result = kNone;
        if (r == kWin && ch == 2) { g_info = 0; show("Streak", "Finding a level...", "Cancel", nullptr); search(); }
        else if (r == kWin) { g_active = false; g_result = kLose; show_result(); g_result = kNone; }
        else if (ch == 2) start();
        else stop("salir");
    }
    int a = g_action;
    g_action = 0;
    if (a == 1) open_window();
    if (a == 2) start();
    if (a == 3 && g_active && g_skips > 0) {
        g_skips--;
        show("Streak", "Finding a level...", "Cancel", nullptr);
        search();
    }
}

}  // namespace streak

void on_http(const std::string& url, const std::string& body) {
    if (!g_on) return;
    try {
        namecol::on_response(url, body);
    } catch (...) {
        logf("[nombres] respuesta no reconocida de %s", url.c_str());
    }
}
}  // namespace songs
