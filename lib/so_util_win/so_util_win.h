#ifndef SO_UTIL_WIN_H
#define SO_UTIL_WIN_H

/*
 * Windows / x64 port of the core Android ELF (.so) loader.
 *
 * Goals for this first cut:
 *  - Load an ARMv7 / ARM64 Android .so into memory
 *  - Parse headers, apply basic relocations, resolve symbols against a table
 *  - NO execution of ARM code yet (that comes with the dynarec)
 *
 * Original Vita so_util by TheFloW / Rinnegatamante lineage.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SO_MAX_DATA_SEG 8
#define SO_ALIGN(x, a)  (((x) + ((a) - 1)) & ~((a) - 1))

typedef struct so_default_dynlib {
    const char *symbol;
    uintptr_t   func;
} so_default_dynlib;

typedef struct so_module {
    struct so_module *next;

    /* Mapped segments */
    void     *text_map;          /* VirtualAlloc base for executable-ish region */
    size_t    text_map_size;
    void     *data_map[SO_MAX_DATA_SEG];
    size_t    data_map_size[SO_MAX_DATA_SEG];
    int       n_data;

    uintptr_t text_base;
    size_t    text_size;
    uintptr_t data_base[SO_MAX_DATA_SEG];
    size_t    data_size[SO_MAX_DATA_SEG];

    /* ELF metadata (point into the mapped image or a copy of the file) */
    void     *file_image;        /* full file kept for section walking if needed */
    size_t    file_size;

    /* 32-bit ELF (Android ARMv7 games). ARM64 support can be added later. */
    void     *ehdr;              /* Elf32_Ehdr* */
    void     *phdr;              /* Elf32_Phdr* */
    void     *shdr;              /* Elf32_Shdr* */

    void     *dynamic;           /* Elf32_Dyn* */
    void     *dynsym;            /* Elf32_Sym* */
    void     *reldyn;            /* Elf32_Rel* */
    void     *relplt;            /* Elf32_Rel* */
    int     (**init_array)(void);
    uint32_t *hash;

    int       num_dynamic;
    int       num_dynsym;
    int       num_reldyn;
    int       num_relplt;
    int       num_init_array;

    char     *soname;
    char     *shstr;
    char     *dynstr;

    int       is_arm64;          /* 0 = Elf32/ARMv7, 1 = future Elf64 */
} so_module;

/* Load from path or memory buffer. load_addr is currently ignored on Windows
 * (we let the OS choose); kept for API compatibility with the Vita loader. */
int      so_file_load(so_module *mod, const char *filename, uintptr_t load_addr);
int      so_mem_load (so_module *mod, const void *buffer, size_t so_size, uintptr_t load_addr);

/* Apply load-relative relocations (R_ARM_RELATIVE etc.). */
int      so_relocate(so_module *mod);

/* Resolve undefined symbols against the supplied table.
 * Unresolved JUMP_SLOT entries are left pointing at a trap stub. */
int      so_resolve(so_module *mod,
                    so_default_dynlib *default_dynlib,
                    int size_default_dynlib,
                    int default_dynlib_only);

/* Call .init_array constructors (only safe after a working dynarec). */
void     so_initialize(so_module *mod);

/* Symbol lookup by name. Returns 0 if not found. */
uintptr_t so_symbol(so_module *mod, const char *symbol);

/* Free all mappings belonging to the module. */
void     so_unload(so_module *mod);

/* Optional: print basic info to stdout for debugging. */
void     so_dump_info(const so_module *mod);

#ifdef __cplusplus
}
#endif

#endif /* SO_UTIL_WIN_H */
