// Funciones pequeñas y muy usadas por el juego, compiladas para ARM (Thumb-2) y cargadas DENTRO de
// la memoria del juego: así el traductor las ejecuta sin salir a C++ (cada salida cuesta).
// Las copias grandes siguen yendo al memcpy/memset del sistema (más rápido para muchos bytes).
// Se regenera con tools/compilar_fastlib.sh → src/fastlib_blob.h
typedef unsigned int size_t;
typedef unsigned int u32;
typedef unsigned char u8;

#define BIG 256
// Tabla rellenada por el host al cargar: direcciones de las versiones del sistema.
__attribute__((used, section(".text.table"))) static void* fl_host[2] = {0, 0};  // [0]=memcpy [1]=memset
#define HOST_MEMCPY ((void* (*)(void*, const void*, size_t))fl_host[0])
#define HOST_MEMSET ((void* (*)(void*, int, size_t))fl_host[1])

void* fl_memcpy(void* d, const void* s, size_t n) {
    if (n >= BIG) return HOST_MEMCPY(d, s, n);
    u8* dp = (u8*)d;
    const u8* sp = (const u8*)s;
    if ((((u32)dp | (u32)sp) & 3) == 0) {
        while (n >= 4) { *(u32*)dp = *(const u32*)sp; dp += 4; sp += 4; n -= 4; }
    }
    while (n--) *dp++ = *sp++;
    return d;
}

void* fl_memmove(void* d, const void* s, size_t n) {
    u8* dp = (u8*)d;
    const u8* sp = (const u8*)s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return fl_memcpy(d, s, n);
    while (n--) dp[n] = sp[n];
    return d;
}

void* fl_memset(void* d, int c, size_t n) {
    if (n >= BIG) return HOST_MEMSET(d, c, n);
    u8* dp = (u8*)d;
    while (n--) *dp++ = (u8)c;
    return d;
}

int fl_memcmp(const void* a, const void* b, size_t n) {
    const u8 *x = (const u8*)a, *y = (const u8*)b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

size_t fl_strlen(const char* s) {
    const char* p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int fl_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

int fl_strncmp(const char* a, const char* b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) return 0;
    }
    return 0;
}

char* fl_strchr(const char* s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char*)s;
        if (!*s) return 0;
    }
}

// Variantes del ABI de ARM
void fl_aeabi_memcpy(void* d, const void* s, size_t n) { fl_memcpy(d, s, n); }
void fl_aeabi_memmove(void* d, const void* s, size_t n) { fl_memmove(d, s, n); }
void fl_aeabi_memset(void* d, size_t n, int c) { fl_memset(d, c, n); }
void fl_aeabi_memclr(void* d, size_t n) { fl_memset(d, 0, n); }
