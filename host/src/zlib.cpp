// zlib para el invitado. z_stream tiene punteros, así que mantenemos un z_stream del host
// por cada uno del invitado y copiamos los campos en cada llamada.
#include <zlib.h>

#include <map>
#include <memory>
#include <mutex>

#include "log.h"
#include "thunks.h"

namespace host {

namespace {

// z_stream de 32 bits (56 bytes).
enum : uint32_t {
    kNextIn = 0, kAvailIn = 4, kTotalIn = 8, kNextOut = 12, kAvailOut = 16, kTotalOut = 20,
    kMsg = 24, kState = 28, kDataType = 44, kAdler = 48,
};

std::mutex g_lock;
std::map<uint32_t, std::unique_ptr<z_stream>> g_streams;
std::map<uint32_t, gzFile> g_gz;

z_stream* stream(uint32_t g, bool create = false) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto& p = g_streams[g];
    if (!p && create) p = std::make_unique<z_stream>();
    return p.get();
}

void to_host(uint32_t g, z_stream* s) {
    uint32_t ni = mem::r32(g + kNextIn), no = mem::r32(g + kNextOut);
    s->next_in = ni ? mem::ptr<Bytef>(ni) : nullptr;
    s->avail_in = mem::r32(g + kAvailIn);
    s->next_out = no ? mem::ptr<Bytef>(no) : nullptr;
    s->avail_out = mem::r32(g + kAvailOut);
}

void to_guest(uint32_t g, z_stream* s) {
    mem::w32(g + kNextIn, s->next_in ? mem::addr(s->next_in) : 0);
    mem::w32(g + kAvailIn, s->avail_in);
    mem::w32(g + kTotalIn, uint32_t(s->total_in));
    mem::w32(g + kNextOut, s->next_out ? mem::addr(s->next_out) : 0);
    mem::w32(g + kAvailOut, s->avail_out);
    mem::w32(g + kTotalOut, uint32_t(s->total_out));
    mem::w32(g + kDataType, uint32_t(s->data_type));
    mem::w32(g + kAdler, uint32_t(s->adler));
    static std::map<std::string, uint32_t> msgs;
    if (s->msg) {
        std::lock_guard<std::mutex> lk(g_lock);
        auto& m = msgs[s->msg];
        if (!m) m = mem::strdup(s->msg);
        mem::w32(g + kMsg, m);
    } else {
        mem::w32(g + kMsg, 0);
    }
    mem::w32(g + kState, 1);  // distinto de 0: algunos códigos lo comprueban
}

// Ejecuta una operación sobre el stream traduciendo campos antes y después.
template <class F>
void with_stream(Cpu& c, bool create, F f) {
    uint32_t g = arg(c, 0);
    z_stream* s = stream(g, create);
    if (!s) { ret(c, uint32_t(Z_STREAM_ERROR)); return; }
    if (create) *s = {};
    to_host(g, s);
    int r = f(s);
    to_guest(g, s);
    ret(c, uint32_t(r));
}

}  // namespace

void register_zlib() {
    DEF("inflateInit_") { with_stream(c, true, [](z_stream* s) { return inflateInit(s); }); });
    DEF("inflateInit2_") { int wb = int(arg(c, 1)); with_stream(c, true, [wb](z_stream* s) { return inflateInit2(s, wb); }); });
    DEF("inflate") { int fl = int(arg(c, 1)); with_stream(c, false, [fl](z_stream* s) { return inflate(s, fl); }); });
    DEF("inflateEnd") { with_stream(c, false, [](z_stream* s) { return inflateEnd(s); }); });
    DEF("inflateReset") { with_stream(c, false, [](z_stream* s) { return inflateReset(s); }); });
    DEF("inflateSync") { with_stream(c, false, [](z_stream* s) { return inflateSync(s); }); });
    DEF("inflateCopy") {
        z_stream* src = stream(arg(c, 1));
        z_stream* dst = stream(arg(c, 0), true);
        ret(c, src ? uint32_t(inflateCopy(dst, src)) : uint32_t(Z_STREAM_ERROR));
    });
    DEF("deflateInit_") { int lv = int(arg(c, 1)); with_stream(c, true, [lv](z_stream* s) { return deflateInit(s, lv); }); });
    DEF("deflateInit2_") {
        int lv = int(arg(c, 1)), me = int(arg(c, 2)), wb = int(arg(c, 3)), ml = int(arg(c, 4)), st = int(arg(c, 5));
        with_stream(c, true, [=](z_stream* s) { return deflateInit2(s, lv, me, wb, ml, st); });
    });
    DEF("deflate") { int fl = int(arg(c, 1)); with_stream(c, false, [fl](z_stream* s) { return deflate(s, fl); }); });
    DEF("deflateEnd") { with_stream(c, false, [](z_stream* s) { return deflateEnd(s); }); });
    DEF("deflateReset") { with_stream(c, false, [](z_stream* s) { return deflateReset(s); }); });
    DEF("deflateParams") { int lv = int(arg(c, 1)), st = int(arg(c, 2)); with_stream(c, false, [=](z_stream* s) { return deflateParams(s, lv, st); }); });
    DEF("crc32") { ret(c, uint32_t(crc32(arg(c, 0), argp<Bytef>(c, 1), arg(c, 2)))); });
    DEF("adler32") { ret(c, uint32_t(adler32(arg(c, 0), argp<Bytef>(c, 1), arg(c, 2)))); });
    DEF("uncompress") {
        uLongf len = mem::r32(arg(c, 1));
        int r = uncompress(argp<Bytef>(c, 0), &len, argp<Bytef>(c, 2), arg(c, 3));
        mem::w32(arg(c, 1), uint32_t(len));
        ret(c, uint32_t(r));
    });
    DEF("compress") {
        uLongf len = mem::r32(arg(c, 1));
        int r = compress(argp<Bytef>(c, 0), &len, argp<Bytef>(c, 2), arg(c, 3));
        mem::w32(arg(c, 1), uint32_t(len));
        ret(c, uint32_t(r));
    });
    DEF("compressBound") { ret(c, uint32_t(compressBound(arg(c, 0)))); });
    DEF("gzopen") {
        gzFile f = gzopen(args(c, 0), args(c, 1));
        if (!f) { ret(c, 0); return; }
        uint32_t h = mem::zalloc(16);
        std::lock_guard<std::mutex> lk(g_lock);
        g_gz[h] = f;
        ret(c, h);
    });
    DEF("gzread") {
        gzFile f;
        { std::lock_guard<std::mutex> lk(g_lock); auto it = g_gz.find(arg(c, 0)); f = it == g_gz.end() ? nullptr : it->second; }
        ret(c, f ? uint32_t(gzread(f, argp(c, 1), arg(c, 2))) : uint32_t(-1));
    });
    DEF("gzclose") {
        gzFile f = nullptr;
        { std::lock_guard<std::mutex> lk(g_lock); auto it = g_gz.find(arg(c, 0)); if (it != g_gz.end()) { f = it->second; g_gz.erase(it); } }
        ret(c, f ? uint32_t(gzclose(f)) : uint32_t(-1));
    });
}

}  // namespace host
