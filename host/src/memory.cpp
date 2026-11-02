#include "memory.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

namespace mem {

uint8_t* base = nullptr;

namespace {

std::mutex g_lock;
uint32_t g_heap_top = kHeapBase;
uint32_t g_lib_top = kLibBase;

// Cabecera de 8 bytes antes de cada bloque.
struct Hdr {
    uint32_t size;   // tamaño útil del bloque
    uint32_t start;  // dirección real del bloque (para memalign)
};

// Clases de tamaño pequeñas: listas libres por clase.
constexpr uint32_t kClasses[] = {16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048};
constexpr int kNumClasses = sizeof(kClasses) / sizeof(kClasses[0]);
std::vector<uint32_t> g_free_small[kNumClasses];
std::multimap<uint32_t, uint32_t> g_free_large;  // tamaño total -> inicio

int class_for(uint32_t size) {
    for (int i = 0; i < kNumClasses; i++)
        if (size <= kClasses[i]) return i;
    return -1;
}

uint32_t g_heap_committed = kHeapBase;

uint32_t bump(uint32_t total, uint32_t align) {
    uint32_t a = (g_heap_top + align - 1) & ~(align - 1);
    if (uint64_t(a) + total > kHeapEnd) {
        std::fprintf(stderr, "[mem] heap del invitado agotado\n");
        std::abort();
    }
    g_heap_top = a + total;
    if (g_heap_top > g_heap_committed) {  // en Windows confirmamos en bloques de 16 MB
        uint32_t to = (g_heap_top + 0xFFFFFF) & ~0xFFFFFFu;
        commit(g_heap_committed, to - g_heap_committed);
        g_heap_committed = to;
    }
    return a;
}

// Devuelve un bloque crudo de "total" bytes (incluye cabecera).
uint32_t raw_alloc(uint32_t total) {
    int c = class_for(total);
    if (c >= 0) {
        auto& fl = g_free_small[c];
        if (!fl.empty()) {
            uint32_t a = fl.back();
            fl.pop_back();
            return a;
        }
        return bump(kClasses[c], 8);
    }
    uint32_t rounded = (total + 4095) & ~4095u;
    auto it = g_free_large.lower_bound(rounded);
    if (it != g_free_large.end() && it->first <= rounded * 2) {
        uint32_t a = it->second;
        g_free_large.erase(it);
        return a;
    }
    return bump(rounded, 4096);
}

uint32_t raw_capacity(uint32_t total) {
    int c = class_for(total);
    return c >= 0 ? kClasses[c] : ((total + 4095) & ~4095u);
}

}  // namespace

void commit(uint32_t a, uint32_t len) {
#ifdef _WIN32
    if (!len) return;
    uint64_t start = a & ~0xFFFull, end = (uint64_t(a) + len + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc(base + start, size_t(end - start), MEM_COMMIT, PAGE_READWRITE)) {
        std::fprintf(stderr, "[mem] no se pudo confirmar %08x+%x (error %lu)\n", a, len, GetLastError());
        std::abort();
    }
#else
    (void)a;
    (void)len;
#endif
}

void init() {
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, size_t(1) << 32, MEM_RESERVE, PAGE_NOACCESS);
    if (!p) {
        std::fprintf(stderr, "[mem] no se pudieron reservar 4 GB (error %lu)\n", GetLastError());
        std::abort();
    }
    base = static_cast<uint8_t*>(p);
    commit(kThunkBase, 0x100000);   // trampolines
    commit(0xFFFF0000, 0x10000);     // ayudas del kernel ARM
#else
    void* p = mmap(nullptr, 1ull << 32, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        std::perror("mmap 4GB");
        std::abort();
    }
    base = static_cast<uint8_t*>(p);
    mprotect(base, kNullGuard, PROT_NONE);
#endif
}

uint32_t alloc(uint32_t size, uint32_t align) {
    if (size == 0) size = 1;
    if (align < 8) align = 8;
    std::lock_guard<std::mutex> lk(g_lock);
    uint32_t total = size + sizeof(Hdr) + (align > 8 ? align : 0);
    uint32_t start = raw_alloc(total);
    uint32_t user = (start + sizeof(Hdr) + align - 1) & ~(align - 1);
    Hdr h{raw_capacity(total) - (user - start), start};
    // guardamos también la capacidad total en el inicio para poder liberar
    std::memcpy(base + user - sizeof(Hdr), &h, sizeof(h));
    return user;
}

uint32_t zalloc(uint32_t size) {
    uint32_t a = alloc(size);
    std::memset(base + a, 0, size);
    return a;
}

void free(uint32_t a) {
    if (!a) return;
    std::lock_guard<std::mutex> lk(g_lock);
    Hdr h;
    std::memcpy(&h, base + a - sizeof(Hdr), sizeof(h));
    uint32_t total = h.size + (a - h.start);
    int c = class_for(total);
    if (c >= 0 && kClasses[c] == total)
        g_free_small[c].push_back(h.start);
    else
        g_free_large.emplace(total, h.start);
}

uint32_t usable_size(uint32_t a) {
    if (!a) return 0;
    Hdr h;
    std::memcpy(&h, base + a - sizeof(Hdr), sizeof(h));
    return h.size;
}

uint32_t realloc(uint32_t a, uint32_t size) {
    if (!a) return alloc(size);
    if (size == 0) {
        free(a);
        return 0;
    }
    uint32_t old = usable_size(a);
    if (size <= old) return a;
    uint32_t n = alloc(size);
    std::memcpy(base + n, base + a, old);
    free(a);
    return n;
}

uint32_t strdup(const char* s) { return memdup(s, std::strlen(s) + 1); }

uint32_t memdup(const void* p, uint32_t n) {
    uint32_t a = alloc(n);
    std::memcpy(base + a, p, n);
    return a;
}

std::vector<std::pair<uint32_t, uint32_t>> g_claimed;  // rangos usados (librerías y mmap fijos)

bool lib_range_free(uint32_t a, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (a < g_lib_top) return false;
    for (auto& [s, l] : g_claimed)
        if (a < s + l && s < a + len) return false;
    return uint64_t(a) + len <= kHeapBase;
}

void claim_lib_range(uint32_t a, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_claimed.emplace_back(a, len);
    commit(a, len);
}

uint32_t reserve_lib(uint32_t size) {
    std::lock_guard<std::mutex> lk(g_lock);
    uint32_t a = g_lib_top;
    for (bool moved = true; moved;) {  // saltar rangos ya ocupados por mmap fijos
        moved = false;
        for (auto& [s, l] : g_claimed)
            if (a < s + l && s < a + size) { a = (s + l + 0xFFFF) & ~0xFFFFu; moved = true; }
    }
    g_lib_top = (a + size + 0xFFFF) & ~0xFFFFu;
    commit(a, size);
    if (g_lib_top > kHeapBase) {
        std::fprintf(stderr, "[mem] no hay espacio para librerías\n");
        std::abort();
    }
    return a;
}

}  // namespace mem
