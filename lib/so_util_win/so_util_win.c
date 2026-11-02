/*
 * so_util_win.c – Windows x64 Android ELF loader (load / relocate / resolve)
 *
 * Supports:
 *   - ELF32  ARMv7   (armeabi-v7a)
 *   - ELF64  AArch64 (arm64-v8a)
 *
 * Does NOT execute ARM/ARM64 code (needs dynarec later).
 */

#include "so_util_win.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  error "so_util_win is currently targeted at Windows"
#endif

#define ELFMAG      "\177ELF"
#define SELFMAG     4
#define ELFCLASS32  1
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_DYN      3
#define EM_ARM      40
#define EM_AARCH64  183
#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4
#define SHN_UNDEF   0
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_SONAME   14

/* ARM32 relocs */
#define R_ARM_NONE        0
#define R_ARM_ABS32       2
#define R_ARM_GLOB_DAT    21
#define R_ARM_JUMP_SLOT   22
#define R_ARM_RELATIVE    23

/* AArch64 relocs */
#define R_AARCH64_NONE       0
#define R_AARCH64_ABS64      257
#define R_AARCH64_GLOB_DAT   1025
#define R_AARCH64_JUMP_SLOT  1026
#define R_AARCH64_RELATIVE   1027

typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;

typedef struct {
    unsigned char e_ident[16];
    Elf32_Half e_type, e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off  e_phoff, e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align;
} Elf32_Phdr;

typedef struct {
    Elf32_Word sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
    Elf32_Word sh_link, sh_info, sh_addralign, sh_entsize;
} Elf32_Shdr;

typedef struct {
    Elf32_Sword d_tag;
    union { Elf32_Word d_val; Elf32_Addr d_ptr; } d_un;
} Elf32_Dyn;

typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info, st_other;
    Elf32_Half st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

#define ELF32_R_SYM(i)  ((i) >> 8)
#define ELF32_R_TYPE(i) ((unsigned char)(i))

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

typedef struct {
    unsigned char e_ident[16];
    Elf64_Half e_type, e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff, e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr, p_paddr;
    Elf64_Xword p_filesz, p_memsz, p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Word  sh_name, sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link, sh_info;
    Elf64_Xword sh_addralign, sh_entsize;
} Elf64_Shdr;

typedef struct {
    Elf64_Sxword d_tag;
    union { Elf64_Xword d_val; Elf64_Addr d_ptr; } d_un;
} Elf64_Dyn;

typedef struct {
    Elf64_Word st_name;
    unsigned char st_info, st_other;
    Elf64_Half st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
} Elf64_Sym;

typedef struct {
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
} Elf64_Rel;

typedef struct {
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;

#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)(i))

/* Extra fields stored via is_arm64; we also track RELA counts in unused slots
 * by packing: num_reldyn = count of .rela.dyn, num_relplt = count of .rela.plt
 * for arm64. Pointers reldyn/relplt point at Elf64_Rela arrays. */

static so_module *g_head = NULL;
static so_module *g_tail = NULL;

static void unresolved_stub(void) {
    fprintf(stderr, "[so_util_win] call into unresolved import\n");
}

static void *win_alloc_rwx(size_t size) {
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static void win_free(void *p, size_t size) {
    (void)size;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

static int load_image32(so_module *mod, const void *so_data, size_t so_size) {
    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)mod->file_image;
    mod->is_arm64 = 0;
    mod->ehdr = ehdr;
    mod->phdr = (Elf32_Phdr *)((uint8_t *)mod->file_image + ehdr->e_phoff);
    mod->shdr = (Elf32_Shdr *)((uint8_t *)mod->file_image + ehdr->e_shoff);
    mod->shstr = (char *)((uint8_t *)mod->file_image +
                          ((Elf32_Shdr *)mod->shdr)[ehdr->e_shstrndx].sh_offset);

    uintptr_t min_vaddr = UINTPTR_MAX, max_vaddr = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *ph = &((Elf32_Phdr *)mod->phdr)[i];
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
        uintptr_t end = (uintptr_t)ph->p_vaddr + ph->p_memsz;
        if (end > max_vaddr) max_vaddr = end;
    }
    if (min_vaddr == UINTPTR_MAX) return -1;

    size_t span = SO_ALIGN(max_vaddr - min_vaddr, 0x1000);
    void *base = win_alloc_rwx(span);
    if (!base) return -1;
    mod->text_map = base;
    mod->text_map_size = span;
    mod->text_base = (uintptr_t)base - min_vaddr;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *ph = &((Elf32_Phdr *)mod->phdr)[i];
        if (ph->p_type != PT_LOAD) continue;
        void *dest = (uint8_t *)base + (ph->p_vaddr - min_vaddr);
        if (ph->p_filesz)
            memcpy(dest, (uint8_t *)mod->file_image + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((uint8_t *)dest + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
        if (ph->p_flags & PF_X)
            mod->text_size = ph->p_memsz;
        else if (mod->n_data < SO_MAX_DATA_SEG) {
            mod->data_base[mod->n_data] = (uintptr_t)dest;
            mod->data_size[mod->n_data] = ph->p_memsz;
            mod->n_data++;
        }
    }

    for (int i = 0; i < ehdr->e_shnum; i++) {
        Elf32_Shdr *sh = &((Elf32_Shdr *)mod->shdr)[i];
        const char *name = mod->shstr + sh->sh_name;
        uintptr_t addr = mod->text_base + sh->sh_addr;
        if      (strcmp(name, ".dynamic") == 0) { mod->dynamic = (void *)addr; mod->num_dynamic = sh->sh_size / sizeof(Elf32_Dyn); }
        else if (strcmp(name, ".dynstr")  == 0) { mod->dynstr  = (char *)addr; }
        else if (strcmp(name, ".dynsym")  == 0) { mod->dynsym  = (void *)addr; mod->num_dynsym  = sh->sh_size / sizeof(Elf32_Sym); }
        else if (strcmp(name, ".rel.dyn") == 0) { mod->reldyn  = (void *)addr; mod->num_reldyn  = sh->sh_size / sizeof(Elf32_Rel); }
        else if (strcmp(name, ".rel.plt") == 0) { mod->relplt  = (void *)addr; mod->num_relplt  = sh->sh_size / sizeof(Elf32_Rel); }
        else if (strcmp(name, ".init_array") == 0) { mod->init_array = (void *)addr; mod->num_init_array = sh->sh_size / sizeof(void *); }
        else if (strcmp(name, ".hash") == 0) { mod->hash = (uint32_t *)addr; }
    }

    if (!mod->dynamic || !mod->dynstr || !mod->dynsym) return -2;

    for (int i = 0; i < mod->num_dynamic; i++) {
        Elf32_Dyn *d = &((Elf32_Dyn *)mod->dynamic)[i];
        if (d->d_tag == DT_SONAME)
            mod->soname = mod->dynstr + d->d_un.d_ptr;
    }
    return 0;
}

static int load_image64(so_module *mod, const void *so_data, size_t so_size) {
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)mod->file_image;
    mod->is_arm64 = 1;
    mod->ehdr = ehdr;
    mod->phdr = (uint8_t *)mod->file_image + ehdr->e_phoff;
    mod->shdr = (uint8_t *)mod->file_image + ehdr->e_shoff;
    mod->shstr = (char *)((uint8_t *)mod->file_image +
                          ((Elf64_Shdr *)mod->shdr)[ehdr->e_shstrndx].sh_offset);

    uintptr_t min_vaddr = UINTPTR_MAX, max_vaddr = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &((Elf64_Phdr *)mod->phdr)[i];
        if (ph->p_type != PT_LOAD) continue;
        if ((uintptr_t)ph->p_vaddr < min_vaddr) min_vaddr = (uintptr_t)ph->p_vaddr;
        uintptr_t end = (uintptr_t)ph->p_vaddr + (uintptr_t)ph->p_memsz;
        if (end > max_vaddr) max_vaddr = end;
    }
    if (min_vaddr == UINTPTR_MAX) return -1;

    size_t span = SO_ALIGN(max_vaddr - min_vaddr, 0x1000);
    void *base = win_alloc_rwx(span);
    if (!base) return -1;
    mod->text_map = base;
    mod->text_map_size = span;
    mod->text_base = (uintptr_t)base - min_vaddr;

    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf64_Phdr *ph = &((Elf64_Phdr *)mod->phdr)[i];
        if (ph->p_type != PT_LOAD) continue;
        void *dest = (uint8_t *)base + ((uintptr_t)ph->p_vaddr - min_vaddr);
        if (ph->p_filesz)
            memcpy(dest, (uint8_t *)mod->file_image + (size_t)ph->p_offset, (size_t)ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((uint8_t *)dest + (size_t)ph->p_filesz, 0, (size_t)(ph->p_memsz - ph->p_filesz));
        if (ph->p_flags & PF_X)
            mod->text_size = (size_t)ph->p_memsz;
        else if (mod->n_data < SO_MAX_DATA_SEG) {
            mod->data_base[mod->n_data] = (uintptr_t)dest;
            mod->data_size[mod->n_data] = (size_t)ph->p_memsz;
            mod->n_data++;
        }
    }

    for (int i = 0; i < ehdr->e_shnum; i++) {
        Elf64_Shdr *sh = &((Elf64_Shdr *)mod->shdr)[i];
        const char *name = mod->shstr + sh->sh_name;
        uintptr_t addr = mod->text_base + (uintptr_t)sh->sh_addr;
        if      (strcmp(name, ".dynamic") == 0) { mod->dynamic = (void *)addr; mod->num_dynamic = (int)(sh->sh_size / sizeof(Elf64_Dyn)); }
        else if (strcmp(name, ".dynstr")  == 0) { mod->dynstr  = (char *)addr; }
        else if (strcmp(name, ".dynsym")  == 0) { mod->dynsym  = (void *)addr; mod->num_dynsym  = (int)(sh->sh_size / sizeof(Elf64_Sym)); }
        else if (strcmp(name, ".rela.dyn") == 0) { mod->reldyn = (void *)addr; mod->num_reldyn = (int)(sh->sh_size / sizeof(Elf64_Rela)); }
        else if (strcmp(name, ".rela.plt") == 0) { mod->relplt = (void *)addr; mod->num_relplt = (int)(sh->sh_size / sizeof(Elf64_Rela)); }
        else if (strcmp(name, ".rel.dyn") == 0) { mod->reldyn = (void *)addr; mod->num_reldyn = (int)(sh->sh_size / sizeof(Elf64_Rel)); }
        else if (strcmp(name, ".rel.plt") == 0) { mod->relplt = (void *)addr; mod->num_relplt = (int)(sh->sh_size / sizeof(Elf64_Rel)); }
        else if (strcmp(name, ".init_array") == 0) { mod->init_array = (void *)addr; mod->num_init_array = (int)(sh->sh_size / sizeof(void *)); }
        else if (strcmp(name, ".hash") == 0) { mod->hash = (uint32_t *)addr; }
    }

    if (!mod->dynamic || !mod->dynstr || !mod->dynsym) return -2;

    for (int i = 0; i < mod->num_dynamic; i++) {
        Elf64_Dyn *d = &((Elf64_Dyn *)mod->dynamic)[i];
        if (d->d_tag == DT_SONAME)
            mod->soname = mod->dynstr + (size_t)d->d_un.d_ptr;
    }
    return 0;
}

static int load_image(so_module *mod, const void *so_data, size_t so_size) {
    memset(mod, 0, sizeof(*mod));

    if (so_size < 16 || memcmp(so_data, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "[so_util_win] not an ELF\n");
        return -1;
    }

    mod->file_image = malloc(so_size);
    if (!mod->file_image) return -1;
    memcpy(mod->file_image, so_data, so_size);
    mod->file_size = so_size;

    const unsigned char *id = (const unsigned char *)mod->file_image;
    int cls = id[4];
    uint16_t machine = *(uint16_t *)(id + 18); /* e_machine little-endian */

    int rc;
    if (cls == ELFCLASS32 && machine == EM_ARM) {
        fprintf(stderr, "[so_util_win] ELF32 ARM (armeabi-v7a)\n");
        rc = load_image32(mod, so_data, so_size);
    } else if (cls == ELFCLASS64 && machine == EM_AARCH64) {
        fprintf(stderr, "[so_util_win] ELF64 AArch64 (arm64-v8a)\n");
        rc = load_image64(mod, so_data, so_size);
    } else {
        fprintf(stderr, "[so_util_win] unsupported ELF class=%u machine=%u\n", cls, machine);
        free(mod->file_image);
        mod->file_image = NULL;
        return -1;
    }

    if (rc != 0) {
        so_unload(mod);
        return rc;
    }

    if (!g_head) { g_head = g_tail = mod; }
    else         { g_tail->next = mod; g_tail = mod; }
    return 0;
}

int so_mem_load(so_module *mod, const void *buffer, size_t so_size, uintptr_t load_addr) {
    (void)load_addr;
    return load_image(mod, buffer, so_size);
}

int so_file_load(so_module *mod, const char *filename, uintptr_t load_addr) {
    (void)load_addr;
    FILE *f = fopen(filename, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    fclose(f);
    int rc = load_image(mod, buf, (size_t)sz);
    free(buf);
    return rc;
}

static int so_relocate32(so_module *mod) {
    int total = mod->num_reldyn + mod->num_relplt;
    for (int i = 0; i < total; i++) {
        Elf32_Rel *rel = i < mod->num_reldyn
            ? &((Elf32_Rel *)mod->reldyn)[i]
            : &((Elf32_Rel *)mod->relplt)[i - mod->num_reldyn];
        Elf32_Sym *sym = &((Elf32_Sym *)mod->dynsym)[ELF32_R_SYM(rel->r_info)];
        uint32_t *ptr = (uint32_t *)(mod->text_base + rel->r_offset);
        int type = ELF32_R_TYPE(rel->r_info);
        uint32_t val;
        switch (type) {
        case R_ARM_RELATIVE:
            *ptr = (uint32_t)(*ptr + (uint32_t)mod->text_base);
            break;
        case R_ARM_ABS32:
            if (sym->st_shndx != SHN_UNDEF)
                *ptr = (uint32_t)(*ptr + (uint32_t)mod->text_base + sym->st_value);
            break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
            if (sym->st_shndx != SHN_UNDEF)
                *ptr = (uint32_t)(mod->text_base + sym->st_value);
            break;
        default:
            break;
        }
    }
    return 0;
}

static int so_relocate64(so_module *mod) {
    /* Prefer RELA (standard for AArch64 Android). */
    int total = mod->num_reldyn + mod->num_relplt;
    for (int i = 0; i < total; i++) {
        Elf64_Rela *rela = i < mod->num_reldyn
            ? &((Elf64_Rela *)mod->reldyn)[i]
            : &((Elf64_Rela *)mod->relplt)[i - mod->num_reldyn];
        Elf64_Sym *sym = &((Elf64_Sym *)mod->dynsym)[ELF64_R_SYM(rela->r_info)];
        uint64_t *ptr = (uint64_t *)(mod->text_base + (uintptr_t)rela->r_offset);
        uint32_t type = ELF64_R_TYPE(rela->r_info);
        switch (type) {
        case R_AARCH64_RELATIVE:
            *ptr = (uint64_t)mod->text_base + (uint64_t)rela->r_addend;
            break;
        case R_AARCH64_ABS64:
            if (sym->st_shndx != SHN_UNDEF)
                *ptr = (uint64_t)mod->text_base + sym->st_value + (uint64_t)rela->r_addend;
            break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
            if (sym->st_shndx != SHN_UNDEF)
                *ptr = (uint64_t)mod->text_base + sym->st_value;
            break;
        default:
            break;
        }
    }
    return 0;
}

int so_relocate(so_module *mod) {
    return mod->is_arm64 ? so_relocate64(mod) : so_relocate32(mod);
}

static uintptr_t resolve_from_deps(so_module *mod, const char *symbol) {
    if (mod->is_arm64) {
        for (int i = 0; i < mod->num_dynamic; i++) {
            Elf64_Dyn *d = &((Elf64_Dyn *)mod->dynamic)[i];
            if (d->d_tag != DT_NEEDED) continue;
            const char *needed = mod->dynstr + (size_t)d->d_un.d_ptr;
            for (so_module *c = g_head; c; c = c->next) {
                if (c == mod || !c->soname) continue;
                if (strcmp(c->soname, needed) == 0) {
                    uintptr_t link = so_symbol(c, symbol);
                    if (link) return link;
                }
            }
        }
    } else {
        for (int i = 0; i < mod->num_dynamic; i++) {
            Elf32_Dyn *d = &((Elf32_Dyn *)mod->dynamic)[i];
            if (d->d_tag != DT_NEEDED) continue;
            const char *needed = mod->dynstr + d->d_un.d_ptr;
            for (so_module *c = g_head; c; c = c->next) {
                if (c == mod || !c->soname) continue;
                if (strcmp(c->soname, needed) == 0) {
                    uintptr_t link = so_symbol(c, symbol);
                    if (link) return link;
                }
            }
        }
    }
    return 0;
}

static int so_resolve32(so_module *mod, so_default_dynlib *default_dynlib,
                        int size_default_dynlib, int default_dynlib_only) {
    int n_entries = size_default_dynlib / (int)sizeof(so_default_dynlib);
    int total = mod->num_reldyn + mod->num_relplt;
    for (int i = 0; i < total; i++) {
        Elf32_Rel *rel = i < mod->num_reldyn
            ? &((Elf32_Rel *)mod->reldyn)[i]
            : &((Elf32_Rel *)mod->relplt)[i - mod->num_reldyn];
        Elf32_Sym *sym = &((Elf32_Sym *)mod->dynsym)[ELF32_R_SYM(rel->r_info)];
        uint32_t *ptr = (uint32_t *)(mod->text_base + rel->r_offset);
        int type = ELF32_R_TYPE(rel->r_info);
        if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT)
            continue;
        if (sym->st_shndx != SHN_UNDEF) continue;

        const char *name = mod->dynstr + sym->st_name;
        int resolved = 0;
        if (!default_dynlib_only) {
            uintptr_t link = resolve_from_deps(mod, name);
            if (link) { *ptr = (uint32_t)link; resolved = 1; }
        }
        if (!resolved && default_dynlib) {
            for (int j = 0; j < n_entries; j++) {
                if (strcmp(name, default_dynlib[j].symbol) == 0) {
                    *ptr = (uint32_t)default_dynlib[j].func;
                    resolved = 1;
                    break;
                }
            }
        }
        if (!resolved) {
            fprintf(stderr, "[so_util_win] unresolved: %s\n", name);
            if (type == R_ARM_JUMP_SLOT)
                *ptr = (uint32_t)(uintptr_t)&unresolved_stub;
        }
    }
    return 0;
}

static int so_resolve64(so_module *mod, so_default_dynlib *default_dynlib,
                        int size_default_dynlib, int default_dynlib_only) {
    int n_entries = size_default_dynlib / (int)sizeof(so_default_dynlib);
    int total = mod->num_reldyn + mod->num_relplt;
    for (int i = 0; i < total; i++) {
        Elf64_Rela *rela = i < mod->num_reldyn
            ? &((Elf64_Rela *)mod->reldyn)[i]
            : &((Elf64_Rela *)mod->relplt)[i - mod->num_reldyn];
        Elf64_Sym *sym = &((Elf64_Sym *)mod->dynsym)[ELF64_R_SYM(rela->r_info)];
        uint64_t *ptr = (uint64_t *)(mod->text_base + (uintptr_t)rela->r_offset);
        uint32_t type = ELF64_R_TYPE(rela->r_info);
        if (type != R_AARCH64_ABS64 && type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT)
            continue;
        if (sym->st_shndx != SHN_UNDEF) continue;

        const char *name = mod->dynstr + sym->st_name;
        int resolved = 0;
        if (!default_dynlib_only) {
            uintptr_t link = resolve_from_deps(mod, name);
            if (link) { *ptr = (uint64_t)link; resolved = 1; }
        }
        if (!resolved && default_dynlib) {
            for (int j = 0; j < n_entries; j++) {
                if (strcmp(name, default_dynlib[j].symbol) == 0) {
                    *ptr = (uint64_t)default_dynlib[j].func;
                    resolved = 1;
                    break;
                }
            }
        }
        if (!resolved) {
            fprintf(stderr, "[so_util_win] unresolved: %s\n", name);
            if (type == R_AARCH64_JUMP_SLOT)
                *ptr = (uint64_t)(uintptr_t)&unresolved_stub;
        }
    }
    return 0;
}

int so_resolve(so_module *mod, so_default_dynlib *default_dynlib,
               int size_default_dynlib, int default_dynlib_only) {
    return mod->is_arm64
        ? so_resolve64(mod, default_dynlib, size_default_dynlib, default_dynlib_only)
        : so_resolve32(mod, default_dynlib, size_default_dynlib, default_dynlib_only);
}

void so_initialize(so_module *mod) {
    for (int i = 0; i < mod->num_init_array; i++) {
        if (mod->init_array[i] && (intptr_t)mod->init_array[i] != -1)
            fprintf(stderr, "[so_util_win] init_array[%d]=%p (not executed)\n",
                    i, (void *)mod->init_array[i]);
    }
}

uintptr_t so_symbol(so_module *mod, const char *symbol) {
    if (!mod || !mod->dynsym || !mod->dynstr) return 0;
    if (mod->is_arm64) {
        for (int i = 0; i < mod->num_dynsym; i++) {
            Elf64_Sym *sym = &((Elf64_Sym *)mod->dynsym)[i];
            if (strcmp(mod->dynstr + sym->st_name, symbol) == 0) {
                if (sym->st_shndx == SHN_UNDEF) return 0;
                return mod->text_base + (uintptr_t)sym->st_value;
            }
        }
    } else {
        for (int i = 0; i < mod->num_dynsym; i++) {
            Elf32_Sym *sym = &((Elf32_Sym *)mod->dynsym)[i];
            if (strcmp(mod->dynstr + sym->st_name, symbol) == 0) {
                if (sym->st_shndx == SHN_UNDEF) return 0;
                return mod->text_base + sym->st_value;
            }
        }
    }
    return 0;
}

void so_unload(so_module *mod) {
    if (!mod) return;
    if (mod->text_map) {
        win_free(mod->text_map, mod->text_map_size);
        mod->text_map = NULL;
    }
    free(mod->file_image);
    mod->file_image = NULL;

    so_module **pp = &g_head;
    while (*pp) {
        if (*pp == mod) {
            *pp = mod->next;
            if (g_tail == mod)
                g_tail = (*pp == NULL && pp == &g_head) ? NULL : g_head;
            /* simple: recompute tail */
            g_tail = g_head;
            if (g_tail) while (g_tail->next) g_tail = g_tail->next;
            break;
        }
        pp = &(*pp)->next;
    }
    memset(mod, 0, sizeof(*mod));
}

void so_dump_info(const so_module *mod) {
    if (!mod) return;
    printf("so_module @ %p  arch=%s\n", (const void *)mod, mod->is_arm64 ? "arm64" : "arm32");
    printf("  soname     : %s\n", mod->soname ? mod->soname : "(none)");
    printf("  text_base  : 0x%llx\n", (unsigned long long)mod->text_base);
    printf("  text_size  : %zu\n", mod->text_size);
    printf("  map        : %p (%zu bytes)\n", mod->text_map, mod->text_map_size);
    printf("  dynsym=%d reldyn=%d relplt=%d init=%d\n",
           mod->num_dynsym, mod->num_reldyn, mod->num_relplt, mod->num_init_array);
}
