## Maki Plugins: free VST3 / AU plugins

Version 1.0.3. Pick your system below and follow its steps.

| Plugin | What it does |
|---|---|
| **DynMap** | Multiband dynamics you draw: input/output level curves (Maximus style) and transient curves on up to 12 bands, saturation, clipper and true-peak limiter |
| **FloorMatch** | Brings the background noise of dialogue takes (e.g. boom recordings) to one level and colour, leaving the dialogue untouched |
| **MSC** | Multistage stereo control: band split, dynamic pan, Haas, Juno-style chorus, mid/side image |
| **WaveRef** | Reference A/B switcher: 16 loaded tracks that are remembered for new songs, loop, cues, loudness match, mid/side spectrum and stereo view of mix and reference, tempo and host sync with time-stretch, delta/side/mono listening, blind test |
| **StereoScale** | Left / right / mid / side gain, 0–200 % |

### Mac (macOS 11 or newer, Apple Silicon and Intel)

Works in Logic Pro and GarageBand (Audio Unit), and in Ableton Live, Cubase, FL Studio and Reaper (VST3).
The plugins are installed for **every user of the Mac**, so on a shared studio computer one install is enough.
You need the Mac's administrator password.

**1. Download** [MakiPlugins-macOS-Installer.pkg](https://github.com/MaczorLuksemburg/klaudiuszoweVST/releases/latest/download/MakiPlugins-macOS-Installer.pkg)
(it lands in your Downloads folder).

**2. Allow it to open.** The plugins are free and not paid-signed by Apple, so macOS stops the installer the
first time:

- **macOS 15 Sequoia or newer:** double-click the file. When macOS says it was *not opened*, click **Done**.
  Open **System Settings > Privacy & Security**, scroll down to *Security*, and next to
  *"MakiPlugins-macOS-Installer.pkg" was blocked* click **Open Anyway**. Enter the password and click
  **Open Anyway** again.
- **macOS 14 or older:** right-click (or Control-click) the file, choose **Open**, then **Open** again.

**3. Install.** Quit your DAW first. In the installer click **Continue**, **Agree** to the licence, then
**Install** and enter the administrator password. To install only some of the plugins, click **Customize** on
the *Installation Type* page.

**4. Open your DAW.** The plugins are listed under **Maki Plugins**.

| DAW | If a plugin doesn't show up |
|---|---|
| Logic Pro | **Logic Pro > Settings > Plug-in Manager**, select the plugin, click **Reset & Rescan Selection** |
| Ableton Live | **Settings > Plug-ins**: switch on **Use VST3 Plug-in System Folders** (and the **Audio Units** switches), click **Rescan** |
| FL Studio | **Options > Manage plugins > Find installed plugins** |
| Reaper | **Settings > Plug-ins > VST > Re-scan** |
| Cubase | Restart Cubase; check **Studio > VST Plug-in Manager** |

**Updating:** download the newer installer and run it again; it replaces the old versions.

**Where the files go:** `/Library/Audio/Plug-Ins/Components` (Audio Unit) and `/Library/Audio/Plug-Ins/VST3` (VST3).

**Uninstalling:** open **Terminal** and paste:

```
sudo rm -rf /Library/Audio/Plug-Ins/VST3/{DynMap,FloorMatch,MSC,StereoScale,WaveRef}.vst3 /Library/Audio/Plug-Ins/Components/{DynMap,FloorMatch,MSC,StereoScale,WaveRef}.component
```

<details>
<summary>Without the installer (zip, no administrator password, only for your own user account)</summary>

1. Download `MakiPlugins-macOS.zip` from the release page and unzip it.
2. Copy the `.component` files to `~/Library/Audio/Plug-Ins/Components` and the `.vst3` files to
   `~/Library/Audio/Plug-Ins/VST3`. In Finder, press **Cmd + Shift + G** and paste the path to get there.
3. Plugins copied by hand keep macOS's download flag, which blocks them. Open **Terminal** and run:

   ```
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components ~/Library/Audio/Plug-Ins/VST3
   ```

4. Restart your DAW.

Don't use both ways on one Mac: if you switch to the installer, delete these copies from your user folder
first, or the DAW lists every plugin twice.
</details>

**If macOS says a plugin "is damaged" or "can't be opened":** open **Terminal**, run this line, then restart the DAW:

```
sudo xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/Components /Library/Audio/Plug-Ins/VST3
```

### Windows (64-bit)

**Installer:** download [MakiPlugins-Windows-Setup.exe](https://github.com/MaczorLuksemburg/klaudiuszoweVST/releases/latest/download/MakiPlugins-Windows-Setup.exe),
close your DAW and run it. If Windows shows *"Windows protected your PC"*, click **More info > Run anyway**
(expected for free, unsigned plugins). It installs into `C:\Program Files\Common Files\VST3`, which every DAW
scans, and adds an uninstaller to Windows' **Apps** list.

**Or by hand:** unzip `MakiPlugins-Windows.zip`, copy the `.vst3` folders to `C:\Program Files\Common Files\VST3`
and rescan plugins in your DAW.

### Linux (x86-64, Ubuntu 22.04-era or newer)

1. Unzip `MakiPlugins-Linux.zip` and copy the `.vst3` folders to `~/.vst3/` (create it if needed).
2. Rescan plugins in your DAW (for example Reaper or Bitwig).

---
Free and open source under the GNU AGPLv3. WaveRef includes the Rubber Band library (GPL v2 or later). Source code: https://github.com/MaczorLuksemburg/klaudiuszoweVST
