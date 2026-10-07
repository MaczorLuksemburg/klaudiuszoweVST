# klaudiuszoweVST

AI-made audio plugins by **Maki Plugins**, for testing and fun. Free, open source, VST3 on Windows, macOS
and Linux, plus AU on macOS for Logic Pro.

| Plugin | What it does |
|---|---|
| **FloorMatch** | Brings the background noise of dialogue takes (e.g. boom recordings) to one level and colour, leaving the dialogue untouched |
| **MSC** | Multistage stereo control: band split, dynamic pan, Haas, Juno-style chorus, mid/side image |
| **StereoScale** | Left / right / mid / side gain, 0–200 % |

## Download
Get the latest version from the [Releases page](https://github.com/MaczorLuksemburg/klaudiuszoweVST/releases).
Installation steps for each system are in [docs/INSTALL.md](docs/INSTALL.md) and on every release page.

## Building
Needs CMake 3.22+ and a C++20 compiler (Visual Studio 2022 on Windows). JUCE is downloaded automatically.

```
cmake -S . -B build
cmake --build build --config Release
```

Pushing a tag like `v1.0.0` builds all three systems on GitHub and publishes a release with the zips.

## Licence
[GNU AGPLv3](LICENSE). Built with [JUCE](https://juce.com), used under its AGPLv3 licence.
