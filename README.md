# klaudiuszoweVST

AI-made audio plugins by **Maki Plugins**, for testing and fun. Free, open source, VST3 on Windows, macOS
and Linux, plus AU on macOS for Logic Pro.

| Plugin | What it does |
|---|---|
| **DynMap** | Multiband dynamics you draw: input/output level curves (Maximus style) and transient curves on up to 12 bands, saturation, clipper and true-peak limiter |
| **FloorMatch** | Brings the background noise of dialogue takes (e.g. boom recordings) to one level and colour, leaving the dialogue untouched |
| **MSC** | Multistage stereo control: band split, dynamic pan, Haas, Juno-style chorus, dimension, mid/side image |
| **WaveRef** | Reference A/B switcher: 16 loaded tracks that are remembered for new songs, loop, cues, loudness match, mid/side spectrum and stereo view of mix and reference, tempo and host sync with time-stretch, delta/side/mono listening, blind test |
| **StereoScale** | Left / right / mid / side gain, 0–200 % |

## Download
- **Mac** (Logic, Ableton, Cubase, FL Studio, Reaper): [MakiPlugins-macOS-Installer.pkg](https://github.com/MaczorLuksemburg/klaudiuszoweVST/releases/latest/download/MakiPlugins-macOS-Installer.pkg)
- **Windows**: [MakiPlugins-Windows-Setup.exe](https://github.com/MaczorLuksemburg/klaudiuszoweVST/releases/latest/download/MakiPlugins-Windows-Setup.exe)
- **Linux** and plain zips: the [Releases page](https://github.com/MaczorLuksemburg/klaudiuszoweVST/releases/latest)

The plugins are free but not paid-signed, so macOS and Windows ask once before the installer runs: the
step-by-step guide is in [docs/INSTALL.md](docs/INSTALL.md) and on every release page.

## Building
Needs CMake 3.22+ and a C++20 compiler (Visual Studio 2022 on Windows). JUCE is downloaded automatically.

```
cmake -S . -B build
cmake --build build --config Release
```

The pack version is set once in the root `CMakeLists.txt`. Pushing a matching tag like `v1.0.3` builds all
three systems on GitHub and publishes a release with the zips and the Mac and Windows installers.

## Licence
[GNU AGPLv3](LICENSE). Built with [JUCE](https://juce.com), used under its AGPLv3 licence. WaveRef also contains the [Rubber Band](https://breakfastquay.com/rubberband/) time-stretching library (GPL v2 or later).
