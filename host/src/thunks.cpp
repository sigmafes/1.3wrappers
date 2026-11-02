#include "thunks.h"

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "log.h"

namespace host {

namespace {
std::mutex g_lock;
std::unordered_map<std::string, uint32_t> g_syms;
std::unordered_map<std::string, uint32_t> g_stubs;
}  // namespace

void def(const std::string& name, cpu::Handler h) {
    uint32_t a = cpu::add_thunk(name, std::move(h));
    std::lock_guard<std::mutex> lk(g_lock);
    g_syms[name] = a;
}

void def_data(const std::string& name, uint32_t guest_addr) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_syms[name] = guest_addr;
}

uint32_t lookup(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_syms.find(name);
    return it == g_syms.end() ? 0 : it->second;
}

uint32_t resolve(const std::string& name, bool is_object) {
    if (uint32_t a = lookup(name)) return a;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_stubs.find(name);
        if (it != g_stubs.end()) return it->second;
    }
    uint32_t a;
    if (is_object) {
        a = mem::zalloc(256);
        logf("[host] variable sin implementar: %s (relleno con ceros)", name.c_str());
    } else {
        auto warned = std::make_shared<bool>(false);
        a = cpu::add_thunk(name, [name, warned](Cpu& c) {
            if (!*warned) {
                *warned = true;
                logf("[stub] %s() no implementada, devuelve 0 (lr=%08x)", name.c_str(), cpu::reg(c, 14));
            }
            ret64(c, 0);
        });
    }
    std::lock_guard<std::mutex> lk(g_lock);
    g_stubs[name] = a;
    return a;
}

std::string format(const char* fmt, VaArgs& va) {
    std::string out;
    if (!fmt) return "(null)";
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        const char* start = p++;
        if (*p == '%') {
            out += '%';
            p++;
            continue;
        }
        std::string spec = "%";
        while (std::strchr("-+ #0'", *p)) spec += *p++;
        if (*p == '*') { spec += std::to_string(int(va.next32())); p++; }
        else while (*p >= '0' && *p <= '9') spec += *p++;
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') { spec += std::to_string(int(va.next32())); p++; }
            else while (*p >= '0' && *p <= '9') spec += *p++;
        }
        int lng = 0;  // 0 normal, 1 l, 2 ll, -1 h, 3 z/j/t
        while (std::strchr("hlLqjzt", *p) && *p) {
            if (*p == 'l') lng = lng == 1 ? 2 : 1;
            else if (*p == 'L' || *p == 'q') lng = 2;
            else if (*p == 'h') lng = -1;
            else if (*p == 'j') lng = 2;
            else lng = 3;
            p++;
        }
        char conv = *p ? *p++ : 0;
        char buf[512];
        switch (conv) {
        case 'd': case 'i': {
            long long v = lng == 2 ? (long long)va.next64() : (long long)(int32_t)va.next32();
            std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), v);
            out += buf;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long long v = lng == 2 ? va.next64() : va.next32();
            std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), v);
            out += buf;
            break;
        }
        case 'c':
            std::snprintf(buf, sizeof buf, (spec + "c").c_str(), int(va.next32()));
            out += buf;
            break;
        case 'p':
            std::snprintf(buf, sizeof buf, "0x%x", va.next32());
            out += buf;
            break;
        case 's': {
            uint32_t a = va.next32();
            const char* s = a ? mem::str(a) : "(null)";
            if (spec == "%") out += s;
            else {
                std::string tmp(std::max<size_t>(std::strlen(s) + 64, 512), 0);
                int n = std::snprintf(tmp.data(), tmp.size(), (spec + "s").c_str(), s);
                out.append(tmp.data(), std::min<size_t>(n, tmp.size() - 1));
            }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t bits = va.next64();
            double d;
            std::memcpy(&d, &bits, 8);
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
            out += buf;
            break;
        }
        case 'n':
            va.next32();
            break;
        default:
            out.append(start, p);
            break;
        }
    }
    return out;
}

}  // namespace host
