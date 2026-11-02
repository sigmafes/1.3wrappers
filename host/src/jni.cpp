// JNI falso: el juego cree que habla con la máquina virtual de Java.
// En la fase 1 solo registramos lo que pide; más adelante lo implementaremos en C++.
#include "jni.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <ctime>
#include <sstream>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "log.h"
#include "thunks.h"

#include "jni_names.inc"

namespace host {

namespace {

std::mutex g_lock;
uint32_t g_vm = 0, g_env = 0;
uint32_t g_next_handle = 0x100;
std::map<uint32_t, std::string> g_classes;  // handle -> nombre
std::map<uint32_t, std::string> g_methods;  // id -> "Clase.metodo firma"
std::map<std::string, uint32_t> g_natives;  // "Clase.metodo" -> función del invitado

uint32_t new_handle() { std::lock_guard<std::mutex> lk(g_lock); return g_next_handle += 4; }

// Objetos Java emulados (los jstring son char* del invitado; el resto son handles).
struct JObj {
    std::string cls;
    std::string path;  // java/io/File
};
std::map<uint32_t, JObj> g_objs;

uint32_t new_obj(const std::string& cls, const std::string& path = "") {
    uint32_t h = new_handle();
    std::lock_guard<std::mutex> lk(g_lock);
    g_objs[h] = JObj{cls, path};
    return h;
}
JObj* obj(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_objs.find(h);
    return it == g_objs.end() ? nullptr : &it->second;
}

std::string data_dir() {
    const char* d = std::getenv("GD_DATA");
    return d ? d : "data";
}

// Identificador único de esta instalación (como el ANDROID_ID del móvil). El servidor asocia a él
// la cuenta del jugador y sus niveles; se genera la primera vez y se guarda en data/user_id.txt.
static std::string install_id() {
    static std::string id;
    if (!id.empty()) return id;
    std::string path = data_dir() + "/user_id.txt";
    std::ifstream in(path);
    std::getline(in, id);
    if (id.size() == 36) return id;
    std::random_device rd;
    std::mt19937_64 rng((uint64_t(rd()) << 32) ^ rd() ^ uint64_t(std::time(nullptr)));
    char buf[40];
    uint64_t a = rng(), b = rng();
    std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx", uint32_t(a >> 32), uint32_t(a >> 16) & 0xffff,
                  uint32_t(a) & 0xffff, uint32_t(b >> 48), (unsigned long long)(b & 0xffffffffffffULL));
    id = buf;
    std::filesystem::create_directories(data_dir());
    std::ofstream(path) << id << "\n";
    logf("[jni] nuevo identificador de jugador: %s", id.c_str());
    return id;
}

// Las versiones anteriores daban a todos el mismo identificador ("gdhost-player") y el juego lo
// guarda en CCGameManager.dat; se cambia por el de esta instalación.
static void fix_saved_player_id() {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(data_dir(), ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->path().filename() != "CCGameManager.dat") continue;
        std::ifstream in(it->path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        in.close();
        std::string d = ss.str();
        const std::string old = "<string>gdhost-player</string>";
        size_t k = d.find(old);
        if (k == std::string::npos) continue;
        d.replace(k, old.size(), "<string>" + install_id() + "</string>");
        std::ofstream(it->path(), std::ios::binary) << d;
        logf("[jni] identificador de jugador actualizado en %s", it->path().string().c_str());
    }
}

// Implementaciones de métodos Java: "clase.método firma" -> función.
std::map<std::string, JMethod> g_java;

void register_java() {
    fix_saved_player_id();
    g_java["android/os/Environment.getExternalStorageDirectory ()Ljava/io/File;"] = [](uint32_t, auto&) {
        // Ruta de Android: el host la traduce a data/sdcard en cada acceso a archivos.
        std::filesystem::create_directories(data_dir() + "/sdcard");
        return uint64_t(new_obj("java/io/File", "/storage/emulated/0"));
    };
    auto file_path = [](uint32_t self, auto&) -> uint64_t {
        JObj* o = obj(self);
        return mem::strdup(o ? o->path.c_str() : "");
    };
    g_java["java/io/File.getPath ()Ljava/lang/String;"] = file_path;
    g_java["java/io/File.getAbsolutePath ()Ljava/lang/String;"] = file_path;
    g_java["java/io/File.toString ()Ljava/lang/String;"] = file_path;

    // Métodos de la actividad que el juego consulta.
    auto str = [](const char* v) { return [v](uint32_t, auto&) -> uint64_t { return mem::strdup(v); }; };
    auto yes = [](uint32_t, auto&) -> uint64_t { return 1; };
    g_java["org/cocos2dx/lib/Cocos2dxActivity.getCocos2dxPackageName ()Ljava/lang/String;"] = str("app.onepointthree.starry_sky");
    g_java["org/cocos2dx/lib/Cocos2dxActivity.getCurrentLanguage ()Ljava/lang/String;"] = str("en");
    g_java["com/customRobTop/BaseRobTopActivity.isNetworkAvailable ()Z"] = yes;
    // Al volver de segundo plano el juego pregunta si debe reanudar la música.
    g_java["com/customRobTop/BaseRobTopActivity.shouldResumeSound ()Z"] = yes;
    g_java["android/os/Build$VERSION.SDK_INT I"] = [](uint32_t, auto&) -> uint64_t { return 28; };
    // Contexto de la app: Ascella comprueba el nombre del paquete.
    g_java["android/app/ActivityThread.currentActivityThread ()Landroid/app/ActivityThread;"] = [](uint32_t, auto&) -> uint64_t {
        return new_obj("android/app/ActivityThread");
    };
    g_java["android/app/ActivityThread.getApplication ()Landroid/app/Application;"] = [](uint32_t, auto&) -> uint64_t {
        return new_obj("android/app/Application");
    };
    for (const char* cls : {"android/content/Context", "android/app/Application", "android/app/Activity", "android/content/ContextWrapper"})
        g_java[std::string(cls) + ".getPackageName ()Ljava/lang/String;"] = str("app.onepointthree.starry_sky");
    g_java["com/customRobTop/BaseRobTopActivity.getUserID ()Ljava/lang/String;"] = [](uint32_t, auto&) -> uint64_t {
        return mem::strdup(install_id().c_str());
    };
    g_java["com/customRobTop/BaseRobTopActivity.doesFileExist (Ljava/lang/String;)Z"] = [](uint32_t, auto& a) -> uint64_t {
        return std::filesystem::exists(mem::str(uint32_t(a[0]))) ? 1 : 0;
    };
}

// Lee los argumentos de un método según su firma JNI.
std::vector<uint64_t> read_java_args(const std::string& sig, VaArgs* va, uint32_t jvalues) {
    std::vector<uint64_t> out;
    size_t i = sig.find('(') + 1;
    int idx = 0;
    while (i < sig.size() && sig[i] != ')') {
        char t = sig[i];
        bool wide = t == 'J' || t == 'D';
        while (sig[i] == '[') i++;
        if (sig[i] == 'L') i = sig.find(';', i);
        i++;
        uint64_t v;
        if (va) {
            if (t == 'F') {  // en varargs, float se promociona a double
                uint64_t bits = va->next64();
                double d; std::memcpy(&d, &bits, 8);
                float f = float(d); uint32_t fb; std::memcpy(&fb, &f, 4);
                v = fb;
            } else v = wide ? va->next64() : va->next32();
        } else {
            v = wide ? mem::r64(jvalues + idx * 8) : mem::r32(jvalues + idx * 8);
        }
        out.push_back(v);
        idx++;
    }
    return out;
}

std::string class_name(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_classes.find(h);
    return it == g_classes.end() ? "?" : it->second;
}

}  // namespace

void java_method(const std::string& desc, JMethod m) { g_java[desc] = std::move(m); }

uint32_t new_java_array(uint32_t len, uint32_t elem_size) {
    uint32_t a = mem::zalloc(8 + len * elem_size);
    mem::w32(a, len);
    return a;
}

uint32_t jni_vm() { return g_vm; }
uint32_t jni_env() { return g_env; }
uint32_t jni_native(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_natives.find(key);
    return it == g_natives.end() ? 0 : it->second;
}

void register_jni() {
    register_java();
    // Tabla JNINativeInterface: cada entrada es un trampolín.
    constexpr uint32_t n = sizeof(kJniNames) / sizeof(kJniNames[0]);
    uint32_t table = mem::zalloc(n * 4);
    std::map<std::string, cpu::Handler> impl;

    impl["GetVersion"] = [](Cpu& c) { ret(c, 0x00010006); };
    impl["FindClass"] = [](Cpu& c) {
        uint32_t h = new_handle();
        { std::lock_guard<std::mutex> lk(g_lock); g_classes[h] = args(c, 1); }
        logf("[jni] FindClass(%s) -> %x", args(c, 1), h);
        ret(c, h);
    };
    auto get_method = [](Cpu& c) {
        uint32_t id = new_handle();
        std::string desc = class_name(arg(c, 1)) + "." + args(c, 2) + " " + args(c, 3);
        { std::lock_guard<std::mutex> lk(g_lock); g_methods[id] = desc; }
        if (std::getenv("HOST_TRACE_JNI")) logf("[jni] id %x = %s", id, desc.c_str());
        ret(c, id);
    };
    impl["GetMethodID"] = get_method;
    impl["GetStaticMethodID"] = get_method;
    impl["GetFieldID"] = get_method;
    impl["GetStaticFieldID"] = get_method;
    impl["RegisterNatives"] = [](Cpu& c) {
        std::string cls = class_name(arg(c, 1));
        for (uint32_t i = 0; i < arg(c, 3); i++) {
            uint32_t m = arg(c, 2) + i * 12;  // {name, signature, fnPtr}
            std::string key = cls + "." + mem::str(mem::r32(m));
            std::lock_guard<std::mutex> lk(g_lock);
            g_natives[key] = mem::r32(m + 8);
        }
        logf("[jni] RegisterNatives(%s, %u métodos)", cls.c_str(), arg(c, 3));
        ret(c, 0);
    };
    for (const char* ref : {"NewGlobalRef", "NewLocalRef", "NewWeakGlobalRef"})
        impl[ref] = [](Cpu& c) { ret(c, arg(c, 1)); };
    for (const char* del : {"DeleteGlobalRef", "DeleteLocalRef", "DeleteWeakGlobalRef", "ExceptionClear", "ExceptionDescribe"})
        impl[del] = [](Cpu& c) { ret(c, 0); };
    impl["ExceptionCheck"] = [](Cpu& c) { ret(c, 0); };
    impl["ExceptionOccurred"] = [](Cpu& c) { ret(c, 0); };
    impl["PushLocalFrame"] = [](Cpu& c) { ret(c, 0); };
    impl["PopLocalFrame"] = [](Cpu& c) { ret(c, arg(c, 1)); };
    impl["EnsureLocalCapacity"] = [](Cpu& c) { ret(c, 0); };
    impl["GetObjectClass"] = [](Cpu& c) {
        JObj* o = obj(arg(c, 1));
        uint32_t h = new_handle();
        std::lock_guard<std::mutex> lk(g_lock);
        g_classes[h] = o ? o->cls : "java/lang/Object";
        ret(c, h);
    };
    impl["NewStringUTF"] = [](Cpu& c) { ret(c, mem::strdup(args(c, 1) ? args(c, 1) : "")); };
    impl["GetStringUTFChars"] = [](Cpu& c) { if (arg(c, 2)) *argp<uint8_t>(c, 2) = 0; ret(c, arg(c, 1)); };
    impl["ReleaseStringUTFChars"] = [](Cpu& c) {};
    impl["GetStringUTFLength"] = [](Cpu& c) { ret(c, arg(c, 1) ? uint32_t(std::strlen(args(c, 1))) : 0); };
    // Crear objetos: NewObject[V|A](env, clazz, ctor, ...) -> llama a "<init>" si está implementado.
    for (std::string v : {"NewObject", "NewObjectV", "NewObjectA"}) {
        char variant = v.back();
        impl[v] = [variant](Cpu& c) {
            std::string cls = class_name(arg(c, 1));
            uint32_t o = new_obj(cls);
            std::string desc;
            {
                std::lock_guard<std::mutex> lk(g_lock);
                auto m = g_methods.find(arg(c, 2));
                if (m != g_methods.end()) desc = m->second;
            }
            auto it = g_java.find(desc);
            if (it != g_java.end()) {
                std::string sig = desc.substr(desc.find(' ') + 1);
                std::vector<uint64_t> a;
                if (variant == 'V') { auto va = VaArgs::from_list(arg(c, 3)); a = read_java_args(sig, &va, 0); }
                else if (variant == 'A') a = read_java_args(sig, nullptr, arg(c, 3));
                else { auto va = VaArgs::from_regs(c, 3); a = read_java_args(sig, &va, 0); }
                it->second(o, a);
            }
            ret(c, o);
        };
    }
    impl["IsInstanceOf"] = [](Cpu& c) { ret(c, 1); };
    impl["IsSameObject"] = [](Cpu& c) { ret(c, arg(c, 1) == arg(c, 2)); };
    // Campos: Get[Static]<Tipo>Field(env, obj|clazz, fieldID) -> valor registrado en g_java con firma de campo.
    for (const char* t : {"Int", "Boolean", "Long", "Float", "Object", "Short", "Byte", "Char", "Double"}) {
        for (std::string pre : {"Get", "GetStatic"}) {
            impl[pre + t + "Field"] = [](Cpu& c) {
                std::string desc;
                {
                    std::lock_guard<std::mutex> lk(g_lock);
                    auto m = g_methods.find(arg(c, 2));
                    if (m != g_methods.end()) desc = m->second;
                }
                auto it = g_java.find(desc);
                ret64(c, it != g_java.end() ? it->second(arg(c, 1), {}) : 0);
            };
        }
    }
    impl["GetArrayLength"] = [](Cpu& c) { ret(c, arg(c, 1) ? mem::r32(arg(c, 1)) : 0); };
    const std::pair<const char*, uint32_t> types[] = {{"Boolean", 1}, {"Byte", 1}, {"Char", 2}, {"Short", 2},
                                                      {"Int", 4}, {"Long", 8}, {"Float", 4}, {"Double", 8}};
    for (auto [t, sz] : types) {
        std::string T = t;
        uint32_t esz = sz;
        impl["New" + T + "Array"] = [esz](Cpu& c) { ret(c, new_java_array(arg(c, 1), esz)); };
        impl["Get" + T + "ArrayElements"] = [](Cpu& c) {
            if (arg(c, 2)) *argp<uint8_t>(c, 2) = 0;
            ret(c, java_array_data(arg(c, 1)));
        };
        impl["Release" + T + "ArrayElements"] = [](Cpu& c) {};
        impl["Get" + T + "ArrayRegion"] = [esz](Cpu& c) {  // (env, arr, start, len, buf)
            std::memcpy(argp(c, 4), mem::ptr(java_array_data(arg(c, 1)) + arg(c, 2) * esz), arg(c, 3) * esz);
        };
        impl["Set" + T + "ArrayRegion"] = [esz](Cpu& c) {
            std::memcpy(mem::ptr(java_array_data(arg(c, 1)) + arg(c, 2) * esz), argp(c, 4), arg(c, 3) * esz);
        };
    }
    impl["GetPrimitiveArrayCritical"] = [](Cpu& c) { ret(c, java_array_data(arg(c, 1))); };
    impl["ReleasePrimitiveArrayCritical"] = [](Cpu& c) {};
    impl["GetJavaVM"] = [](Cpu& c) { mem::w32(arg(c, 1), g_vm); ret(c, 0); };

    for (uint32_t i = 0; i < n; i++) {
        std::string name = kJniNames[i];
        if (name == "reserved") continue;
        cpu::Handler h;
        auto it = impl.find(name);
        if (it != impl.end()) {
            h = it->second;
        } else if (name.rfind("Call", 0) == 0 && name.find("Method") != std::string::npos) {
            // Call[Static|Nonvirtual]<Tipo>Method[V|A](env, obj|clazz, [clazz,] methodID, ...)
            bool nonvirtual = name.find("Nonvirtual") != std::string::npos;
            char variant = name.back();  // 'V', 'A' o 'd' (variádica)
            h = [name, nonvirtual, variant](Cpu& c) {
                int mi = nonvirtual ? 3 : 2;
                std::string desc;
                {
                    std::lock_guard<std::mutex> lk(g_lock);
                    auto m = g_methods.find(arg(c, mi));
                    if (m != g_methods.end()) desc = m->second;
                }
                std::string sig = desc.substr(desc.find(' ') + 1);
                std::vector<uint64_t> a;
                if (variant == 'V') { auto va = VaArgs::from_list(arg(c, mi + 1)); a = read_java_args(sig, &va, 0); }
                else if (variant == 'A') a = read_java_args(sig, nullptr, arg(c, mi + 1));
                else { auto va = VaArgs::from_regs(c, mi + 1); a = read_java_args(sig, &va, 0); }
                auto it = g_java.find(desc);
                if (it == g_java.end()) {
                    static std::map<std::string, int> seen;
                    if (seen[desc]++ < 2) logf("[jni] %s %s (sin implementar)", name.c_str(), desc.c_str());
                    // Un String nulo rompe std::string en C++: devolvemos "" por defecto.
                    bool returns_string = sig.ends_with(")Ljava/lang/String;");
                    ret64(c, returns_string ? mem::strdup("") : 0);
                    return;
                }
                ret64(c, it->second(arg(c, 1), a));
            };
        } else {
            h = [name](Cpu& c) {
                // Llamadas a métodos de Java: anotamos cuál es para implementarlo después.
                std::string what;
                if (name.find("Method") != std::string::npos) {
                    std::lock_guard<std::mutex> lk(g_lock);
                    auto m = g_methods.find(arg(c, 2));
                    if (m != g_methods.end()) what = m->second;
                }
                logf("[jni] %s %s (sin implementar, devuelve 0)", name.c_str(), what.c_str());
                ret64(c, 0);
            };
        }
        mem::w32(table + i * 4, cpu::add_thunk("JNI::" + name, h));
    }
    g_env = mem::zalloc(4);
    mem::w32(g_env, table);

    // JavaVM: JNIInvokeInterface {3 reservados, DestroyJavaVM, AttachCurrentThread,
    // DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon}
    uint32_t vtable = mem::zalloc(8 * 4);
    auto put_env = [](Cpu& c) { if (arg(c, 1)) mem::w32(arg(c, 1), g_env); ret(c, 0); };
    mem::w32(vtable + 3 * 4, cpu::add_thunk("JavaVM::DestroyJavaVM", [](Cpu& c) { ret(c, 0); }));
    mem::w32(vtable + 4 * 4, cpu::add_thunk("JavaVM::AttachCurrentThread", put_env));
    mem::w32(vtable + 5 * 4, cpu::add_thunk("JavaVM::DetachCurrentThread", [](Cpu& c) { ret(c, 0); }));
    mem::w32(vtable + 6 * 4, cpu::add_thunk("JavaVM::GetEnv", put_env));
    mem::w32(vtable + 7 * 4, cpu::add_thunk("JavaVM::AttachCurrentThreadAsDaemon", put_env));
    g_vm = mem::zalloc(4);
    mem::w32(g_vm, vtable);
}

}  // namespace host
