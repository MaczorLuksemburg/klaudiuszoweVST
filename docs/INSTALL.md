## Maki plugins: free VST3 / AU plugins

Download the zip for your system below, then follow the steps for it.

| Plugin | What it does |
|---|---|
| **DynMap** | Multiband dynamics you draw: input/output level curves (Maximus style) and transient curves on up to 12 bands, saturation, clipper and true-peak limiter |
| **MSC** | Multistage stereo control: band split, dynamic pan, Haas, Juno-style chorus, mid/side image |
| **StereoScale** | Left / right / mid / side gain, 0–200 % |

### Windows (`MakiPlugins-Windows.zip`)
1. Unzip and copy the `.vst3` folders to `C:\Program Files\Common Files\VST3`.
2. Rescan plugins in your DAW.

The first time, Windows may warn that the files are from the internet; that's expected for free, unsigned plugins.

### macOS (`MakiPlugins-macOS.zip`), macOS 11 or newer, Apple Silicon and Intel
1. Unzip.
2. Copy the `.component` files to `~/Library/Audio/Plug-Ins/Components` (for Logic Pro)
   and the `.vst3` files to `~/Library/Audio/Plug-Ins/VST3` (Cubase, Ableton, FL Studio, Reaper).
   In Finder, press **Cmd + Shift + G** and paste the path to get there.
3. The plugins aren't signed by Apple, so macOS blocks them until you clear the download flag.
   Open **Terminal** and run:

   ```
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components ~/Library/Audio/Plug-Ins/VST3
   ```

4. Restart your DAW. In Logic Pro, open **Logic Pro > Settings > Plug-in Manager** and rescan if a plugin doesn't show up.

### Linux (`MakiPlugins-Linux.zip`), x86-64, Ubuntu 22.04-era or newer
1. Unzip and copy the `.vst3` folders to `~/.vst3/` (create it if needed).
2. Rescan plugins in your DAW (for example Reaper or Bitwig).

---
Free and open source under the GNU AGPLv3. Source code: https://github.com/MaczorLuksemburg/klaudiuszoweVST
