// Audio de cocos2d-x (Cocos2dxActivity.*Music/*Effect en Java) con SDL_mixer.
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "apk.h"
#include "jni.h"
#include "log.h"
#include "memory.h"
#include "thunks.h"

namespace host {

namespace {

const std::string kAct = "org/cocos2dx/lib/Cocos2dxActivity.";
bool g_ok = false;
std::mutex g_lock;

struct Music {
    std::shared_ptr<std::vector<uint8_t>> data;
    Mix_Music* mus = nullptr;
};
std::map<std::string, Music> g_music;
std::string g_current;
float g_music_vol = 1.f, g_fx_vol = 1.f;

struct Effect {
    std::shared_ptr<std::vector<uint8_t>> data;
    Mix_Chunk* chunk = nullptr;
};
std::map<std::string, Effect> g_fx;

float as_float(uint64_t bits) { float f; uint32_t b = uint32_t(bits); std::memcpy(&f, &b, 4); return f; }
uint64_t float_bits(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }

// Ruta del juego -> datos (assets del APK o archivo del sistema).
std::shared_ptr<std::vector<uint8_t>> load(const std::string& path) {
    bool absolute = !path.empty() && (path[0] == '/' || (path.size() > 2 && path[1] == ':'));
    if (absolute) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return nullptr;
        auto v = std::make_shared<std::vector<uint8_t>>();
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) v->insert(v->end(), buf, buf + n);
        std::fclose(f);
        return v;
    }
    auto d = apk::read("assets/" + path);
    return d ? d : apk::read(path);
}

Music* music(const std::string& path) {
    auto it = g_music.find(path);
    if (it != g_music.end()) return &it->second;
    Music m;
    m.data = load(path);
    if (!m.data) { logf("[audio] no existe la música %s", path.c_str()); return nullptr; }
    m.mus = Mix_LoadMUS_RW(SDL_RWFromConstMem(m.data->data(), int(m.data->size())), 1);
    if (!m.mus) { logf("[audio] %s: %s", path.c_str(), Mix_GetError()); return nullptr; }
    return &(g_music[path] = m);
}

Effect* effect(const std::string& path) {
    auto it = g_fx.find(path);
    if (it != g_fx.end()) return &it->second;
    Effect e;
    e.data = load(path);
    if (!e.data) { logf("[audio] no existe el efecto %s", path.c_str()); return nullptr; }
    e.chunk = Mix_LoadWAV_RW(SDL_RWFromConstMem(e.data->data(), int(e.data->size())), 1);
    if (!e.chunk) { logf("[audio] %s: %s", path.c_str(), Mix_GetError()); return nullptr; }
    return &(g_fx[path] = e);
}

std::string jstr(uint64_t a) { return a ? mem::str(uint32_t(a)) : ""; }

}  // namespace

// Salida de FMOD: org.fmod.AudioDevice (init/write/close). Su audio se convierte al formato
// de SDL_mixer y se suma a la mezcla final (Mix_SetPostMix).
namespace fmod_dev {
std::mutex lock;
SDL_AudioStream* stream = nullptr;
int out_rate = 44100, out_channels = 2;
uint32_t max_buffered = 0;

void postmix(void*, Uint8* buf, int len) {
    std::lock_guard<std::mutex> lk(lock);
    if (!stream) return;
    std::vector<int16_t> tmp(size_t(len) / 2);
    int got = SDL_AudioStreamGet(stream, tmp.data(), len);
    auto* out = reinterpret_cast<int16_t*>(buf);
    for (int i = 0; i < got / 2; i++) out[i] = int16_t(std::clamp(int(out[i]) + int(tmp[i]), -32768, 32767));
    // GD_AUDIO_DUMP=archivo.raw: guarda la mezcla final (para pruebas sin altavoces).
    static std::FILE* dump = std::getenv("GD_AUDIO_DUMP") ? std::fopen(std::getenv("GD_AUDIO_DUMP"), "wb") : nullptr;
    if (dump) std::fwrite(buf, 1, size_t(len), dump);
}
}  // namespace fmod_dev

// API nativa de assets de Android (libandroid.so) sobre el APK.
namespace assets {
struct Asset {
    std::shared_ptr<std::vector<uint8_t>> data;
    uint32_t pos = 0;
};
std::mutex lock;
std::map<uint32_t, Asset> open_assets;

void register_api() {
    DEF("AAssetManager_fromJava") { ret(c, 0xA55E7); });
    DEF("AAssetManager_open") {
        std::string name = args(c, 1);
        auto d = apk::read("assets/" + name);
        if (!d) { logf("[assets] no existe %s", name.c_str()); ret(c, 0); return; }
        uint32_t h = mem::zalloc(16);
        std::lock_guard<std::mutex> lk(lock);
        open_assets[h] = Asset{d, 0};
        ret(c, h);
    });
    DEF("AAsset_read") {
        std::lock_guard<std::mutex> lk(lock);
        auto it = open_assets.find(arg(c, 0));
        if (it == open_assets.end()) { ret(c, uint32_t(-1)); return; }
        Asset& a = it->second;
        uint32_t n = std::min<uint32_t>(arg(c, 2), uint32_t(a.data->size()) - a.pos);
        std::memcpy(argp(c, 1), a.data->data() + a.pos, n);
        a.pos += n;
        ret(c, n);
    });
    auto seek = [](Cpu& c, int64_t off, int whence) {
        std::lock_guard<std::mutex> lk(lock);
        auto it = open_assets.find(arg(c, 0));
        if (it == open_assets.end()) return int64_t(-1);
        Asset& a = it->second;
        int64_t base = whence == 1 ? a.pos : whence == 2 ? int64_t(a.data->size()) : 0;
        int64_t p = std::clamp<int64_t>(base + off, 0, int64_t(a.data->size()));
        a.pos = uint32_t(p);
        return p;
    };
    def("AAsset_seek", [seek](Cpu& c) { ret(c, uint32_t(seek(c, int32_t(arg(c, 1)), int(arg(c, 2))))); });
    def("AAsset_seek64", [seek](Cpu& c) { ret64(c, uint64_t(seek(c, int64_t(arg64(c, 2)), int(arg(c, 4))))); });
    DEF("AAsset_getLength") {
        std::lock_guard<std::mutex> lk(lock);
        auto it = open_assets.find(arg(c, 0));
        ret(c, it == open_assets.end() ? 0 : uint32_t(it->second.data->size()));
    });
    DEF("AAsset_getLength64") {
        std::lock_guard<std::mutex> lk(lock);
        auto it = open_assets.find(arg(c, 0));
        ret64(c, it == open_assets.end() ? 0 : it->second.data->size());
    });
    DEF("AAsset_getRemainingLength") {
        std::lock_guard<std::mutex> lk(lock);
        auto it = open_assets.find(arg(c, 0));
        ret(c, it == open_assets.end() ? 0 : uint32_t(it->second.data->size() - it->second.pos));
    });
    DEF("AAsset_close") {
        std::lock_guard<std::mutex> lk(lock);
        open_assets.erase(arg(c, 0));
        mem::free(arg(c, 0));
    });
}
}  // namespace assets

// Pausa/reanuda la música al perder/recuperar el foco (por si el juego no la reanuda él mismo).
static bool g_music_was_playing = false;
void audio_focus(bool focused) {
    if (!g_ok) return;
    if (!focused) {
        g_music_was_playing = Mix_PlayingMusic() && !Mix_PausedMusic();
    } else if (g_music_was_playing && Mix_PausedMusic()) {
        Mix_ResumeMusic();
    }
}

void audio_init() {
    assets::register_api();
    java_method("org/fmod/FMOD.checkInit ()Z", [](uint32_t, auto&) -> uint64_t { return 1; });
    java_method("org/fmod/FMOD.supportsAAudio ()Z", [](uint32_t, auto&) -> uint64_t { return 0; });
    java_method("org/fmod/FMOD.supportsLowLatency ()Z", [](uint32_t, auto&) -> uint64_t { return 0; });
    java_method("org/fmod/FMOD.getAssetManager ()Landroid/content/res/AssetManager;", [](uint32_t, auto&) -> uint64_t { return 0xA55E7; });
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        logf("[audio] sin dispositivo de audio: %s", SDL_GetError());
    } else {
        Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG);
        g_ok = Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024) == 0;
        if (g_ok) {
            Mix_AllocateChannels(32);
            Uint16 fmt;
            Mix_QuerySpec(&fmod_dev::out_rate, &fmt, &fmod_dev::out_channels);
        }
        logf("[audio] %s", g_ok ? "SDL_mixer listo" : Mix_GetError());
    }
    auto M = [](const std::string& m, JMethod f) { java_method(kAct + m, std::move(f)); };

    java_method("org/fmod/AudioDevice.init (IIII)Z", [](uint32_t, auto& a) -> uint64_t {
        // init(canales, frecuencia, ...): convertimos al formato de la mezcla de SDL_mixer
        std::lock_guard<std::mutex> lk(fmod_dev::lock);
        if (fmod_dev::stream) SDL_FreeAudioStream(fmod_dev::stream);
        fmod_dev::stream = SDL_NewAudioStream(AUDIO_S16SYS, uint8_t(a[0]), int(a[1]), AUDIO_S16SYS,
                                              uint8_t(fmod_dev::out_channels), fmod_dev::out_rate);
        fmod_dev::max_buffered = uint32_t(fmod_dev::out_rate) * fmod_dev::out_channels * 2 / 8;  // ~125 ms
        if (g_ok) Mix_SetPostMix(fmod_dev::postmix, nullptr);
        logf("[fmod] salida de audio %u Hz, %u canales", uint32_t(a[1]), uint32_t(a[0]));
        return 1;
    });
    java_method("org/fmod/AudioDevice.write ([SI)V", [](uint32_t, auto& a) -> uint64_t {
        uint32_t arr = uint32_t(a[0]), n = uint32_t(a[1]);
        if (!g_ok) {  // sin audio: descartamos al ritmo real para no acelerar a FMOD
            SDL_Delay(Uint32(1000.0 * n / (2 * 48000)));
            return 0;
        }
        // Como AudioTrack.write: bloquea si hay demasiado audio pendiente.
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(fmod_dev::lock);
                if (!fmod_dev::stream) return 0;
                if (uint32_t(SDL_AudioStreamAvailable(fmod_dev::stream)) < fmod_dev::max_buffered) {
                    SDL_AudioStreamPut(fmod_dev::stream, mem::ptr(java_array_data(arr)), int(n * 2));
                    return 0;
                }
            }
            SDL_Delay(2);
        }
    });
    java_method("org/fmod/AudioDevice.close ()V", [](uint32_t, auto&) -> uint64_t {
        std::lock_guard<std::mutex> lk(fmod_dev::lock);
        if (fmod_dev::stream) SDL_FreeAudioStream(fmod_dev::stream);
        fmod_dev::stream = nullptr;
        return 0;
    });
    // Si no hay audio, los métodos existen igual pero no hacen nada.
    M("preloadBackgroundMusic (Ljava/lang/String;)V", [](uint32_t, auto& a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_ok) music(jstr(a[0]));
        return 0;
    });
    M("playBackgroundMusic (Ljava/lang/String;Z)V", [](uint32_t, auto& a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_lock);
        g_current = jstr(a[0]);
        logf("[audio] música: %s", g_current.c_str());
        if (!g_ok) return 0;
        if (Music* m = music(g_current)) {
            Mix_VolumeMusic(int(g_music_vol * MIX_MAX_VOLUME));
            Mix_PlayMusic(m->mus, a[1] ? -1 : 0);
        }
        return 0;
    });
    M("stopBackgroundMusic ()V", [](uint32_t, auto&) -> uint64_t { if (g_ok) Mix_HaltMusic(); return 0; });
    M("pauseBackgroundMusic ()V", [](uint32_t, auto&) -> uint64_t { if (g_ok) Mix_PauseMusic(); return 0; });
    M("resumeBackgroundMusic ()V", [](uint32_t, auto&) -> uint64_t { if (g_ok) Mix_ResumeMusic(); return 0; });
    M("rewindBackgroundMusic ()V", [](uint32_t, auto&) -> uint64_t { if (g_ok) Mix_RewindMusic(); return 0; });
    M("isBackgroundMusicPlaying ()Z", [](uint32_t, auto&) -> uint64_t { return g_ok && Mix_PlayingMusic() && !Mix_PausedMusic(); });
    M("setBackgroundMusicTime (F)V", [](uint32_t, auto& a) -> uint64_t { if (g_ok) Mix_SetMusicPosition(as_float(a[0])); return 0; });
    M("setBackgroundMusicVolume (F)V", [](uint32_t, auto& a) -> uint64_t {
        g_music_vol = as_float(a[0]);
        if (g_ok) Mix_VolumeMusic(int(g_music_vol * MIX_MAX_VOLUME));
        return 0;
    });
    M("getBackgroundMusicVolume ()F", [](uint32_t, auto&) -> uint64_t { return float_bits(g_music_vol); });

    auto play_fx = [](uint32_t, auto& a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_ok) return 0;
        Effect* e = effect(jstr(a[0]));
        if (!e) return 0;
        int ch = Mix_PlayChannel(-1, e->chunk, a[1] ? -1 : 0);
        if (ch >= 0) Mix_Volume(ch, int(g_fx_vol * MIX_MAX_VOLUME));
        return uint64_t(ch + 1);
    };
    M("playEffect (Ljava/lang/String;Z)I", play_fx);
    M("playEffect (Ljava/lang/String;ZFFF)I", play_fx);
    M("preloadEffect (Ljava/lang/String;)V", [](uint32_t, auto& a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_ok) effect(jstr(a[0]));
        return 0;
    });
    M("unloadEffect (Ljava/lang/String;)V", [](uint32_t, auto&) -> uint64_t { return 0; });
    M("stopEffect (I)V", [](uint32_t, auto& a) -> uint64_t { if (g_ok && a[0]) Mix_HaltChannel(int(a[0]) - 1); return 0; });
    M("pauseEffect (I)V", [](uint32_t, auto& a) -> uint64_t { if (g_ok && a[0]) Mix_Pause(int(a[0]) - 1); return 0; });
    M("resumeEffect (I)V", [](uint32_t, auto& a) -> uint64_t { if (g_ok && a[0]) Mix_Resume(int(a[0]) - 1); return 0; });
    M("stopAllEffects ()V", [](uint32_t, auto&) -> uint64_t { if (g_ok) Mix_HaltChannel(-1); return 0; });
    M("pauseAllEffects ()V", [](uint32_t, auto&) -> uint64_t { if (g_ok) Mix_Pause(-1); return 0; });
    M("resumeAllEffects ()V", [](uint32_t, auto&) -> uint64_t { if (g_ok) Mix_Resume(-1); return 0; });
    M("setEffectsVolume (F)V", [](uint32_t, auto& a) -> uint64_t {
        g_fx_vol = as_float(a[0]);
        if (g_ok) Mix_Volume(-1, int(g_fx_vol * MIX_MAX_VOLUME));
        return 0;
    });
    M("getEffectsVolume ()F", [](uint32_t, auto&) -> uint64_t { return float_bits(g_fx_vol); });
}

}  // namespace host
