# NBA Jam Mobile (Xbox Dev Mode)

## Put the OBB and APK here

```
D:\DevelopmentFiles\NBA Jam Mobile\
```

Copy into that folder:

| File | Notes |
|------|--------|
| Your `.apk` | Keep a copy of the Android package |
| Your `.obb` | e.g. `main.40080.com.eamobile.nbajam_row_wf.obb` |
| `libnbajam.so` | Extract from the APK (`lib/armeabi-v7a` or `lib/arm64-v8a`) |

Create the folder with Device Portal → File explorer, or from a PC if you have SMB/network access to the console.

On launch the app scans that path and lists what it finds.

## Display name

**NBA Jam Mobile** (identity `NbaJamMobile`)

## Build / sideload

GitHub Actions → **UWP Build** on `xbox-uwp`.  
Artifact: **NbaJamMobile**. Install `loader.cer`, then the `.appx` + dependencies via Device Portal.
