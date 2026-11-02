#ifndef SO_UTIL_WIN_H
#define SO_UTIL_WIN_H

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

    void     *text_map;
    size_t    text_map_size;
    void     *data_map[SO_MAX_DATA_SEG];
    size_t    data_map_size[SO_MAX_DATA_SEG];
    int       n_data;

    uintptr_t text_base;
    size_t    text_size;
    uintptr_t data_base[SO_MAX_DATA_SEG];
    size_t    data_size[SO_MAX_DATA_SEG];

    void     *file_image;
    size_t    file_size;

    void     *ehdr;
    void     *phdr;
    void     *shdr;

    void     *dynamic;
    void     *dynsym;
    void     *reldyn;
    void     *relplt;
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

    int       is_arm64;
} so_module;

/* Optional log sink (UTF-8 lines). Set from the app to capture loader diagnostics. */
typedef void (*so_log_fn)(const char *line);
void so_set_log(so_log_fn fn);

int      so_file_load(so_module *mod, const char *filename, uintptr_t load_addr);
int      so_mem_load (so_module *mod, const void *buffer, size_t so_size, uintptr_t load_addr);
int      so_relocate(so_module *mod);
int      so_resolve(so_module *mod,
                    so_default_dynlib *default_dynlib,
                    int size_default_dynlib,
                    int default_dynlib_only);
void     so_initialize(so_module *mod);
uintptr_t so_symbol(so_module *mod, const char *symbol);
void     so_unload(so_module *mod);
void     so_dump_info(const so_module *mod);

#ifdef __cplusplus
}
#endif

#endif
