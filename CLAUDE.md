# klaudiuszoweVST

AI-made VST plugins for testing and fun.

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

## GitHub
- `gh`/git use a fine-grained token scoped to this repo only. Push or open PRs only when asked.
