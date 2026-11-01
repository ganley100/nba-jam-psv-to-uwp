# Xbox UWP Port Notes

## What we keep from the Vita tree

- `lib/so_util/` – ELF loader core
- `lib/falso_jni/` – fake JNI (submodule)
- `source/dynlib.c` – import resolution table
- `source/java.c` – Java method stubs
- Most of `source/reimpl/` structure (controls, io, mem, pthr, log, asset_manager, etc.)
- `source/patch.c` – game-specific patches (reference)

## What must be replaced

| Vita piece              | Xbox replacement                          |
|-------------------------|-------------------------------------------|
| kubridge / RWX tricks   | Windows VirtualAlloc + PAGE_EXECUTE_READWRITE / dynarec |
| vitaGL + EGL            | ANGLE (GLES→DX12) or native DirectX 12   |
| `ux0:data/...` paths    | UWP Storage / user-selected folder        |
| `sceKernel*` threads    | Win32 / C++11 threads / UWP concurrency   |
| Vita input              | XInput / GameInput                        |
| Vita CMake + VITASDK    | Visual Studio UWP project or CMake + MSVC |

## Architecture mismatch

Xbox Series X|S UWP apps are **x86-64**.  
The Android game ships **ARMv7** (Vita port) or **arm64-v8a** (Switch port).

Options:

1. **Dynamic recompilation** (FEX-style or custom) – preferred for iteration speed
2. **Static recompilation** – better long-term performance, more work up front
3. Prefer the **arm64** `.so` from the Switch port when possible

## First real milestones

1. Make `so_util` compile and load an ELF on Windows x64 (no execution yet).
2. Resolve symbols against a stub table.
3. Integrate a minimal ARM64 dynarec or interpreter.
4. Bring up a blank UWP window + ANGLE context.
5. Drive the game lifecycle (`JNI_OnLoad` → `nativeOnCreate` → frame loop).

## Data the user must supply

- `libnbajam.so` (prefer arm64-v8a from the Android APK)
- Game data / OBB contents (or a packed equivalent)
- Optional: `libNimble.so` if required by the build

The loader never ships these files.
