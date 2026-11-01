# NBA Jam · Android SO Loader for Xbox (UWP)

Fork of the [PS Vita Android SO loader](https://github.com/elliencode/nba-jam-psv) aimed at running the **official Android release of NBA Jam (v4.00.80)** on **Xbox Series X|S Developer Mode** via a native UWP app.

> **This is early scaffolding.** There is no playable build yet.

## Goal

A general-purpose UWP loader that lets the user supply their own legally obtained Android game files (`libnbajam.so` + data/OBB) and run them on Xbox Dev Mode.

The app itself ships **no game code or assets**.

## Status

| Component              | Status                          |
|------------------------|---------------------------------|
| Vita source (base)     | Present (this fork)             |
| `xbox-uwp` branch      | Created                         |
| GitHub Actions skeleton| Added                           |
| Windows/UWP project    | Not yet                         |
| ARM64 → x86-64 dynarec | Not yet                         |
| EGL/ANGLE / DX12       | Not yet                         |
| Input (XInput)         | Not yet                         |
| Sideloadable .appx     | Not yet                         |

## Plan (high level)

1. **Keep** the core SO loader (`lib/so_util`), FalsoJNI, and most of the reimpl layer.
2. **Replace** all Vita-specific pieces (kubridge, vitaGL, `ux0:` paths, `sceKernel*` calls).
3. Add an ARM64 → x86-64 dynamic recompiler (or static recomp later).
4. Bring up graphics via ANGLE → DX12 (or native DX12).
5. Map Xbox controllers and package as a Game-type UWP app for Device Portal sideloading.

## Legal

Same rules as the original Vita port:

- You must legally own NBA Jam for Android and supply your own `.apk` / `.obb` extracted files.
- This repository contains only the loader / compatibility layer.

See the original [Disclaimer](https://github.com/elliencode/nba-jam-psv) and the `LICENSE` file.

## Building (not ready yet)

The current tree still targets the Vita SDK. UWP build instructions will appear here once the Visual Studio / CMake UWP project lands.

## Credits

- **elliencode** / original Vita port authors
- **TheFloW** – Android SO loader foundation
- **Rinnegatamante** – so_util / VitaGL lineage
- Switch port (`nbajam_nx`) for arm64 reference

---

Branch: `xbox-uwp`  
Repo: https://github.com/ganley100/nba-jam-psv-to-uwp
