# NBA Jam Mobile (Xbox Dev Mode)

UWP loader shell for the Android version of NBA Jam on Xbox Series X|S Developer Mode.

## Display name

**NBA Jam Mobile** (package identity: `NbaJamMobile`)

On the Xbox it will show up under that name in Dev Mode / My games & apps.

## Game files folder

When the file picker lands, put your legally obtained Android data here:

```
NBA Jam Mobile/
  libnbajam.so
  (data / OBB contents)
```

You can keep that folder on a USB drive attached to the Xbox or under the app LocalState once import is implemented.

## Icons

Tiles use full-bleed artwork (no letterboxing). Source art is the official-style NBA Jam Mobile icon; CI builds all required UWP sizes (`StoreLogo`, `Square44`, `Square150`, `Wide310x150`, `SplashScreen`).

## Build

GitHub Actions → **UWP Build** on branch `xbox-uwp`.

Artifact name: **NbaJamMobile** (`.appx` + `loader.cer` + dependencies).

## Sideload (Device Portal)

1. Install `loader.cer`
2. Add the `.appx` and `Dependencies\x64`
3. Launch **NBA Jam Mobile**
