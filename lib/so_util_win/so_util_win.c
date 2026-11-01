/*
 * so_util_win.c – Windows x64 Android ELF loader (load / relocate / resolve)
 *
 * Derived from the Vita so_util (TheFloW / Rinnegatamante lineage).
 * This version removes all Vita / kubridge dependencies and does NOT
 * execute ARM code. It is the foundation for a later dynarec layer.
 */

#include "so_util_win.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#  error "so_util_win is currently targeted at Windows"
#endif

/* Minimal ELF32 definitions so we do not depend on the huge Vita elf.h */
#define ELFMAG      "\177ELF"
#define SELFMAG     4
#define ELFCLASS32  1
#define ELFDATA2LSB 1
#define ET_DYN      3
#define EM_ARM      40
#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define PF_R        4
#define SHT_NULL    0
#define SHN_UNDEF   0
#define DT_NULL     0
#define DT_NEEDED   1
#define DT_SONAME   14
#define R_ARM_NONE        0
#define R_ARM_ABS32       2
#define R_ARM_RELATIVE    23
#define R_ARM_GLOB_DAT    21
#define R_ARM_JUMP_SLOT   22

typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;

typedef struct {
    unsigned char e_ident[16];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off  e_phoff;
    Elf32_Off  e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

typedef struct {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off  sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
} Elf32_Shdr;

typedef struct {
    Elf32_Sword d_tag;
    union {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
} Elf32_Dyn;

typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

#define ELF32_R_SYM(i)  ((i) >> 8)
#define ELF32_R_TYPE(i) ((unsigned char)(i))

static so_module *g_head = NULL;
static so_module *g_tail = NULL;

/* Trap for unresolved JUMP_SLOT – just a safe no-op / breakpoint placeholder. */
static void unresolved_stub(void) {
    fprintf(stderr, "[so_util_win] call into unresolved import\n");
}

static void *win_alloc_rwx(size_t size) {
    void *p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    return p;
}

static void *win_alloc_rw(size_t size) {
    void *p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return p;
}

static void win_free(void *p, size_t size) {
    (void)size;
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

static int load_image(so_module *mod, const void *so_data, size_t so_size) {
    memset(mod, 0, sizeof(*mod));

    if (so_size < sizeof(Elf32_Ehdr) || memcmp(so_data, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "[so_util_win] not an ELF\n");
        return -1;
    }

    /* Keep a private copy of the file so section headers stay valid. */
    mod->file_image = malloc(so_size);
    if (!mod->file_image) return -1;
    memcpy(mod->file_image, so_data, so_size);
    mod->file_size = so_size;

    Elf32_Ehdr *ehdr = (Elf32_Ehdr *)mod->file_image;
    if (ehdr->e_ident[4] != ELFCLASS32 || ehdr->e_machine != EM_ARM) {
        fprintf(stderr, "[so_util_win] only 32-bit ARM ELFs supported in this cut (class=%u machine=%u)\n",
                ehdr->e_ident[4], ehdr->e_machine);
        free(mod->file_image);
        mod->file_image = NULL;
        return -1;
    }

    mod->ehdr = ehdr;
    mod->phdr = (Elf32_Phdr *)((uint8_t *)mod->file_image + ehdr->e_phoff);
    mod->shdr = (Elf32_Shdr *)((uint8_t *)mod->file_image + ehdr->e_shoff);
    mod->shstr = (char *)((uint8_t *)mod->file_image + ((Elf32_Shdr *)mod->shdr)[ehdr->e_shstrndx].sh_offset);

    /* Find the overall address span of PT_LOAD segments so we can map one
     * contiguous region (simpler than the Vita fixed-address scheme). */
    uintptr_t min_vaddr = UINTPTR_MAX;
    uintptr_t max_vaddr = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *ph = &((Elf32_Phdr *)mod->phdr)[i];
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
        uintptr_t end = ph->p_vaddr + ph->p_memsz;
        if (end > max_vaddr) max_vaddr = end;
    }
    if (min_vaddr == UINTPTR_MAX) {
        fprintf(stderr, "[so_util_win] no PT_LOAD segments\n");
        free(mod->file_image);
        return -1;
    }

    size_t span = SO_ALIGN(max_vaddr - min_vaddr, 0x1000);
    void *base = win_alloc_rwx(span);
    if (!base) {
        fprintf(stderr, "[so_util_win] VirtualAlloc failed (%lu)\n", GetLastError());
        free(mod->file_image);
        return -1;
    }

    mod->text_map = base;
    mod->text_map_size = span;
    mod->text_base = (uintptr_t)base - min_vaddr; /* bias so vaddr + bias = real */

    /* Copy each LOAD segment into the mapping. */
    for (int i = 0; i < ehdr->e_phnum; i++) {
        Elf32_Phdr *ph = &((Elf32_Phdr *)mod->phdr)[i];
        if (ph->p_type != PT_LOAD) continue;

        void *dest = (uint8_t *)base + (ph->p_vaddr - min_vaddr);
        if (ph->p_filesz)
            memcpy(dest, (uint8_t *)mod->file_image + ph->p_offset, ph->p_filesz);
        if (ph->p_memsz > ph->p_filesz)
            memset((uint8_t *)dest + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);

        if (ph->p_flags & PF_X) {
            mod->text_size = ph->p_memsz;
        } else {
            if (mod->n_data < SO_MAX_DATA_SEG) {
                mod->data_base[mod->n_data] = (uintptr_t)dest;
                mod->data_size[mod->n_data] = ph->p_memsz;
                mod->n_data++;
            }
        }
    }

    /* Locate dynamic sections by name. */
    for (int i = 0; i < ehdr->e_shnum; i++) {
        Elf32_Shdr *sh = &((Elf32_Shdr *)mod->shdr)[i];
        const char *name = mod->shstr + sh->sh_name;
        uintptr_t addr = mod->text_base + sh->sh_addr;

        if      (strcmp(name, ".dynamic")   == 0) { mod->dynamic    = (void *)addr; mod->num_dynamic    = sh->sh_size / sizeof(Elf32_Dyn); }
        else if (strcmp(name, ".dynstr")    == 0) { mod->dynstr     = (char *)addr; }
        else if (strcmp(name, ".dynsym")    == 0) { mod->dynsym     = (void *)addr; mod->num_dynsym     = sh->sh_size / sizeof(Elf32_Sym); }
        else if (strcmp(name, ".rel.dyn")   == 0) { mod->reldyn     = (void *)addr; mod->num_reldyn     = sh->sh_size / sizeof(Elf32_Rel); }
        else if (strcmp(name, ".rel.plt")   == 0) { mod->relplt     = (void *)addr; mod->num_relplt     = sh->sh_size / sizeof(Elf32_Rel); }
        else if (strcmp(name, ".init_array")== 0) { mod->init_array = (void *)addr; mod->num_init_array = sh->sh_size / sizeof(void *); }
        else if (strcmp(name, ".hash")      == 0) { mod->hash       = (uint32_t *)addr; }
    }

    if (!mod->dynamic || !mod->dynstr || !mod->dynsym) {
        fprintf(stderr, "[so_util_win] missing required dynamic sections\n");
        so_unload(mod);
        return -2;
    }

    for (int i = 0; i < mod->num_dynamic; i++) {
        Elf32_Dyn *d = &((Elf32_Dyn *)mod->dynamic)[i];
        if (d->d_tag == DT_SONAME)
            mod->soname = mod->dynstr + d->d_un.d_ptr;
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
    if (!f) {
        fprintf(stderr, "[so_util_win] cannot open %s\n", filename);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }

    void *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    int rc = load_image(mod, buf, (size_t)sz);
    free(buf);
    return rc;
}

int so_relocate(so_module *mod) {
    int total = mod->num_reldyn + mod->num_relplt;
    for (int i = 0; i < total; i++) {
        Elf32_Rel *rel = i < mod->num_reldyn
            ? &((Elf32_Rel *)mod->reldyn)[i]
            : &((Elf32_Rel *)mod->relplt)[i - mod->num_reldyn];
        Elf32_Sym *sym = &((Elf32_Sym *)mod->dynsym)[ELF32_R_SYM(rel->r_info)];
        uintptr_t *ptr = (uintptr_t *)(mod->text_base + rel->r_offset);
        int type = ELF32_R_TYPE(rel->r_info);
        uintptr_t val;

        switch (type) {
        case R_ARM_RELATIVE:
            val = *ptr + mod->text_base;
            *ptr = val;
            break;
        case R_ARM_ABS32:
            if (sym->st_shndx != SHN_UNDEF) {
                val = *ptr + mod->text_base + sym->st_value;
                *ptr = val;
            }
            break;
        case R_ARM_GLOB_DAT:
        case R_ARM_JUMP_SLOT:
            if (sym->st_shndx != SHN_UNDEF) {
                val = mod->text_base + sym->st_value;
                *ptr = val;
            }
            break;
        case R_ARM_NONE:
            break;
        default:
            /* Leave unknown types alone for now. */
            break;
        }
    }
    return 0;
}

static uintptr_t resolve_from_deps(so_module *mod, const char *symbol) {
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
    return 0;
}

int so_resolve(so_module *mod, so_default_dynlib *default_dynlib,
               int size_default_dynlib, int default_dynlib_only) {
    int n_entries = size_default_dynlib / (int)sizeof(so_default_dynlib);
    int total = mod->num_reldyn + mod->num_relplt;

    for (int i = 0; i < total; i++) {
        Elf32_Rel *rel = i < mod->num_reldyn
            ? &((Elf32_Rel *)mod->reldyn)[i]
            : &((Elf32_Rel *)mod->relplt)[i - mod->num_reldyn];
        Elf32_Sym *sym = &((Elf32_Sym *)mod->dynsym)[ELF32_R_SYM(rel->r_info)];
        uintptr_t *ptr = (uintptr_t *)(mod->text_base + rel->r_offset);
        int type = ELF32_R_TYPE(rel->r_info);

        if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT)
            continue;
        if (sym->st_shndx != SHN_UNDEF)
            continue;

        const char *name = mod->dynstr + sym->st_name;
        int resolved = 0;

        if (!default_dynlib_only) {
            uintptr_t link = resolve_from_deps(mod, name);
            if (link) {
                *ptr = (type == R_ARM_ABS32) ? (*ptr + link) : link;
                resolved = 1;
            }
        }

        if (!resolved && default_dynlib) {
            for (int j = 0; j < n_entries; j++) {
                if (strcmp(name, default_dynlib[j].symbol) == 0) {
                    *ptr = default_dynlib[j].func;
                    resolved = 1;
                    break;
                }
            }
        }

        if (!resolved) {
            fprintf(stderr, "[so_util_win] unresolved: %s\n", name);
            if (type == R_ARM_JUMP_SLOT)
                *ptr = (uintptr_t)&unresolved_stub;
        }
    }
    return 0;
}

void so_initialize(so_module *mod) {
    /* Only call this after a dynarec can actually run the ARM constructors. */
    for (int i = 0; i < mod->num_init_array; i++) {
        if (mod->init_array[i] && (intptr_t)mod->init_array[i] != -1) {
            fprintf(stderr, "[so_util_win] init_array[%d]=%p (not executed – no dynarec yet)\n",
                    i, (void *)mod->init_array[i]);
        }
    }
}

uintptr_t so_symbol(so_module *mod, const char *symbol) {
    if (!mod || !mod->dynsym || !mod->dynstr) return 0;
    for (int i = 0; i < mod->num_dynsym; i++) {
        Elf32_Sym *sym = &((Elf32_Sym *)mod->dynsym)[i];
        if (strcmp(mod->dynstr + sym->st_name, symbol) == 0) {
            if (sym->st_shndx == SHN_UNDEF) return 0;
            return mod->text_base + sym->st_value;
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
    for (int i = 0; i < mod->n_data; i++) {
        /* data lives inside text_map in this simple mapping scheme */
        mod->data_map[i] = NULL;
    }
    free(mod->file_image);
    mod->file_image = NULL;

    /* Unlink from global list */
    so_module **pp = &g_head;
    while (*pp) {
        if (*pp == mod) {
            *pp = mod->next;
            if (g_tail == mod) g_tail = (pp == &g_head) ? NULL : (so_module *)((char *)pp - offsetof(so_module, next));
            break;
        }
        pp = &(*pp)->next;
    }
    memset(mod, 0, sizeof(*mod));
}

void so_dump_info(const so_module *mod) {
    if (!mod) return;
    printf("so_module @ %p\n", (const void *)mod);
    printf("  soname     : %s\n", mod->soname ? mod->soname : "(none)");
    printf("  text_base  : 0x%llx\n", (unsigned long long)mod->text_base);
    printf("  text_size  : %zu\n", mod->text_size);
    printf("  map        : %p (%zu bytes)\n", mod->text_map, mod->text_map_size);
    printf("  dynsym     : %d  reldyn: %d  relplt: %d\n",
           mod->num_dynsym, mod->num_reldyn, mod->num_relplt);
    printf("  init_array : %d entries\n", mod->num_init_array);
}
