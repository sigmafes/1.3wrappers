// Cargador de librerías ELF32 ARM (como el linker de Android, simplificado).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ElfLib {
    std::string name;
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t symtab = 0, strtab = 0, nsyms = 0;
    uint32_t rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0;
    uint32_t init = 0, init_array = 0, init_arraysz = 0;
    uint32_t exidx = 0, exidx_count = 0;  // tablas de excepciones ARM
    std::vector<std::string> needed;
    std::unordered_map<std::string, uint32_t> exports;  // símbolo -> dirección absoluta
    std::map<uint32_t, std::string> funcs;               // para simbolizar direcciones
};

class ElfLoader {
public:
    // Busca un símbolo que no está en ninguna librería cargada (libc, GL, ...).
    // Devuelve 0 si no existe; is_object indica si es una variable.
    using HostResolver = std::function<uint32_t(const std::string& name, bool is_object)>;

    ElfLib* load(const std::string& path);
    void link_all(const HostResolver& host);
    void run_inits();
    uint32_t find(const std::string& name) const;
    // Sustituye un símbolo para todas las librerías que lo importen (la original sigue en find_original).
    void override_symbol(const std::string& name, uint32_t addr) { overrides_[name] = addr; }
    uint32_t find_original(const std::string& name) const;
    std::string symbolize(uint32_t addr) const;
    const std::vector<std::unique_ptr<ElfLib>>& libs() const { return libs_; }
    ElfLib* lib(const std::string& name) const;

private:
    std::vector<std::unique_ptr<ElfLib>> libs_;
    std::unordered_map<std::string, uint32_t> overrides_;
};
