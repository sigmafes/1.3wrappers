// pthreads del invitado sobre hilos nativos.
// En bionic 32 bits mutex/cond/once/key son de 4 bytes: usamos su dirección como clave.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "log.h"
#include "thunks.h"

namespace host {

namespace {

std::mutex g_lock;
std::map<uint32_t, std::unique_ptr<std::recursive_mutex>> g_mutexes;
std::map<uint32_t, std::unique_ptr<std::condition_variable_any>> g_conds;
std::map<uint32_t, std::unique_ptr<std::shared_mutex>> g_rwlocks;
std::map<uint32_t, std::thread> g_threads;
std::atomic<uint32_t> g_next_thread{2};
std::atomic<uint32_t> g_next_key{1};
std::recursive_mutex g_once_lock;

thread_local uint32_t t_self = 1;
thread_local std::unordered_map<uint32_t, uint32_t> t_specific;

template <class T>
T& get(std::map<uint32_t, std::unique_ptr<T>>& m, uint32_t a) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto& p = m[a];
    if (!p) p = std::make_unique<T>();
    return *p;
}

}  // namespace

void register_pthread() {
    DEF("pthread_mutex_init") { std::lock_guard<std::mutex> lk(g_lock); g_mutexes[arg(c, 0)] = std::make_unique<std::recursive_mutex>(); ret(c, 0); });
    DEF("pthread_mutex_destroy") { ret(c, 0); });
    DEF("pthread_mutex_lock") { get(g_mutexes, arg(c, 0)).lock(); ret(c, 0); });
    DEF("pthread_mutex_unlock") { get(g_mutexes, arg(c, 0)).unlock(); ret(c, 0); });
    DEF("pthread_mutex_trylock") { ret(c, get(g_mutexes, arg(c, 0)).try_lock() ? 0 : 16); });
    for (const char* n : {"pthread_mutexattr_init", "pthread_mutexattr_destroy", "pthread_mutexattr_settype",
                          "pthread_attr_init", "pthread_attr_destroy", "pthread_attr_setdetachstate",
                          "pthread_attr_setstacksize", "pthread_attr_setschedparam", "pthread_condattr_init",
                          "pthread_condattr_destroy", "pthread_cond_destroy", "pthread_rwlock_destroy"})
        def(n, [](Cpu& c) { ret(c, 0); });

    DEF("pthread_cond_init") { get(g_conds, arg(c, 0)); ret(c, 0); });
    DEF("pthread_cond_signal") { get(g_conds, arg(c, 0)).notify_one(); ret(c, 0); });
    DEF("pthread_cond_broadcast") { get(g_conds, arg(c, 0)).notify_all(); ret(c, 0); });
    DEF("pthread_cond_wait") {
        auto& cv = get(g_conds, arg(c, 0));
        auto& m = get(g_mutexes, arg(c, 1));
        std::unique_lock<std::recursive_mutex> lk(m, std::adopt_lock);
        cv.wait(lk);
        lk.release();
        ret(c, 0);
    });
    DEF("pthread_cond_timedwait") {
        auto& cv = get(g_conds, arg(c, 0));
        auto& m = get(g_mutexes, arg(c, 1));
        uint32_t ts = arg(c, 2);
        auto deadline = std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds(int32_t(mem::r32(ts))) + std::chrono::nanoseconds(int32_t(mem::r32(ts + 4)))));
        std::unique_lock<std::recursive_mutex> lk(m, std::adopt_lock);
        auto st = cv.wait_until(lk, deadline);
        lk.release();
        ret(c, st == std::cv_status::timeout ? 110 : 0);  // ETIMEDOUT
    });

    DEF("pthread_rwlock_init") { get(g_rwlocks, arg(c, 0)); ret(c, 0); });
    DEF("pthread_rwlock_rdlock") { get(g_rwlocks, arg(c, 0)).lock_shared(); ret(c, 0); });
    DEF("pthread_rwlock_wrlock") { get(g_rwlocks, arg(c, 0)).lock(); ret(c, 0); });
    DEF("pthread_rwlock_unlock") {
        // No sabemos si era lectura o escritura: probamos ambos.
        auto& l = get(g_rwlocks, arg(c, 0));
        if (l.try_lock_shared()) { l.unlock_shared(); l.unlock_shared(); }
        else l.unlock();
        ret(c, 0);
    });

    DEF("pthread_key_create") { mem::w32(arg(c, 0), g_next_key++); ret(c, 0); });
    DEF("pthread_key_delete") { ret(c, 0); });
    DEF("pthread_setspecific") { t_specific[arg(c, 0)] = arg(c, 1); ret(c, 0); });
    DEF("pthread_getspecific") { auto it = t_specific.find(arg(c, 0)); ret(c, it == t_specific.end() ? 0 : it->second); });
    DEF("pthread_once") {
        std::lock_guard<std::recursive_mutex> lk(g_once_lock);
        if (mem::r32(arg(c, 0)) == 0) {
            mem::w32(arg(c, 0), 1);
            cpu::call(arg(c, 1));
        }
        ret(c, 0);
    });

    DEF("pthread_self") { ret(c, t_self); });
    DEF("pthread_equal") { ret(c, arg(c, 0) == arg(c, 1)); });
    DEF("pthread_create") {
        uint32_t id = g_next_thread++;
        uint32_t fn = arg(c, 2), a = arg(c, 3);
        if (arg(c, 0)) mem::w32(arg(c, 0), id);
        std::thread th([id, fn, a] {
            t_self = id;
            cpu::thread_init();
            cpu::call(fn, {a});
        });
        std::lock_guard<std::mutex> lk(g_lock);
        g_threads[id] = std::move(th);
        ret(c, 0);
    });
    DEF("pthread_detach") {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_threads.find(arg(c, 0));
        if (it != g_threads.end()) { it->second.detach(); g_threads.erase(it); }
        ret(c, 0);
    });
    DEF("pthread_join") {
        std::thread th;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            auto it = g_threads.find(arg(c, 0));
            if (it == g_threads.end()) { ret(c, 3); return; }
            th = std::move(it->second);
            g_threads.erase(it);
        }
        th.join();
        if (arg(c, 1)) mem::w32(arg(c, 1), 0);
        ret(c, 0);
    });
    DEF("pthread_exit") { logf("[host] pthread_exit no soportado aún"); std::_Exit(9); });
}

}  // namespace host
