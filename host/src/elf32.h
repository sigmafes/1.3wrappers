// Definiciones mínimas de ELF de 32 bits (para no depender de <elf.h>, que Windows no tiene).
#pragma once
#include <cstdint>

struct Elf32_Ehdr {
    uint8_t e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Elf32_Phdr {
    uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
};
struct Elf32_Dyn {
    int32_t d_tag;
    union {
        uint32_t d_val;
        uint32_t d_ptr;
    } d_un;
};
struct Elf32_Sym {
    uint32_t st_name, st_value, st_size;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
};
struct Elf32_Rel {
    uint32_t r_offset, r_info;
};

#define ELFMAG "\177ELF"
#define SELFMAG 4
constexpr uint16_t EM_ARM = 40;
constexpr uint32_t PT_LOAD = 1, PT_DYNAMIC = 2;
constexpr int32_t DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6,
                  DT_INIT = 12, DT_REL = 17, DT_RELSZ = 18, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27;
constexpr uint16_t SHN_UNDEF = 0;
constexpr int STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2;
constexpr int STT_OBJECT = 1, STT_FUNC = 2;
constexpr uint32_t R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23;
#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((i) & 0xff)
#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)
