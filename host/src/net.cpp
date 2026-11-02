// Sockets y DNS para el invitado (que habla "Linux ARM").
// Linux: casi todo pasa directo. Windows: Winsock con traducción de constantes, estructuras y errores.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "platform.h"
#include "thunks.h"

namespace host {

namespace {

bool trace() {
    static bool t = std::getenv("HOST_TRACE_NET") != nullptr;
    return t;
}

// Constantes de Linux (las que usa el invitado).
constexpr int L_AF_INET6 = 10, L_SOL_SOCKET = 1;
constexpr int L_SO_REUSEADDR = 2, L_SO_ERROR = 4, L_SO_BROADCAST = 6, L_SO_SNDBUF = 7, L_SO_RCVBUF = 8,
              L_SO_KEEPALIVE = 9, L_SO_LINGER = 13, L_SO_RCVTIMEO = 20, L_SO_SNDTIMEO = 21;
constexpr int L_IPV6_V6ONLY = 26, L_AI_NUMERICSERV = 0x400;

// GD_HOSTS="dominio=ip,...": redirige dominios.
// Registra la cabecera de las peticiones HTTP que manda el juego (diagnóstico de red).
static void log_http_request(const char* p, uint32_t n) {
    if (!p || n < 5 || (std::strncmp(p, "GET ", 4) != 0 && std::strncmp(p, "POST ", 5) != 0)) return;
    std::string s(p, std::min(n, 1500u));
    for (char& ch : s) if (ch == '\r') ch = ' ';
    logf("[net] petición:\n%s", s.c_str());
}

std::string redirect(const char* node) {
    std::string n = node ? node : "";
    if (const char* hosts = std::getenv("GD_HOSTS")) {
        std::string h = std::string(",") + hosts + ",";
        size_t p = h.find("," + n + "=");
        if (!n.empty() && p != std::string::npos) {
            size_t b = p + n.size() + 2;
            n = h.substr(b, h.find(',', b) - b);
        }
    }
    return n;
}

#ifdef _WIN32
SOCKET S(int fd) { return SOCKET(px::socket_of(fd)); }
int err() { return px::wsa_to_linux(WSAGetLastError()); }
int host_family(int f) { return f == L_AF_INET6 ? AF_INET6 : f; }
int guest_family(int f) { return f == AF_INET6 ? L_AF_INET6 : f; }
#else
int S(int fd) { return fd; }
int err() { return errno; }
int host_family(int f) { return f; }
int guest_family(int f) { return f; }
#endif

uint32_t ret_err(Cpu& c, long r) {
    if (r < 0) set_errno(err());
    ret(c, uint32_t(r));
    return uint32_t(r);
}

// sockaddr del invitado -> host (en Windows cambia el número de AF_INET6).
struct Addr {
    sockaddr_storage ss{};
    socklen_t len = 0;
};
Addr addr_in(uint32_t g, uint32_t len) {
    Addr a;
    a.len = socklen_t(std::min<uint32_t>(len, sizeof a.ss));
    std::memcpy(&a.ss, mem::ptr(g), size_t(a.len));
    a.ss.ss_family = uint16_t(host_family(mem::r16(g)));
    return a;
}
void addr_out(const sockaddr* sa, socklen_t len, uint32_t g, uint32_t glen_ptr) {
    if (!g || !glen_ptr) return;
    uint32_t cap = mem::r32(glen_ptr);
    uint32_t n = std::min<uint32_t>(cap, uint32_t(len));
    std::memcpy(mem::ptr(g), sa, n);
    if (n >= 2) mem::w16(g, uint16_t(guest_family(sa->sa_family)));
    mem::w32(glen_ptr, uint32_t(len));
}

// struct addrinfo de 32 bits: flags, family, socktype, protocol, addrlen, canonname*, addr*, next*
uint32_t build_addrinfo(const addrinfo* ai) {
    uint32_t head = 0, prev = 0;
    for (; ai; ai = ai->ai_next) {
        uint32_t g = mem::zalloc(32);
        mem::w32(g + 0, uint32_t(ai->ai_flags));
        mem::w32(g + 4, uint32_t(guest_family(ai->ai_family)));
        mem::w32(g + 8, uint32_t(ai->ai_socktype));
        mem::w32(g + 12, uint32_t(ai->ai_protocol));
        mem::w32(g + 16, uint32_t(ai->ai_addrlen));
        mem::w32(g + 20, ai->ai_canonname ? mem::strdup(ai->ai_canonname) : 0);
        uint32_t sa = mem::memdup(ai->ai_addr, uint32_t(ai->ai_addrlen));
        mem::w16(sa, uint16_t(guest_family(ai->ai_addr->sa_family)));
        mem::w32(g + 24, sa);
        if (prev) mem::w32(prev + 28, g); else head = g;
        prev = g;
    }
    return head;
}

// Opciones de socket Linux -> host. Devuelve false si no se conoce.
bool map_opt(int& level, int& opt) {
    if (level == L_SOL_SOCKET) {
        level = SOL_SOCKET;
        switch (opt) {
        case L_SO_REUSEADDR: opt = SO_REUSEADDR; return true;
        case L_SO_ERROR: opt = SO_ERROR; return true;
        case L_SO_BROADCAST: opt = SO_BROADCAST; return true;
        case L_SO_SNDBUF: opt = SO_SNDBUF; return true;
        case L_SO_RCVBUF: opt = SO_RCVBUF; return true;
        case L_SO_KEEPALIVE: opt = SO_KEEPALIVE; return true;
        case L_SO_LINGER: opt = SO_LINGER; return true;
        case L_SO_RCVTIMEO: opt = SO_RCVTIMEO; return true;
        case L_SO_SNDTIMEO: opt = SO_SNDTIMEO; return true;
        default: return false;
        }
    }
    if (level == IPPROTO_IPV6 && opt == L_IPV6_V6ONLY) { opt = IPV6_V6ONLY; return true; }
    return true;  // IPPROTO_TCP (TCP_NODELAY=1) y demás coinciden
}

int send_flags(uint32_t f) {
    int r = int(f & 0x3);  // MSG_OOB, MSG_PEEK
#ifndef _WIN32
    r |= MSG_NOSIGNAL;
    if (f & 0x40) r |= MSG_DONTWAIT;
#endif
    return r;
}

std::mutex g_lock;

}  // namespace

void register_net() {
    DEF("socket") {
        int fam = host_family(int(arg(c, 0)));
        int type = int(arg(c, 1)) & 0xF, proto = int(arg(c, 2));
        bool nonblock = arg(c, 1) & 04000;  // SOCK_NONBLOCK
#ifdef _WIN32
        SOCKET s = ::socket(fam, type, proto);
        int fd = s == INVALID_SOCKET ? -1 : px::socket_fd(uintptr_t(s));
        if (fd < 0) { set_errno(s == INVALID_SOCKET ? err() : px::last_error()); ret(c, uint32_t(-1)); return; }
        if (nonblock) px::fcntl(fd, 4, 04002);
#else
        (void)type;
        (void)nonblock;
        int fd = ::socket(fam, int(arg(c, 1)), proto);
        if (fd < 0) { set_errno(errno); ret(c, uint32_t(-1)); return; }
#endif
        if (trace()) logf("[net] socket(%u, %u, %u) -> %d", arg(c, 0), arg(c, 1), arg(c, 2), fd);
        ret(c, uint32_t(fd));
    });
    DEF("connect") {
        Addr a = addr_in(arg(c, 1), arg(c, 2));
        int r = ::connect(S(int(arg(c, 0))), reinterpret_cast<sockaddr*>(&a.ss), a.len);
        int e = r ? err() : 0;
#ifdef _WIN32
        if (e == 11) e = 115;  // WSAEWOULDBLOCK en connect = EINPROGRESS
#endif
        if (trace()) {
            char ip[64] = "?";
            auto* sa = reinterpret_cast<sockaddr_in*>(&a.ss);
            if (sa->sin_family == AF_INET) ::inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
            logf("[net] connect(fd=%d, %s:%d) -> %d (err %d)", int(arg(c, 0)), ip, ntohs(sa->sin_port), r, e);
        }
        if (r) set_errno(e);
        ret(c, uint32_t(r ? -1 : 0));
    });
    DEF("bind") {
        Addr a = addr_in(arg(c, 1), arg(c, 2));
        ret_err(c, ::bind(S(int(arg(c, 0))), reinterpret_cast<sockaddr*>(&a.ss), a.len));
    });
    DEF("listen") { ret_err(c, ::listen(S(int(arg(c, 0))), int(arg(c, 1)))); });
    DEF("accept") {
        sockaddr_storage ss{};
        socklen_t len = sizeof ss;
#ifdef _WIN32
        SOCKET s = ::accept(S(int(arg(c, 0))), reinterpret_cast<sockaddr*>(&ss), &len);
        if (s == INVALID_SOCKET) { ret_err(c, -1); return; }
        int fd = px::socket_fd(uintptr_t(s));
#else
        int fd = ::accept(int(arg(c, 0)), reinterpret_cast<sockaddr*>(&ss), &len);
        if (fd < 0) { ret_err(c, -1); return; }
#endif
        addr_out(reinterpret_cast<sockaddr*>(&ss), len, arg(c, 1), arg(c, 2));
        ret(c, uint32_t(fd));
    });
    DEF("shutdown") { ret_err(c, ::shutdown(S(int(arg(c, 0))), int(arg(c, 1)))); });
    DEF("send") { log_http_request(argp<char>(c, 1), arg(c, 2)); ret_err(c, ::send(S(int(arg(c, 0))), argp<char>(c, 1), int(arg(c, 2)), send_flags(arg(c, 3)))); });
    DEF("recv") { ret_err(c, ::recv(S(int(arg(c, 0))), argp<char>(c, 1), int(arg(c, 2)), int(arg(c, 3) & 0x3))); });
    DEF("sendto") {
        if (!arg(c, 4)) { log_http_request(argp<char>(c, 1), arg(c, 2)); ret_err(c, ::send(S(int(arg(c, 0))), argp<char>(c, 1), int(arg(c, 2)), send_flags(arg(c, 3)))); return; }
        Addr a = addr_in(arg(c, 4), arg(c, 5));
        ret_err(c, ::sendto(S(int(arg(c, 0))), argp<char>(c, 1), int(arg(c, 2)), send_flags(arg(c, 3)),
                            reinterpret_cast<sockaddr*>(&a.ss), a.len));
    });
    DEF("recvfrom") {
        sockaddr_storage ss{};
        socklen_t len = sizeof ss;
        long r = ::recvfrom(S(int(arg(c, 0))), argp<char>(c, 1), int(arg(c, 2)), int(arg(c, 3) & 0x3),
                            reinterpret_cast<sockaddr*>(&ss), &len);
        if (r >= 0) addr_out(reinterpret_cast<sockaddr*>(&ss), len, arg(c, 4), arg(c, 5));
        ret_err(c, r);
    });
    auto name_fn = [](Cpu& c, bool peer) {
        sockaddr_storage ss{};
        socklen_t len = sizeof ss;
        int r = peer ? ::getpeername(S(int(arg(c, 0))), reinterpret_cast<sockaddr*>(&ss), &len)
                     : ::getsockname(S(int(arg(c, 0))), reinterpret_cast<sockaddr*>(&ss), &len);
        if (!r) addr_out(reinterpret_cast<sockaddr*>(&ss), len, arg(c, 1), arg(c, 2));
        ret_err(c, r);
    };
    def("getsockname", [name_fn](Cpu& c) { name_fn(c, false); });
    def("getpeername", [name_fn](Cpu& c) { name_fn(c, true); });
    DEF("setsockopt") {
        int level = int(arg(c, 1)), opt = int(arg(c, 2));
        int glevel = level, gopt = opt;
        if (!map_opt(level, opt)) { ret(c, 0); return; }  // opción desconocida: la ignoramos
        if (glevel == L_SOL_SOCKET && (gopt == L_SO_RCVTIMEO || gopt == L_SO_SNDTIMEO)) {
            uint32_t sec = mem::r32(arg(c, 3)), usec = mem::r32(arg(c, 3) + 4);
#ifdef _WIN32
            DWORD ms = sec * 1000 + usec / 1000;
            ret_err(c, ::setsockopt(S(int(arg(c, 0))), level, opt, reinterpret_cast<const char*>(&ms), sizeof ms));
#else
            struct timeval tv{time_t(sec), suseconds_t(usec)};
            ret_err(c, ::setsockopt(int(arg(c, 0)), level, opt, &tv, sizeof tv));
#endif
            return;
        }
        ret_err(c, ::setsockopt(S(int(arg(c, 0))), level, opt, argp<char>(c, 3), socklen_t(arg(c, 4))));
    });
    DEF("getsockopt") {
        int level = int(arg(c, 1)), opt = int(arg(c, 2));
        int glevel = level, gopt = opt;
        if (!map_opt(level, opt)) { set_errno(92); ret(c, uint32_t(-1)); return; }
        socklen_t len = socklen_t(mem::r32(arg(c, 4)));
        int r = ::getsockopt(S(int(arg(c, 0))), level, opt, argp<char>(c, 3), &len);
        mem::w32(arg(c, 4), uint32_t(len));
#ifdef _WIN32
        if (!r && glevel == L_SOL_SOCKET && gopt == L_SO_ERROR) {  // código WSA -> errno Linux
            int v = int(mem::r32(arg(c, 3)));
            mem::w32(arg(c, 3), uint32_t(v ? px::wsa_to_linux(v) : 0));
        }
#endif
        if (trace()) logf("[net] getsockopt(%d, %d, %d) -> %d val=%d", int(arg(c, 0)), glevel, gopt, r, int(mem::r32(arg(c, 3))));
        ret_err(c, r);
    });
    DEF("select") {
        // fd_set del invitado: mapa de bits de 1024 descriptores (128 bytes). Lo resolvemos con poll.
        int n = int(arg(c, 0));
        uint32_t sets[3] = {arg(c, 1), arg(c, 2), arg(c, 3)};
        int timeout = -1;
        if (arg(c, 4)) timeout = int(mem::r32(arg(c, 4)) * 1000 + mem::r32(arg(c, 4) + 4) / 1000);
        std::vector<px::PollFd> pf;
        for (int fd = 0; fd < n && fd < 1024; fd++) {
            int16_t ev = 0;
            for (int k = 0; k < 3; k++)
                if (sets[k] && ((mem::r32(sets[k] + uint32_t(fd / 32) * 4) >> (fd % 32)) & 1)) ev |= k == 0 ? 1 : k == 1 ? 4 : 2;
            if (ev) pf.push_back({fd, ev, 0});
        }
        int r = px::poll(pf.data(), uint32_t(pf.size()), timeout);
        if (r < 0) { set_errno(px::last_error()); ret(c, uint32_t(-1)); return; }
        for (uint32_t s : sets)
            if (s) std::memset(mem::ptr(s), 0, 128);
        int count = 0;
        for (auto& p : pf) {
            auto set_bit = [&](uint32_t s) {
                if (!s) return;
                uint32_t w = s + uint32_t(p.fd / 32) * 4;
                mem::w32(w, mem::r32(w) | (1u << (p.fd % 32)));
                count++;
            };
            if ((p.revents & (1 | 0x10 | 8)) && (p.events & 1)) set_bit(sets[0]);
            if ((p.revents & (4 | 8)) && (p.events & 4)) set_bit(sets[1]);
            if ((p.revents & 2) && (p.events & 2)) set_bit(sets[2]);
        }
        ret(c, uint32_t(count));
    });
    DEF("ioctl") {
        uint32_t req = arg(c, 1);
        int fd = int(arg(c, 0));
        if (req == 0x5421) {  // FIONBIO
            int on = int(mem::r32(arg(c, 2)));
            int fl = px::fcntl(fd, 3, 0);
            ret(c, uint32_t(px::fcntl(fd, 4, on ? (uint32_t(fl) | 04000) : (uint32_t(fl) & ~04000u))));
            return;
        }
        if (req == 0x541B) {  // FIONREAD
#ifdef _WIN32
            u_long v = 0;
            int r = ::ioctlsocket(S(fd), FIONREAD, &v);
            mem::w32(arg(c, 2), uint32_t(v));
            ret_err(c, r ? -1 : 0);
#else
            ret_err(c, ::ioctl(fd, FIONREAD, argp<int>(c, 2)));
#endif
            return;
        }
        set_errno(25);  // ENOTTY
        ret(c, uint32_t(-1));
    });
    DEF("getaddrinfo") {
        addrinfo hints{}, *res = nullptr;
        if (uint32_t h = arg(c, 2)) {
            uint32_t f = mem::r32(h);
            hints.ai_flags = int(f & 0x7);  // PASSIVE, CANONNAME, NUMERICHOST
            if (f & L_AI_NUMERICSERV) hints.ai_flags |= AI_NUMERICSERV;
            hints.ai_family = host_family(int(mem::r32(h + 4)));
            hints.ai_socktype = int(mem::r32(h + 8));
            hints.ai_protocol = int(mem::r32(h + 12));
        }
        std::string node = redirect(args(c, 0));
        int r = ::getaddrinfo(node.empty() ? nullptr : node.c_str(), args(c, 1), arg(c, 2) ? &hints : nullptr, &res);
        logf("[net] getaddrinfo(%s) -> %d", args(c, 0) ? args(c, 0) : "null", r);
        mem::w32(arg(c, 3), r == 0 ? build_addrinfo(res) : 0);
        if (res) ::freeaddrinfo(res);
        ret(c, r == 0 ? 0 : uint32_t(-2));  // EAI_NONAME
    });
    DEF("freeaddrinfo") {
        for (uint32_t g = arg(c, 0); g;) {
            uint32_t next = mem::r32(g + 28);
            mem::free(mem::r32(g + 20));
            mem::free(mem::r32(g + 24));
            mem::free(g);
            g = next;
        }
    });
    DEF("gai_strerror") {
        static uint32_t msg = 0;
        if (!msg) msg = mem::strdup("Name or service not known");
        ret(c, msg);
    });
    DEF("getnameinfo") {
        Addr a = addr_in(arg(c, 0), arg(c, 1));
        ret(c, uint32_t(::getnameinfo(reinterpret_cast<sockaddr*>(&a.ss), a.len, argp<char>(c, 2), arg(c, 3),
                                      argp<char>(c, 4), arg(c, 5), int(arg(c, 6)))));
    });
    DEF("gethostbyname") {
        // hostent de 32 bits: name*, aliases**, addrtype, length, addr_list**
        static uint32_t he = 0, list = 0, addr = 0, aliases = 0;
        std::lock_guard<std::mutex> lk(g_lock);
        if (!he) { he = mem::zalloc(20); list = mem::zalloc(8); addr = mem::zalloc(16); aliases = mem::zalloc(4); }
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        std::string node = redirect(args(c, 0));
        if (::getaddrinfo(node.c_str(), nullptr, &hints, &res) != 0 || !res) { ret(c, 0); return; }
        std::memcpy(mem::ptr(addr), &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr, 4);
        ::freeaddrinfo(res);
        mem::w32(he, mem::strdup(args(c, 0)));
        mem::w32(he + 4, aliases);
        mem::w32(he + 8, AF_INET);
        mem::w32(he + 12, 4);
        mem::w32(he + 16, list);
        mem::w32(list, addr);
        mem::w32(list + 4, 0);
        ret(c, he);
    });
    DEF("inet_ntop") {
        const char* r = ::inet_ntop(host_family(int(arg(c, 0))), argp(c, 1), argp<char>(c, 2), arg(c, 3));
        ret(c, r ? arg(c, 2) : 0);
    });
    DEF("inet_pton") { ret(c, uint32_t(::inet_pton(host_family(int(arg(c, 0))), args(c, 1), argp(c, 2)))); });
    DEF("inet_addr") { ret(c, uint32_t(::inet_addr(args(c, 0)))); });
    DEF("if_nametoindex") { ret(c, 0); });
}

}  // namespace host
