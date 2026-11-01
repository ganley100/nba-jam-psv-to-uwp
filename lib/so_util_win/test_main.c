/*
 * Minimal smoke test for so_util_win.
 * Usage: so_util_win_test <path-to-libnbajam.so>
 *
 * Loads the .so, applies relocations, resolves a few common symbols against
 * a stub table, and prints basic info. Does NOT execute ARM code.
 */
#include "so_util_win.h"
#include <stdio.h>
#include <stdlib.h>

static int stub_ret0(void) { return 0; }
static void *stub_malloc(size_t n) { return malloc(n); }
static void  stub_free(void *p) { free(p); }

static so_default_dynlib stubs[] = {
    { "malloc",  (uintptr_t)stub_malloc },
    { "free",    (uintptr_t)stub_free   },
    { "__errno", (uintptr_t)stub_ret0   },
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.so>\n", argv[0]);
        return 1;
    }

    so_module mod;
    if (so_file_load(&mod, argv[1], 0) != 0) {
        fprintf(stderr, "load failed\n");
        return 1;
    }

    so_dump_info(&mod);

    if (so_relocate(&mod) != 0) {
        fprintf(stderr, "relocate failed\n");
        so_unload(&mod);
        return 1;
    }
    printf("relocate ok\n");

    so_resolve(&mod, stubs, sizeof(stubs), 0);
    printf("resolve done\n");

    /* Try a few symbols that NBA Jam is known to export. */
    const char *try_syms[] = {
        "JNI_OnLoad",
        "Java_com_eamobile_nbajam_NBAJamActivity_nativeOnCreate",
        "Java_com_eamobile_nbajam_NBAJamActivity_nativeKeyDown",
        NULL
    };
    for (int i = 0; try_syms[i]; i++) {
        uintptr_t a = so_symbol(&mod, try_syms[i]);
        printf("  %s -> 0x%llx\n", try_syms[i], (unsigned long long)a);
    }

    so_initialize(&mod); /* only prints; does not run ARM */
    so_unload(&mod);
    printf("ok\n");
    return 0;
}
