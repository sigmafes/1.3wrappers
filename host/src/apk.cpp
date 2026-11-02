#include "apk.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "log.h"

namespace apk {

namespace {

struct Entry {
    uint32_t local_offset;
    uint32_t comp_size;
    uint32_t size;
    uint16_t method;
};

std::FILE* g_file = nullptr;
std::map<std::string, Entry> g_entries;
std::map<std::string, std::weak_ptr<std::vector<uint8_t>>> g_cache;
std::mutex g_lock;

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

}  // namespace

bool open(const std::string& path) {
    g_file = std::fopen(path.c_str(), "rb");
    if (!g_file) return false;
    std::fseek(g_file, 0, SEEK_END);
    long size = std::ftell(g_file);
    long tail = std::min<long>(size, 65536 + 22);
    std::vector<uint8_t> buf(tail);
    std::fseek(g_file, size - tail, SEEK_SET);
    if (std::fread(buf.data(), 1, tail, g_file) != size_t(tail)) return false;
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--)
        if (rd32(&buf[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) return false;
    uint32_t cd_size = rd32(&buf[eocd + 12]), cd_off = rd32(&buf[eocd + 16]);
    std::vector<uint8_t> cd(cd_size);
    std::fseek(g_file, cd_off, SEEK_SET);
    if (std::fread(cd.data(), 1, cd_size, g_file) != cd_size) return false;
    for (size_t p = 0; p + 46 <= cd.size() && rd32(&cd[p]) == 0x02014b50;) {
        uint16_t n = rd16(&cd[p + 28]), extra = rd16(&cd[p + 30]), comment = rd16(&cd[p + 32]);
        Entry e{rd32(&cd[p + 42]), rd32(&cd[p + 20]), rd32(&cd[p + 24]), rd16(&cd[p + 10])};
        g_entries[std::string(reinterpret_cast<char*>(&cd[p + 46]), n)] = e;
        p += 46 + n + extra + comment;
    }
    logf("[apk] %s: %zu archivos", path.c_str(), g_entries.size());
    return true;
}

bool exists(const std::string& name) { return g_entries.count(name) > 0; }

std::shared_ptr<std::vector<uint8_t>> read(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (auto c = g_cache[name].lock()) return c;
    auto it = g_entries.find(name);
    if (it == g_entries.end()) return nullptr;
    const Entry& e = it->second;
    uint8_t lh[30];
    std::fseek(g_file, e.local_offset, SEEK_SET);
    if (std::fread(lh, 1, 30, g_file) != 30) return nullptr;
    std::fseek(g_file, e.local_offset + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
    std::vector<uint8_t> comp(e.comp_size);
    if (std::fread(comp.data(), 1, comp.size(), g_file) != comp.size()) return nullptr;
    auto out = std::make_shared<std::vector<uint8_t>>(e.size);
    if (e.method == 0) {
        *out = std::move(comp);
    } else {
        z_stream z{};
        inflateInit2(&z, -MAX_WBITS);
        z.next_in = comp.data();
        z.avail_in = uInt(comp.size());
        z.next_out = out->data();
        z.avail_out = uInt(out->size());
        inflate(&z, Z_FINISH);
        inflateEnd(&z);
    }
    g_cache[name] = out;
    return out;
}

}  // namespace apk
