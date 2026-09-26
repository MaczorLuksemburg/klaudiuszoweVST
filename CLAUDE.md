# klaudiuszoweVST

AI-made VST plugins for testing and fun.

## Branding
- Company / vendor name is "Maki plugins" (`KLAUD_COMPANY_NAME` in the root `CMakeLists.txt`); plugin headers show it via `JucePlugin_Manufacturer`.
- Never change an existing plugin's `PLUGIN_MANUFACTURER_CODE` or `PLUGIN_CODE`: DAWs use them to find the plugin in saved projects.

## Locations
- Source code lives in this repo (`C:\Users\user\Documents\GitHub\klaudiuszoweVST`), one folder per plugin.
- Built plugins (`.vst3` bundles) go to `C:\gen plugins`. Each plugin's build must copy its output there
  (e.g. JUCE CMake: `COPY_PLUGIN_AFTER_BUILD TRUE` with `VST3_COPY_DIR "C:/gen plugins"`).
  The path contains a space: always quote it.
- Never commit build output; keep `build/` directories out of git.

## Build
- JUCE is fetched by CMake (root `CMakeLists.txt`); each plugin is a subdirectory under `plugins/`, added from the root.
- Shared UI look (FabFilter-inspired, keep it minimal) lives in `shared/KlaudLookAndFeel.h`; reuse it for new plugins.
- `cmake -S . -B build -G "Visual Studio 17 2022" -A x64`, then `cmake --build build --config Release`.

## Targets
- Systems: Windows, macOS, Linux. DAWs: Logic Pro, Cubase, FL Studio, Ableton Live, Reaper.
- Formats: VST3 everywhere + AU on macOS (Logic). Pass `${KLAUD_PLUGIN_FORMATS}` and `${KLAUD_PLUGIN_COPY_ARGS}`
  to every `juce_add_plugin`.
- There is no local Mac/Linux: `.github/workflows/build.yml` builds all three and runs `auval` on macOS.
  Keep code portable (no Windows-only APIs).

## GitHub
- `gh`/git use a fine-grained token scoped to this repo only. Push or open PRs only when asked.
