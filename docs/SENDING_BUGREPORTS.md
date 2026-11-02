# How to send a bug report

After launching **NBA Jam Mobile** on Xbox Dev Mode:

1. Open **Device Portal** in a browser (your console IP).
2. Go to **File explorer**.
3. Navigate to the app **LocalState** folder  
   (path looks like `U:\Users\...\Packages\NbaJamMobile_...\LocalState`).
4. Download:
   - **`BUGREPORT.txt`** ← send this first (short, structured)
   - `loader_log.txt` ← optional full log

## What to do with BUGREPORT.txt

- Paste the whole file into chat with Grok, **or**
- Attach it on GitHub Issues on `nba-jam-psv-to-uwp`

It includes app version, folder/files found, ELF load result, symbols, and memory info so fixes can be targeted without guessing from photos.
