// Capa de plataforma: lo que el juego espera de Linux/Android, implementado en Linux o Windows.
// Todas las funciones devuelven -1 en error y dejan el error en px::last_error() con
// numeración de Linux (la que entiende el juego).
#pragma once
#include <cstdint>
#include <ctime>
#include <string>

namespace px {

int last_error();
void set_error(int linux_errno);

// ---------- descriptores (archivos, sockets y tuberías comparten numeración) ----------
int open(const char* path, uint32_t linux_flags, int mode);
long read(int fd, void* buf, size_t n);
long write(int fd, const void* buf, size_t n);
int close(int fd);
int64_t lseek(int fd, int64_t off, int whence);
long pread(int fd, void* buf, size_t n, int64_t off);
long pwrite(int fd, const void* buf, size_t n, int64_t off);
int fsync(int fd);
int ftruncate(int fd, int64_t len);
int pipe(int fds[2]);
// pollfd del invitado: {int fd; short events; short revents} (igual en ARM y x86).
struct PollFd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};
int poll(PollFd* fds, uint32_t n, int timeout_ms);
// fcntl sin bloqueos (F_GETFD/F_SETFD/F_GETFL/F_SETFL, numeración de Linux).
int fcntl(int fd, int cmd, uint32_t arg);
// Bloqueos tipo flock (cmd Linux 5/6/7 = GETLK/SETLK/SETLKW). En Windows siempre se conceden.
int lock(int fd, int cmd, int16_t& type, int16_t whence, int64_t start, int64_t len, int32_t& pid);
// Archivo temporal con un contenido dado, abierto para lectura.
int temp_fd_with(const std::string& content);

// ---------- sistema de archivos ----------
struct Stat {
    uint64_t dev = 0, ino = 0, rdev = 0, blocks = 0;
    uint32_t mode = 0, nlink = 1, uid = 0, gid = 0, blksize = 4096;
    int64_t size = 0;
    int64_t atime = 0, mtime = 0, ctime = 0;
};
int stat(const char* path, Stat& st);
int lstat(const char* path, Stat& st);
int fstat(int fd, Stat& st);
int access(const char* path, int mode);
int mkdir(const char* path, int mode);
int rmdir(const char* path);
int unlink(const char* path);
int readlink(const char* path, char* buf, size_t n);
std::string getcwd();

struct DirEntry {
    std::string name;
    uint64_t ino = 0;
    uint8_t type = 0;  // DT_* de Linux: 4 dir, 8 archivo
};
void* opendir(const char* path);
bool readdir(void* dir, DirEntry& out);
void closedir(void* dir);

// ---------- proceso / tiempo ----------
uint32_t pid();
uint32_t tid();
void localtime(int64_t t, std::tm& out);
void gmtime(int64_t t, std::tm& out);
void realtime(int64_t& sec, int64_t& nsec);
void fixed_clock_start();
void fixed_clock_advance(int64_t ns);
void monotonic(int64_t& sec, int64_t& nsec);
void sleep_ns(int64_t ns);

// ---------- sockets (usado por net.cpp) ----------
#ifdef _WIN32
int socket_fd(uintptr_t sock);  // registra un SOCKET y devuelve su descriptor del invitado
uintptr_t socket_of(int fd);    // ~0 si no es un socket
void socket_forget(int fd);
int wsa_to_linux(int wsa_error);
#endif

}  // namespace px
