#include "elf_loader.h"

#include "elf32.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <set>

#include "cpu.h"
#include "log.h"
#include "memory.h"

namespace {

std::string strip_version(const char* s) {
    std::string n(s);
    auto at = n.find('@');
    return at == std::string::npos ? n : n.substr(0, at);
}

std::string basename_of(const std::string& p) {
    auto s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

ElfLib* ElfLoader::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        logf("[elf] no se pudo abrir %s", path.c_str());
        return nullptr;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
    auto* eh = reinterpret_cast<const Elf32_Ehdr*>(file.data());
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_machine != EM_ARM) {
        logf("[elf] %s no es ELF ARM", path.c_str());
        return nullptr;
    }
    auto* ph = reinterpret_cast<const Elf32_Phdr*>(file.data() + eh->e_phoff);

    uint32_t span = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) span = std::max(span, ph[i].p_vaddr + ph[i].p_memsz);

    auto lib = std::make_unique<ElfLib>();
    lib->name = basename_of(path);
    lib->size = (span + 0xFFF) & ~0xFFFu;
    lib->base = mem::reserve_lib(lib->size);
    uint32_t b = lib->base;

    const Elf32_Dyn* dyn = nullptr;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            std::memcpy(mem::ptr(b + ph[i].p_vaddr), file.data() + ph[i].p_offset, ph[i].p_filesz);
        } else if (ph[i].p_type == 0x70000001) {  // PT_ARM_EXIDX
            lib->exidx = b + ph[i].p_vaddr;
            lib->exidx_count = ph[i].p_memsz / 8;
        } else if (ph[i].p_type == PT_DYNAMIC) {
            dyn = reinterpret_cast<const Elf32_Dyn*>(file.data() + ph[i].p_offset);
        }
    }
    if (!dyn) {
        logf("[elf] %s sin PT_DYNAMIC", path.c_str());
        return nullptr;
    }
    std::vector<uint32_t> needed_off;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        uint32_t v = dyn->d_un.d_val;
        switch (dyn->d_tag) {
        case DT_NEEDED: needed_off.push_back(v); break;
        case DT_SYMTAB: lib->symtab = b + v; break;
        case DT_STRTAB: lib->strtab = b + v; break;
        case DT_HASH: lib->nsyms = mem::r32(b + v + 4); break;
        case DT_REL: lib->rel = b + v; break;
        case DT_RELSZ: lib->relsz = v; break;
        case DT_JMPREL: lib->jmprel = b + v; break;
        case DT_PLTRELSZ: lib->pltrelsz = v; break;
        case DT_INIT: lib->init = b + v; break;
        case DT_INIT_ARRAY: lib->init_array = b + v; break;
        case DT_INIT_ARRAYSZ: lib->init_arraysz = v; break;
        default: break;
        }
    }
    for (uint32_t o : needed_off) lib->needed.push_back(mem::str(lib->strtab + o));

    // Símbolos exportados.
    for (uint32_t i = 1; i < lib->nsyms; i++) {
        auto* s = mem::ptr<Elf32_Sym>(lib->symtab + i * sizeof(Elf32_Sym));
        if (s->st_shndx == SHN_UNDEF || s->st_name == 0) continue;
        std::string n = strip_version(mem::str(lib->strtab + s->st_name));
        uint32_t a = b + s->st_value;
        int bind = ELF32_ST_BIND(s->st_info);
        if (bind == STB_GLOBAL || bind == STB_WEAK) {
            if (!lib->exports.count(n) || bind == STB_GLOBAL) lib->exports[n] = a;
        }
        if (ELF32_ST_TYPE(s->st_info) == STT_FUNC) lib->funcs[a & ~1u] = n;
    }

    logf("[elf] %s cargada en %08x-%08x (%zu símbolos)", lib->name.c_str(), b, b + lib->size, lib->exports.size());
    libs_.push_back(std::move(lib));
    return libs_.back().get();
}

uint32_t ElfLoader::find(const std::string& name) const {
    auto o = overrides_.find(name);
    if (o != overrides_.end()) return o->second;
    return find_original(name);
}

uint32_t ElfLoader::find_original(const std::string& name) const {
    for (auto& l : libs_) {
        auto it = l->exports.find(name);
        if (it != l->exports.end()) return it->second;
    }
    return 0;
}

ElfLib* ElfLoader::lib(const std::string& name) const {
    for (auto& l : libs_)
        if (l->name == name) return l.get();
    return nullptr;
}

void ElfLoader::link_all(const HostResolver& host) {
    std::set<std::string> missing;
    for (auto& lp : libs_) {
        ElfLib& l = *lp;
        auto apply = [&](uint32_t table, uint32_t size) {
            for (uint32_t off = 0; off < size; off += sizeof(Elf32_Rel)) {
                auto* r = mem::ptr<Elf32_Rel>(table + off);
                uint32_t where = l.base + r->r_offset;
                uint32_t type = ELF32_R_TYPE(r->r_info);
                uint32_t symi = ELF32_R_SYM(r->r_info);
                uint32_t S = 0;
                if (symi) {
                    auto* s = mem::ptr<Elf32_Sym>(l.symtab + symi * sizeof(Elf32_Sym));
                    std::string n = strip_version(mem::str(l.strtab + s->st_name));
                    bool defined_here = s->st_shndx != SHN_UNDEF;
                    if (defined_here && ELF32_ST_BIND(s->st_info) == STB_LOCAL) S = l.base + s->st_value;
                    else S = find(n);
                    if (!S && defined_here) S = l.base + s->st_value;
                    if (!S) S = host(n, ELF32_ST_TYPE(s->st_info) == STT_OBJECT);
                    if (!S && ELF32_ST_BIND(s->st_info) != STB_WEAK) missing.insert(n);
                }
                switch (type) {
                case R_ARM_RELATIVE: mem::w32(where, mem::r32(where) + l.base); break;
                case R_ARM_GLOB_DAT:
                case R_ARM_JUMP_SLOT: mem::w32(where, S); break;
                case R_ARM_ABS32: mem::w32(where, mem::r32(where) + S); break;
                default: logf("[elf] %s: relocación %u no soportada", l.name.c_str(), type); std::abort();
                }
            }
        };
        apply(l.rel, l.relsz);
        apply(l.jmprel, l.pltrelsz);
    }
    for (auto& n : missing) logf("[elf] símbolo sin resolver: %s", n.c_str());
}

void ElfLoader::run_inits() {
    for (auto& l : libs_) {
        if (l->init) cpu::call(l->init);
        for (uint32_t i = 0; i < l->init_arraysz / 4; i++) {
            uint32_t fn = mem::r32(l->init_array + i * 4);
            if (fn && fn != 0xFFFFFFFF) cpu::call(fn);
        }
        logf("[elf] constructores de %s ejecutados", l->name.c_str());
    }
}

std::string ElfLoader::symbolize(uint32_t addr) const {
    char buf[256];
    for (auto& l : libs_) {
        if (addr < l->base || addr >= l->base + l->size) continue;
        auto it = l->funcs.upper_bound(addr);
        if (it != l->funcs.begin()) {
            --it;
            std::snprintf(buf, sizeof buf, "(%s+%x: %s+%x)", l->name.c_str(), addr - l->base, it->second.c_str(), addr - it->first);
        } else {
            std::snprintf(buf, sizeof buf, "(%s+%x)", l->name.c_str(), addr - l->base);
        }
        return buf;
    }
    if (addr >= mem::kThunkBase) return "(thunk " + cpu::thunk_name((addr - mem::kThunkBase) / 8) + ")";
    return "";
}
