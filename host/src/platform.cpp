#include "platform.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace px {

namespace {
thread_local int t_error = 0;

// Flags de open() de Linux ARM.
constexpr uint32_t L_WRONLY = 01, L_RDWR = 02, L_CREAT = 0100, L_EXCL = 0200, L_TRUNC = 01000, L_APPEND = 02000,
                   L_NONBLOCK = 04000, L_DIRECTORY = 040000, L_NOFOLLOW = 0100000, L_DIRECT = 0200000;
}  // namespace

int last_error() { return t_error; }
void set_error(int e) { t_error = e; }

// ---------- sistema de archivos portable ----------
std::string getcwd() { return std::filesystem::current_path().string(); }

void* opendir(const char* path) {
    std::error_code ec;
    auto* it = new std::filesystem::directory_iterator(path, ec);
    if (ec) {
        delete it;
        set_error(ENOENT);
        return nullptr;
    }
    return it;
}
bool readdir(void* dir, DirEntry& out) {
    auto* it = static_cast<std::filesystem::directory_iterator*>(dir);
    std::error_code ec;
    if (*it == std::filesystem::directory_iterator()) return false;
    out.name = (*it)->path().filename().string();
    out.type = (*it)->is_directory(ec) ? 4 : 8;
    out.ino = std::hash<std::string>{}(out.name);
    it->increment(ec);
    return true;
}
void closedir(void* dir) { delete static_cast<std::filesystem::directory_iterator*>(dir); }

// Reloj fijo para pruebas repetibles: cada frame avanza exactamente lo mismo (GD_FIXED_DT).
int64_t g_fixed_ns = -1, g_fixed_base = 0;
void fixed_clock_start() {
    g_fixed_base = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    g_fixed_base -= g_fixed_base % 1000000000;
    g_fixed_ns = 0;
}
void fixed_clock_advance(int64_t ns) { if (g_fixed_ns >= 0) g_fixed_ns += ns; }
void realtime(int64_t& sec, int64_t& nsec) {
    auto ns = g_fixed_ns >= 0 ? g_fixed_base + g_fixed_ns
                              : std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    sec = ns / 1000000000;
    nsec = ns % 1000000000;
}
void monotonic(int64_t& sec, int64_t& nsec) {
    auto ns = g_fixed_ns >= 0 ? g_fixed_ns + 1000000000000LL
                              : std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    sec = ns / 1000000000;
    nsec = ns % 1000000000;
}
void sleep_ns(int64_t ns) { std::this_thread::sleep_for(std::chrono::nanoseconds(ns)); }

#ifndef _WIN32
// =====================================================================================
// Linux: llamadas directas (los números de errno ya son los de Linux).
// =====================================================================================
namespace {
template <class T>
T chk(T r) {
    if (r < 0) t_error = errno;
    return r;
}
int host_flags(uint32_t f) {
    int r = int(f & ~(L_DIRECTORY | L_NOFOLLOW | L_DIRECT | 0400000));
    if (f & L_DIRECTORY) r |= O_DIRECTORY;
    if (f & L_NOFOLLOW) r |= O_NOFOLLOW;
    if (f & L_DIRECT) r |= O_DIRECT;
    return r;
}
void fill(const struct stat& s, Stat& st) {
    st.dev = s.st_dev; st.ino = s.st_ino; st.rdev = s.st_rdev; st.blocks = s.st_blocks;
    st.mode = s.st_mode; st.nlink = uint32_t(s.st_nlink); st.uid = s.st_uid; st.gid = s.st_gid;
    st.blksize = uint32_t(s.st_blksize); st.size = s.st_size;
    st.atime = s.st_atim.tv_sec; st.mtime = s.st_mtim.tv_sec; st.ctime = s.st_ctim.tv_sec;
}
}  // namespace

int open(const char* p, uint32_t f, int mode) { return chk(::open(p, host_flags(f), mode)); }
long read(int fd, void* b, size_t n) { return chk(long(::read(fd, b, n))); }
long write(int fd, const void* b, size_t n) { return chk(long(::write(fd, b, n))); }
int close(int fd) { return chk(::close(fd)); }
int64_t lseek(int fd, int64_t o, int w) { return chk(int64_t(::lseek(fd, off_t(o), w))); }
long pread(int fd, void* b, size_t n, int64_t o) { return chk(long(::pread(fd, b, n, off_t(o)))); }
long pwrite(int fd, const void* b, size_t n, int64_t o) { return chk(long(::pwrite(fd, b, n, off_t(o)))); }
int fsync(int fd) { return chk(::fsync(fd)); }
int ftruncate(int fd, int64_t l) { return chk(::ftruncate(fd, off_t(l))); }
int pipe(int fds[2]) { return chk(::pipe(fds)); }
int poll(PollFd* f, uint32_t n, int t) { return chk(::poll(reinterpret_cast<struct pollfd*>(f), n, t)); }
int fcntl(int fd, int cmd, uint32_t a) { return chk(::fcntl(fd, cmd, a)); }
int lock(int fd, int cmd, int16_t& type, int16_t whence, int64_t start, int64_t len, int32_t& pid) {
    struct flock fl{};
    fl.l_type = type;
    fl.l_whence = whence;
    fl.l_start = off_t(start);
    fl.l_len = off_t(len);
    int hcmd = cmd == 5 ? F_GETLK : cmd == 6 ? F_SETLK : F_SETLKW;
    int r = chk(::fcntl(fd, hcmd, &fl));
    type = fl.l_type;
    pid = fl.l_pid;
    return r;
}
#ifndef __NR_memfd_create
#if defined(__aarch64__)
#define __NR_memfd_create 279
#elif defined(__x86_64__)
#define __NR_memfd_create 319
#endif
#endif
int temp_fd_with(const std::string& content) {
    // Archivo anónimo en memoria (Android no tiene /tmp); si no se puede, uno temporal.
    int fd = int(::syscall(__NR_memfd_create, "gdhost", 0));
    if (fd < 0) {
        std::string tmpl = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/gdhost-XXXXXX";
        fd = ::mkstemp(tmpl.data());
        if (fd < 0) return chk(fd);
        ::unlink(tmpl.c_str());
    }
    (void)!::write(fd, content.data(), content.size());
    ::lseek(fd, 0, SEEK_SET);
    return fd;
}
int stat(const char* p, Stat& st) { struct stat s; int r = chk(::stat(p, &s)); if (!r) fill(s, st); return r; }
int lstat(const char* p, Stat& st) { struct stat s; int r = chk(::lstat(p, &s)); if (!r) fill(s, st); return r; }
int fstat(int fd, Stat& st) { struct stat s; int r = chk(::fstat(fd, &s)); if (!r) fill(s, st); return r; }
int access(const char* p, int m) { return chk(::access(p, m)); }
int mkdir(const char* p, int m) { return chk(::mkdir(p, mode_t(m))); }
int rmdir(const char* p) { return chk(::rmdir(p)); }
int unlink(const char* p) { return chk(::unlink(p)); }
int readlink(const char* p, char* b, size_t n) { return chk(int(::readlink(p, b, n))); }
uint32_t pid() { return uint32_t(::getpid()); }
uint32_t tid() { return uint32_t(::gettid()); }
void localtime(int64_t t, std::tm& out) { time_t tt = time_t(t); ::localtime_r(&tt, &out); }
void gmtime(int64_t t, std::tm& out) { time_t tt = time_t(t); ::gmtime_r(&tt, &out); }

#else
// =====================================================================================
// Windows
// =====================================================================================
namespace {
constexpr int kFirstSocketFd = 600, kMaxFd = 1024;  // sockets en 600..1023 (cabe en fd_set del invitado)
std::mutex g_lock;
SOCKET g_socks[kMaxFd];
uint32_t g_flags[kMaxFd];
bool g_init = [] {
    for (auto& s : g_socks) s = INVALID_SOCKET;
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
    return true;
}();

// errno de la CRT de Windows -> Linux (los primeros coinciden).
int crt_to_linux(int e) {
    switch (e) {
    case 36: return 35;   // EDEADLK
    case 38: return 36;   // ENAMETOOLONG
    case 39: return 37;   // ENOLCK
    case 40: return 38;   // ENOSYS
    case 41: return 39;   // ENOTEMPTY
    case 42: return 84;   // EILSEQ
    default: return e;
    }
}
template <class T>
T chk(T r) {
    if (r < 0) t_error = crt_to_linux(errno);
    return r;
}
bool is_sock(int fd) { return fd >= kFirstSocketFd && fd < kMaxFd && g_socks[fd] != INVALID_SOCKET; }
long sock_err() {
    t_error = wsa_to_linux(WSAGetLastError());
    return -1;
}
int host_flags(uint32_t f) {
    int r = _O_BINARY;
    if (f & L_RDWR) r |= _O_RDWR;
    else if (f & L_WRONLY) r |= _O_WRONLY;
    else r |= _O_RDONLY;
    if (f & L_CREAT) r |= _O_CREAT;
    if (f & L_EXCL) r |= _O_EXCL;
    if (f & L_TRUNC) r |= _O_TRUNC;
    if (f & L_APPEND) r |= _O_APPEND;
    return r;
}
void fill(const struct _stat64& s, Stat& st) {
    st.dev = s.st_dev; st.ino = s.st_ino; st.rdev = s.st_rdev;
    st.mode = s.st_mode; st.nlink = s.st_nlink; st.uid = 0; st.gid = 0;
    st.size = s.st_size; st.blocks = uint64_t(s.st_size + 511) / 512;
    st.atime = s.st_atime; st.mtime = s.st_mtime; st.ctime = s.st_ctime;
}
}  // namespace

int wsa_to_linux(int w) {
    switch (w) {
    case WSAEWOULDBLOCK: return 11;      // EAGAIN
    case WSAEINPROGRESS: return 115;
    case WSAEALREADY: return 114;
    case WSAENOTSOCK: return 88;
    case WSAEDESTADDRREQ: return 89;
    case WSAEMSGSIZE: return 90;
    case WSAEPROTOTYPE: return 91;
    case WSAENOPROTOOPT: return 92;
    case WSAEPROTONOSUPPORT: return 93;
    case WSAEOPNOTSUPP: return 95;
    case WSAEAFNOSUPPORT: return 97;
    case WSAEADDRINUSE: return 98;
    case WSAEADDRNOTAVAIL: return 99;
    case WSAENETDOWN: return 100;
    case WSAENETUNREACH: return 101;
    case WSAENETRESET: return 102;
    case WSAECONNABORTED: return 103;
    case WSAECONNRESET: return 104;
    case WSAENOBUFS: return 105;
    case WSAEISCONN: return 106;
    case WSAENOTCONN: return 107;
    case WSAETIMEDOUT: return 110;
    case WSAECONNREFUSED: return 111;
    case WSAEHOSTUNREACH: return 113;
    case WSAEINTR: return 4;
    case WSAEBADF: return 9;
    case WSAEACCES: return 13;
    case WSAEFAULT: return 14;
    case WSAEINVAL: return 22;
    case WSAEMFILE: return 24;
    default: return 5;  // EIO
    }
}

int socket_fd(uintptr_t sock) {
    std::lock_guard<std::mutex> lk(g_lock);
    for (int fd = kFirstSocketFd; fd < kMaxFd; fd++) {
        if (g_socks[fd] == INVALID_SOCKET) {
            g_socks[fd] = SOCKET(sock);
            g_flags[fd] = 2;  // O_RDWR
            return fd;
        }
    }
    ::closesocket(SOCKET(sock));
    t_error = 24;  // EMFILE
    return -1;
}
uintptr_t socket_of(int fd) { return is_sock(fd) ? uintptr_t(g_socks[fd]) : ~uintptr_t(0); }
void socket_forget(int fd) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (fd >= 0 && fd < kMaxFd) g_socks[fd] = INVALID_SOCKET;
}

int open(const char* p, uint32_t f, int mode) {
    return chk(::_open(p, host_flags(f), (mode & 0200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD));
}
long read(int fd, void* b, size_t n) {
    if (is_sock(fd)) { int r = ::recv(g_socks[fd], static_cast<char*>(b), int(n), 0); return r < 0 ? sock_err() : r; }
    return chk(long(::_read(fd, b, unsigned(n))));
}
long write(int fd, const void* b, size_t n) {
    if (is_sock(fd)) { int r = ::send(g_socks[fd], static_cast<const char*>(b), int(n), 0); return r < 0 ? sock_err() : r; }
    return chk(long(::_write(fd, b, unsigned(n))));
}
int close(int fd) {
    if (is_sock(fd)) {
        SOCKET s = g_socks[fd];
        socket_forget(fd);
        return ::closesocket(s) ? int(sock_err()) : 0;
    }
    return chk(::_close(fd));
}
int64_t lseek(int fd, int64_t o, int w) { return chk(int64_t(::_lseeki64(fd, o, w))); }
long pread(int fd, void* b, size_t n, int64_t o) {
    std::lock_guard<std::mutex> lk(g_lock);
    int64_t cur = ::_lseeki64(fd, 0, SEEK_CUR);
    if (::_lseeki64(fd, o, SEEK_SET) < 0) return chk(-1L);
    long r = chk(long(::_read(fd, b, unsigned(n))));
    ::_lseeki64(fd, cur, SEEK_SET);
    return r;
}
long pwrite(int fd, const void* b, size_t n, int64_t o) {
    std::lock_guard<std::mutex> lk(g_lock);
    int64_t cur = ::_lseeki64(fd, 0, SEEK_CUR);
    if (::_lseeki64(fd, o, SEEK_SET) < 0) return chk(-1L);
    long r = chk(long(::_write(fd, b, unsigned(n))));
    ::_lseeki64(fd, cur, SEEK_SET);
    return r;
}
int fsync(int fd) { return chk(::_commit(fd)); }
int ftruncate(int fd, int64_t l) { int e = ::_chsize_s(fd, l); if (e) { t_error = crt_to_linux(e); return -1; } return 0; }

// Tubería = par de sockets TCP locales (para que poll/WSAPoll funcione con ella).
int pipe(int fds[2]) {
    SOCKET l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int len = sizeof a;
    if (::bind(l, reinterpret_cast<sockaddr*>(&a), sizeof a) || ::listen(l, 1) ||
        ::getsockname(l, reinterpret_cast<sockaddr*>(&a), &len)) {
        ::closesocket(l);
        return int(sock_err());
    }
    SOCKET w = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (::connect(w, reinterpret_cast<sockaddr*>(&a), sizeof a)) { ::closesocket(l); ::closesocket(w); return int(sock_err()); }
    SOCKET r = ::accept(l, nullptr, nullptr);
    ::closesocket(l);
    BOOL one = TRUE;
    ::setsockopt(w, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    fds[0] = socket_fd(uintptr_t(r));
    fds[1] = socket_fd(uintptr_t(w));
    return 0;
}

int poll(PollFd* f, uint32_t n, int t) {
    // Eventos Linux: POLLIN 1, POLLPRI 2, POLLOUT 4, POLLERR 8, POLLHUP 0x10, POLLNVAL 0x20
    std::vector<WSAPOLLFD> w;
    std::vector<uint32_t> idx;
    int ready = 0;
    for (uint32_t i = 0; i < n; i++) {
        f[i].revents = 0;
        if (f[i].fd < 0) continue;
        if (is_sock(f[i].fd)) {
            WSAPOLLFD p{};
            p.fd = g_socks[f[i].fd];
            if (f[i].events & 1) p.events |= POLLRDNORM;
            if (f[i].events & 4) p.events |= POLLWRNORM;
            w.push_back(p);
            idx.push_back(i);
        } else {
            f[i].revents = int16_t(f[i].events & (1 | 4));  // archivos: siempre listos
            if (f[i].revents) ready++;
        }
    }
    if (w.empty()) {
        if (!ready && t != 0) sleep_ns(int64_t(t < 0 ? 10 : t) * 1000000);
        return ready;
    }
    int r = ::WSAPoll(w.data(), ULONG(w.size()), ready ? 0 : t);
    if (r < 0) return int(sock_err());
    for (size_t k = 0; k < w.size(); k++) {
        int16_t ev = 0;
        if (w[k].revents & (POLLRDNORM | POLLRDBAND)) ev |= 1;
        if (w[k].revents & POLLWRNORM) ev |= 4;
        if (w[k].revents & POLLERR) ev |= 8;
        if (w[k].revents & POLLHUP) ev |= 0x10 | (f[idx[k]].events & 1);
        if (w[k].revents & POLLNVAL) ev |= 0x20;
        f[idx[k]].revents = ev;
        if (ev) ready++;
    }
    return ready;
}

int fcntl(int fd, int cmd, uint32_t a) {
    if (fd < 0 || fd >= kMaxFd) { t_error = 9; return -1; }
    switch (cmd) {
    case 1: case 2: return 0;  // F_GETFD / F_SETFD
    case 3: return int(g_flags[fd]);
    case 4:
        g_flags[fd] = a;
        if (is_sock(fd)) {
            u_long nb = (a & L_NONBLOCK) ? 1 : 0;
            if (::ioctlsocket(g_socks[fd], FIONBIO, &nb)) return int(sock_err());
        }
        return 0;
    default: return 0;
    }
}
int lock(int, int cmd, int16_t& type, int16_t, int64_t, int64_t, int32_t& pid) {
    if (cmd == 5) type = 2;  // F_GETLK: F_UNLCK, nadie más lo tiene
    pid = 0;
    return 0;
}
int temp_fd_with(const std::string& content) {
    char dir[MAX_PATH], path[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    GetTempFileNameA(dir, "gdh", 0, path);
    int fd = ::_open(path, _O_RDWR | _O_BINARY | _O_TRUNC | _O_TEMPORARY);
    if (fd < 0) return chk(fd);
    ::_write(fd, content.data(), unsigned(content.size()));
    ::_lseeki64(fd, 0, SEEK_SET);
    return fd;
}
int stat(const char* p, Stat& st) { struct _stat64 s; int r = chk(::_stat64(p, &s)); if (!r) fill(s, st); return r; }
int lstat(const char* p, Stat& st) { return stat(p, st); }
int fstat(int fd, Stat& st) {
    if (is_sock(fd)) { st = Stat{}; st.mode = 0140000; return 0; }
    struct _stat64 s;
    int r = chk(::_fstat64(fd, &s));
    if (!r) fill(s, st);
    return r;
}
int access(const char* p, int m) { return chk(::_access(p, m & 6)); }
int mkdir(const char* p, int) { return chk(::_mkdir(p)); }
int rmdir(const char* p) { return chk(::_rmdir(p)); }
int unlink(const char* p) { return chk(::_unlink(p)); }
int readlink(const char*, char*, size_t) { t_error = 22; return -1; }
uint32_t pid() { return uint32_t(GetCurrentProcessId()); }
uint32_t tid() { return uint32_t(GetCurrentThreadId()); }
void localtime(int64_t t, std::tm& out) { __time64_t tt = t; _localtime64_s(&out, &tt); }
void gmtime(int64_t t, std::tm& out) { __time64_t tt = t; _gmtime64_s(&out, &tt); }
#endif

}  // namespace px
