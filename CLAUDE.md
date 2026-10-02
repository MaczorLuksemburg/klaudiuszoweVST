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
- The shared look takes a `klaud::Palette` per plugin (StereoScale uses the default grey, MSC a navy palette
  with one accent colour per module).

## Testing
- Plugins with a `Tests/` folder build an offline test program (`<Plugin>_Tests`); `ctest --test-dir build -C Release`
  runs the DSP checks, and CI runs them on all three OSes.
- `MSC_Tests --snapshot <folder>` also renders the editor to PNG: use it to check the UI after layout changes.

## MSC (plugins/MSC)
- Chain: input band split (processed = filter(x), unprocessed = x - filter(x), so it always sums back to x)
  -> dynamic pan -> Haas -> Juno-style chorus -> image, then the unprocessed band is added back.
- Modules default off; touching any control in a module switches it on. Off modules are skipped (15 ms fades).
- Dynamic pan: modulator = filter(mod source, read from the plugin input, not the input module's band)
  -> optional comp (makeup 0-100 %) -> x amount -> mod clip = p, then side += mid * p (mono: L = x(1-p),
  R = x(1+p)). Mid is untouched, so the effect cancels exactly in mono: this is the module's core promise,
  keep it. Never clip or otherwise process L/R separately there; the clipper shapes p only. Stereo peaks can
  rise up to +6 dB at |p| = 1.
- Parameter IDs in `Parameters.h` are saved in projects and presets: never rename or remove them.

## FloorMatch (plugins/FloorMatch)
- Made for friends doing film dialogue: all boom takes on one track, background noise brought to one target
  level (A-weighted, dB where a full-scale sine is 0) and optionally one colour; dialogue must stay untouched.
  Noise is only matched, never removed completely; "Room tone fill" adds shaped noise to takes below the target.
  Its only job is balancing backgrounds between takes (the friends follow it with a peak compressor, no makeup).
  Noise events inside a take (close cloth rustle, a passing car) are left alone by design: only sharp steps
  (< 150 ms) seen by at least a quarter of the bands count as cuts; everything else holds the background.
- DSP is in `Source/Engine.*` (STFT ~21 ms, 75 % overlap). Noise floor per bin = robust truncated mean over a
  window before and after each frame (lookahead, reported as latency); per-band cut detection picks the side
  that belongs to the current take, frames > 6 dB over the estimate are labelled speech and skipped, long
  phrases hold the last estimate, rises without a cut are rate-limited. Gain = sqrt(beta) + (1 - sqrt(beta)) *
  speech gain (MMSE-LSA + presence, lookahead attack, 50 ms release).
- Learned profile (61 bands of 1/6 octave, dB per Hz) is saved in the state tree property `profile`, not a parameter.
- Display: 1/12-octave curves (+ live input spectrum, toggled by the state property `showInputSpectrum`) and a
  ~15 s timeline from `Engine::TimelineColumn`s (~50 ms each): background in/out, target, input level, cut
  markers (where the estimate steps across most bands; set up to `timelineSettle` columns late, so the
  processor only copies settled columns), event-held shading and max-reduction-reached flags.
- `FloorMatch_Tests` measures everything on synthetic takes (formant "speech" over different noises joined with
  cuts); `--quality` isolates speech and noise via shadow channels, `--bench` measures CPU,
  `--render in.wav out.wav [--target dB] [--quietest] ...` processes real recordings for listening tests,
  `--trace in.wav <from s> <to s> [lookahead] [--bin k]` prints the estimator's per-band decisions over time.
  Keep these numbers passing when tuning: the thresholds encode the quality bar.

## Targets
- Systems: Windows, macOS, Linux. DAWs: Logic Pro, Cubase, FL Studio, Ableton Live, Reaper.
- Formats: VST3 everywhere + AU on macOS (Logic). Pass `${KLAUD_PLUGIN_FORMATS}` and `${KLAUD_PLUGIN_COPY_ARGS}`
  to every `juce_add_plugin`.
- There is no local Mac/Linux: `.github/workflows/build.yml` builds all three and runs `auval` on macOS.
  Keep code portable (no Windows-only APIs).

## Releases
- Pushing a `v*` tag (e.g. `v1.0.0`) on `main` builds all three systems and publishes a GitHub Release with
  `MakiPlugins-<OS>.zip` files (plugins + INSTALL.md + LICENSE.txt); `docs/INSTALL.md` is the release notes.
  Publishing a release from GitHub's Releases page (which creates the tag) also works: the job then attaches
  the zips to that release and appends the install guide under the notes written there.
  Keep it up to date when plugins are added. Bump each plugin's `VERSION` in its CMakeLists before tagging.
- Licence: AGPLv3 (`LICENSE`), required because JUCE is used under its open-source licence.

## GitHub
- `gh`/git use a fine-grained token scoped to this repo only. Push or open PRs only when asked.
