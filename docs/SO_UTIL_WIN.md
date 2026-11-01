# so_util_win

Windows / x64 rewrite of the core Android ELF loader.

## Location

```
lib/so_util_win/
  so_util_win.h
  so_util_win.c
  CMakeLists.txt
  test_main.c
```

## What it does today

- Loads a **32-bit ARM** Android `.so` into memory with `VirtualAlloc`
- Parses ELF headers / dynamic sections
- Applies `R_ARM_RELATIVE`, `R_ARM_ABS32`, `R_ARM_GLOB_DAT`, `R_ARM_JUMP_SLOT`
- Resolves imports against a caller-supplied symbol table
- Looks up exported symbols by name
- **Does not execute ARM code**

## Build (desktop smoke test)

```bash
cd lib/so_util_win
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64   # or Ninja / MinGW
cmake --build . --config Release
```

Then:

```bash
./Release/so_util_win_test.exe path/to/libnbajam.so
```

You should see the module info, any unresolved imports, and addresses for
`JNI_OnLoad` / the native activity methods if the file is the right version.

## Next steps

1. Add ARM64 (Elf64) support so we can prefer the Switch-port `.so`.
2. Integrate a dynarec / interpreter so `so_initialize` and game code can run.
3. Wire this library into a UWP app under `uwp/`.

## Relation to original `lib/so_util`

The original Vita sources stay untouched. This is a parallel implementation so
the Xbox work does not break the Vita tree.
