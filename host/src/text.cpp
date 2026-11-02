// Texto TTF: reemplaza Cocos2dxBitmap.createTextBitmap (Java) con stb_truetype.
// Dibuja el texto en un bitmap RGBA y lo entrega al juego con nativeInitBitmapDC.
#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "apk.h"
#include "cpu.h"
#include "jni.h"
#include "log.h"
#include "memory.h"

namespace host {

uint64_t call_java_native(const std::string& cls, const std::string& method, const std::vector<uint32_t>& args);

namespace {

std::mutex g_lock;
struct Font {
    std::shared_ptr<std::vector<uint8_t>> data;
    stbtt_fontinfo info{};
};
std::map<std::string, std::unique_ptr<Font>> g_fonts;

const char* kDefaultFont = "assets/ascella/fonts/Kazesawa/Kazesawa-Regular.ttf";

Font* load_font(const std::string& name) {
    auto it = g_fonts.find(name);
    if (it != g_fonts.end()) return it->second.get();
    auto f = std::make_unique<Font>();
    // Fuentes .ttf del APK; los nombres del sistema (Helvetica, Arial...) usan la fuente por defecto.
    if (name.size() > 4 && (name.ends_with(".ttf") || name.ends_with(".otf"))) {
        f->data = apk::read("assets/" + name);
        if (!f->data) f->data = apk::read(name);
    }
    if (!f->data) f->data = apk::read(kDefaultFont);
    if (!f->data || !stbtt_InitFont(&f->info, f->data->data(), stbtt_GetFontOffsetForIndex(f->data->data(), 0))) {
        logf("[text] no se pudo cargar la fuente %s", name.c_str());
        return nullptr;
    }
    return (g_fonts[name] = std::move(f)).get();
}

// UTF-8 -> códigos Unicode.
std::vector<uint32_t> decode_utf8(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = uint8_t(s[i]);
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (uint8_t(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += size_t(n);
    }
    return out;
}

struct Metrics {
    const stbtt_fontinfo* f;
    float scale;
    int advance(uint32_t cp) const {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(f, int(cp), &adv, &lsb);
        return int(std::lround(adv * scale));
    }
    int width(const std::vector<uint32_t>& line) const {
        int w = 0;
        for (uint32_t cp : line) w += advance(cp);
        return w;
    }
};

// Divide en líneas por '\n' y, si hay ancho máximo, por palabras.
std::vector<std::vector<uint32_t>> layout(const Metrics& m, const std::vector<uint32_t>& text, int max_w) {
    std::vector<std::vector<uint32_t>> lines(1);
    std::vector<uint32_t> word;
    auto flush_word = [&] {
        auto& cur = lines.back();
        if (max_w > 0 && !cur.empty() && m.width(cur) + m.width(word) > max_w) {
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            lines.emplace_back();
            while (!word.empty() && word.front() == ' ') word.erase(word.begin());
        }
        lines.back().insert(lines.back().end(), word.begin(), word.end());
        word.clear();
    };
    for (uint32_t cp : text) {
        if (cp == '\n') { flush_word(); lines.emplace_back(); continue; }
        word.push_back(cp);
        if (cp == ' ') flush_word();
    }
    flush_word();
    return lines;
}

void create_text_bitmap(const std::string& text, const std::string& font, int size, int align, int width, int height) {
    std::lock_guard<std::mutex> lk(g_lock);
    Font* f = load_font(font);
    if (!f || size <= 0) return;
    Metrics m{&f->info, stbtt_ScaleForMappingEmToPixels(&f->info, float(size))};
    int a, d, gap;
    stbtt_GetFontVMetrics(&f->info, &a, &d, &gap);
    int ascent = int(std::ceil(a * m.scale)), descent = int(std::ceil(-d * m.scale));
    int line_h = ascent + descent;

    auto lines = layout(m, decode_utf8(text), width);
    int content_w = 0;
    for (auto& l : lines) content_w = std::max(content_w, m.width(l));
    int content_h = line_h * int(lines.size());
    int W = std::max(width ? width : content_w, 1);
    int H = std::max(height ? height : content_h, 1);

    int halign = align & 0x0F, valign = (align >> 4) & 0x0F;  // 1 izq/arriba, 2 der/abajo, 3 centro
    int y0 = valign == 2 ? H - content_h : valign == 3 ? (H - content_h) / 2 : 0;

    uint32_t arr = new_java_array(uint32_t(W * H * 4), 1);
    uint8_t* px = mem::ptr<uint8_t>(java_array_data(arr));
    std::vector<uint8_t> glyph;
    for (size_t li = 0; li < lines.size(); li++) {
        int lw = m.width(lines[li]);
        int x = halign == 2 ? W - lw : halign == 3 ? (W - lw) / 2 : 0;
        int baseline = y0 + int(li) * line_h + ascent;
        for (uint32_t cp : lines[li]) {
            int x0, y0g, x1, y1;
            stbtt_GetCodepointBitmapBox(&f->info, int(cp), m.scale, m.scale, &x0, &y0g, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0g;
            if (gw > 0 && gh > 0) {
                glyph.assign(size_t(gw) * gh, 0);
                stbtt_MakeCodepointBitmap(&f->info, glyph.data(), gw, gh, gw, m.scale, m.scale, int(cp));
                for (int r = 0; r < gh; r++) {
                    int py = baseline + y0g + r;
                    if (py < 0 || py >= H) continue;
                    for (int col = 0; col < gw; col++) {
                        int pxx = x + x0 + col;
                        if (pxx < 0 || pxx >= W) continue;
                        uint8_t av = glyph[size_t(r) * gw + col];
                        uint8_t* p = px + (size_t(py) * W + pxx) * 4;
                        uint8_t v = std::max(p[3], av);  // blanco premultiplicado
                        p[0] = p[1] = p[2] = p[3] = v;
                    }
                }
            }
            x += m.advance(cp);
        }
    }
    call_java_native("org/cocos2dx/lib/Cocos2dxBitmap", "nativeInitBitmapDC", {jni_env(), 0, uint32_t(W), uint32_t(H), arr});
    mem::free(arr);
}

}  // namespace

void text_init() {
    java_method("org/cocos2dx/lib/Cocos2dxBitmap.createTextBitmap (Ljava/lang/String;Ljava/lang/String;IIII)V",
                [](uint32_t, auto& a) -> uint64_t {
                    std::string text = a[0] ? mem::str(uint32_t(a[0])) : "";
                    std::string font = a[1] ? mem::str(uint32_t(a[1])) : "";
                    create_text_bitmap(text, font, int(a[2]), int(a[3]), int(a[4]), int(a[5]));
                    return 0;
                });
}

}  // namespace host
