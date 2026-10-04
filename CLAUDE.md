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

## DynMap (plugins/DynMap)
- Multiband "dynamic mapping" (FabFilter Saturn's band system + Image-Line Maximus's drawn compressor curves).
  Chain: in gain -> Input stage -> band split -> band stages (solo/mute) -> sum -> Master stage -> out gain
  -> clipper -> true-peak limiter -> auto gain -> delta. The global Mix scales every stage's own dry/wet
  (never a dry signal around the whole plugin: min-phase crossovers shift phase, so that would cancel);
  Delta subtracts the dry input run through a copy of the band split, so neutral settings give silence.
- A stage (`Source/Stage.*`) = pre gain -> detector -> level curve (input dB -> output dB, gain = out - in,
  bottom edge = silence) + transient curve (x = attack/tail measure, y = gain) -> x Amount, clamp to max
  boost/cut, smoothing -> saturation or waveshaper mode (curve applied per sample), oversampled -> post, width, mix.
  Followers run on linear amplitude after a short peak hold, so steady tones read their true peak; the
  transient measure is dB(fast^2 / (slow-attack * slow-release)), positive on attacks, negative on tails.
- Bands belong to 12 slots: slot 0 is the lowest band, slots 1-11 own the crossover at their lower edge
  (`b<n>_on/_freq/_slope`); the layout sorts used slots by frequency, so splitting never renumbers bands.
  Minimum-phase crossovers are LR (6/12/24/48 dB/oct) with allpass compensation (sum is flat); linear phase
  designs nested FIRs on a background thread (sum is an exact delayed impulse).
- Sidechain = inputs 3/4 (bus on by default), scaled by `g_scgain`. Per stage `scsrc`: own signal, sidechain
  (bands: the same band of the sidechain, split by its own crossover) or sidechain full (bands: the whole
  sidechain). Sidechain copies are delayed to line up with the audio each stage sees. `g_sclisten` outputs it.
- Level curves can use linear amplitude axes (0..2, i.e. up to +6 dBFS) like Maximus, whose graph is not in dB:
  measured, its default curve gives +10.6 dB to quiet signals. `Curve::gainAt` converts, so the engine only
  ever sees dB tables. Past a linear curve's right edge the output holds its end value (Maximus limits there),
  and level tables reach +36 dB so pre gain can push past the edge. The "Maximus ..." presets were rebuilt
  from screenshots and fitted to renders of the kit (static curves within ~1 dB); its graph is linear 0..2.
- Detector timing modes, measured on the Maximus VST (all within ~0.2 dB of it):
  - Release mode (`rellaw`): Classic (one-pole, shaped by `relshape`), Auto (a dB follower charging over 3x
    and releasing over 6x the release time holds the release up on sustained level only), or Accel 1-8 =
    Maximus's REL curves: after the 10 ms peak window the envelope falls A * (t / release)^p dB.
  - Attack mode (`attlaw`): Classic or Ease 1-8 = Maximus's attack: ~45 % of a gain drop at once, the rest
    through a chain of 1-7 one-poles on the gain (dB). REL 2 (`rel2`, 0 = off) is the same chain on the gain
    coming back up (Maximus's REL2, applied after the first release); its shape follows the Ease number.
  - Maximus knob laws: ATT/SUSTAIN ms = 2 * (501^v - 1), REL/REL2 twice that; master ATT is a lookahead;
    SUSTAIN s ms (peak mode) = DynMap hold s - 10 ms.
- Input low cut (`g_lowcut`, 12 dB/oct, off at the bottom so Init stays bit-exact); width on every stage.
- History view (toggle in the band display's corner): stages push ~5 ms frames (in/out peak, gain range,
  transient gain) through a lock-free FIFO (`History.h`); `HistoryView` drains it on a timer.
- Curve undo/redo (`CurveUndo`, owned by the processor): a finished edit or drag is one step; cleared
  when a preset or project loads. Factory presets are grouped by category (`categoryOf` in Presets.cpp).
- Curves are not parameters: `CurveBank` keeps them in the state tree child `CURVES` and hands baked tables
  to the audio thread through `CurveSlot` (spin lock, try-lock on the audio side).
- Latency is reported exactly (lookahead, oversampling, linear phase, limiter) and changes only with those settings.
- `DynMap_Tests` checks transparency (bit-exact init), latency, crossover flatness, static curve accuracy,
  transient curves, limiter ceiling, sidechain, presets and state; `--bench` measures CPU, `--only <group>`
  runs one group, `--snapshot <dir>` renders the editor. `--kit <wav>` writes a measurement signal and
  `--compare <render.wav> [--preset <name>]` measures another plugin's render of it (static curves, attack/
  release, frequency response) next to DynMap: used to match FL Studio's Maximus, which can't be hosted here.
  `PluginMeasure --plugin <x.vst3|x.dll> [--list] [--set "<name>=<text>|#<index>=[norm]"] [--state-in/-out <f>]
  --render <in> <out>` (Windows) hosts a VST3, or a VST2 through its own minimal loader (`Tools/Vst2Host.h`,
  no Steinberg SDK), to render the kit: OTT Style / Extreme OTT were fitted to Xfer OTT this way (OTT
  detects RMS), the timing modes to Image-Line's Maximus VST (a demo: it ignores loaded state, so only its
  parameters can be set, and its curves stay at the default straight-to-0 dBFS limit).
  `DynMap_Tests --render <in.wav> <out.wav> [--preset] [--set] [--curve]` runs any file through DynMap. The
  OTT presets are fitted on a music-like kit (drums, bass, pads, quiet passages, noise steps, vocal) as well as
  the sine kit, with 4-point monotone curves: tones alone overfit OTT's quirks.

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
- Windows installer: `installer/windows/build-installer.ps1 [-PluginDir] [-Version]` (Inno Setup 6, installed with
  winget) packs the .vst3 bundles found in `C:\gen plugins` into `installer/windows/output/MakiPlugins-Setup-<v>.exe`
  (installs to Common Files\VST3, per-plugin choice, uninstaller). Not built by CI yet.
- Licence: AGPLv3 (`LICENSE`), required because JUCE is used under its open-source licence.

## GitHub
- `gh`/git use a fine-grained token scoped to this repo only. Push or open PRs only when asked.
