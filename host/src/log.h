#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#ifdef __ANDROID__
#include <android/log.h>
// En Android el registro también se escribe en Download/13gdps-log.txt (accesible sin root).
inline std::FILE*& android_log_file() {
    static std::FILE* f = [] {
        const char* p = std::getenv("GD_LOGFILE");
        std::FILE* r = p ? std::fopen(p, "w") : nullptr;
        return r ? r : std::fopen("/storage/emulated/0/Download/13gdps-log.txt", "w");
    }();
    return f;
}
#endif

inline void logf(const char* fmt, ...) {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    va_list ap;
#ifdef __ANDROID__
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "13GDPS", fmt, ap);
    va_end(ap);
    if (std::FILE* lf = android_log_file()) {
        va_start(ap, fmt);
        std::vfprintf(lf, fmt, ap);
        va_end(ap);
        std::fputc('\n', lf);
        std::fflush(lf);
    }
#endif
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
