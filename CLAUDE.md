# klaudiuszoweVST

AI-made VST plugins for testing and fun.

## Branding
- Company / vendor name is "Maki Plugins" (`KLAUD_COMPANY_NAME` in the root `CMakeLists.txt`); plugin headers show it via `JucePlugin_Manufacturer`.
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
  -> dynamic pan -> Haas -> Juno-style chorus -> dimension -> image, then the unprocessed band is added back.
- Modules default off; touching any control in a module switches it on. Off modules are skipped (15 ms fades).
- Dynamic pan: modulator = filter(mod source, read from the plugin input, not the input module's band)
  -> optional comp (makeup 0-100 %) -> x amount -> mod clip = p, then side += mid * p (mono: L = x(1-p),
  R = x(1+p)). Mid is untouched, so the effect cancels exactly in mono: this is the module's core promise,
  keep it. Never clip or otherwise process L/R separately there; the clipper shapes p only. Stereo peaks can
  rise up to +6 dB at |p| = 1.
- Dimension = Xfer's DimensionExpander (the Dimension half of Serum's Hyper/Dimension), measured by deconvolving
  Reaper renders of noise (`dsp::dimension` in `DSP.h`): mid -> 4 whole-sample delays (15 ms + 99.5 ms x size,
  then x1.18 each) -> gains 0.500 sin, -0.445 cos, -0.484 sin, +0.470 cos of one 0.4547 Hz LFO -> x mix -> added
  to the side only (mono sum untouched, dry never attenuated). Taps stay on whole samples (interpolating dulls the
  highs: -4.7 instead of the measured -3.47 dB side on noise); a Size change glides between them. `MSC_Tests`
  checks the taps, the -3.47 dB level and the mono sum. The `dm_*` group was added after 1.0, so it is appended
  last in the layout (new parameters and factory presets always go at the end).
- `PluginMeasure` can't reproduce DimensionExpander (its side comes out ~40 dB low, delays 4.5x short, even with
  host time info), so the measurement used renders from Reaper instead.
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
  and level tables reach +36 dB so pre gain can push past the edge. The "Linear ..." presets are Maximus factory
  presets read from their .fst files (knobs + curves; curve slots low 0, mid 2, high 4, master 6; bend on a
  segment's end point, ~1.65x ours) and corrected against FL Studio renders of six mastered songs (per-band
  loudness within ~0.7-1.3 dB) plus the sine kit for transients. Maximus PRE: amplitude = (e^(3.9 n) - 1) /
  (e^1.95 - 1), n = stored / 2000; POST: amplitude = stored / 1000. Bands in its RMS sustain mode let
  transients through like a classic attack at ~0.7-0.8x ATT, not like Ease.
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
- User-facing text never needs Maximus knowledge: only the "Maximus" detector style is named after it; tooltips,
  descriptions and preset names (e.g. "Lift & Limit Master", "Linear ..." in the "Linear Curves" section) don't
  mention it. Code comments and docs may.
- Detector styles (`DetectorStyleBank` in Components): 12 built in + Custom 1-3, saved to
  `<user app data>/Maki plugins/DynMap/DetectorStyles.xml` and re-read by every instance when the file changes.
  Simple view: style grid + MAX BOOST/MAX CUT/DET HP (styles never set those); Advanced: every knob, with a style
  menu (arrows, apply, "Save to ... (Custom n)" then a name prompt, "Reset custom detection styles..." with a
  tick-list confirm dialog) in the panel header.
- Grid snapping (3 dB steps): Shift always snaps; the grid icon in each map's header (and the last item of the
  curve right-click menus) switches snapping on for mouse-only use. One UI state property `snapToGrid`, kept
  across preset loads like the other `uiProperties`.
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

## WaveRef (plugins/WaveRef)
- Reference A/B on the master bus: "Mix" = the DAW's signal, "Ref" = a loaded file (8 slots, WAV/AIFF/FLAC/OGG/MP3
  through JUCE, decoded once and resampled to the host rate with a windowed sinc, kept in RAM as float: about 90 MB
  per 4 minutes). Both are always analysed; A/B only chooses what is heard. Zero latency, bit-exact when A is selected
  and monitoring is off (`WaveRef_Tests` checks it).
- Switching is a 5 ms crossfade (short so a kick keeps its attack). `RefPlayer` plays the loop with voices: every jump
  (loop wrap, restart, slot change) starts a new voice and fades the old one, so nothing clicks. B with an empty slot is silent.
- Loudness: BS.1770 K-weighting + gating (`Loudness.h`). The ref loop's loudness comes from precomputed 100 ms energies
  (`TrackData::subEnergies`), the mix's from `LiveMeter` (3 s ungated or 30 s gated, chosen by `lm_mode`). Match gain =
  mix - ref loop, clamped to +-40 dB and applied through a 250 ms ramp, on top of the manual trim. Both analysers see the
  matched level.
- Spectrum (`Spectrum.h`): mid = (L+R)/2 and side = (L-R)/2 of mix and ref (4 curves). The audio thread only fills FIFOs
  (and only while an editor is open); the editor's timer runs the FFTs. Levels are noise-normalised, so white noise of RMS 1
  reads 0 dB at any FFT size. Curves are averaged in the power domain (speed = time constant), smoothed over 1/N octave
  (mean power) or, with smoothing off, the loudest bin between neighbouring points, so a sine never falls between points.
- Tracks reach the audio thread through `Slot::pending` -> `Slot::active` (raw pointers; the message thread keeps the owners
  and frees old ones in `collectGarbage()` only 2 s after retiring them and once the audio thread has moved on).
  Loop positions are kept in seconds in the state tree (`SLOTS`), the audio thread reads sample atomics.
- Editor settings (colours, FFT size, speed, smoothing, tilt, range...) live in the state tree child `UI` and in
  `<user app data>/Maki plugins/WaveRef/Settings.xml`, which new instances start from. Tests have no message loop: they
  use `loadFileBlocking`, `hasLoadsInFlight()` + `drainFinishedLoads()` and `WaveRefEditor::refresh()` (what the timers do).
- Parameter IDs in `Parameters.h` are saved in projects: never rename or remove them.
- Reference transport (`ref_play`, `ref_mode`): Free = own transport (play/stop holds the position, 4 cue points per slot,
  double-click on the waveform plays from there, space bar, Q W E R). A seek goes through `seekRequest`; `RefPlayer` keeps the
  loop rules (before the loop it plays on into it, after it it runs to the file end and comes back; `inLoop` per voice).
  Host time = reference time = host time + `sync_offset` (no looping, voices re-aimed when more than 64 samples off).
  Beats = the reference's beat grid (`SlotInfo::bpm`/`downbeat`, seconds) follows the host's PPQ: ref time =
  downbeat + (ppq + `beat_offset`) x 60 / refBpm + `nudge_ms`. At equal tempo it uses the plain player (exact), otherwise
  `StretchPlayer` (Rubber Band R3, real-time, vendored through FetchContent as a single-file build; GPL v2+ so it is
  AGPL-compatible): it is primed with the start pad / start delay at the expected position, bends its time ratio by up to
  3 % to hold the position, and re-primes (cross-faded, 3 voices) when more than 40 ms off. Key lock off = varispeed. Measured: clicks
  land within ~7 ms of the host's beats at +-25 % tempo; about 8-12 % of one core while stretching.
- `BeatAnalysis.h`: tempo + bar start for a loaded file (onset curve, autocorrelation with a 125 BPM prior, fine tempo/phase
  search on the whole file, grid nudged onto the real attacks). Fast tempos can come out at half speed: the UI has /2, x2, tap.
- Monitoring order after the A/B crossfade: delta (mix - ref, matched) -> side or mono -> listening band. `side` wins over `mono`.
  The listening band can be dragged out on the spectrum (create, resize the edges, move, double-click clears).
- `Stereo.h`: smoothed correlation and width (side share) of mix and ref, plus decimated points for the goniometer.
- Parameter and state keys added after v1 (`ref_play`, `ref_mode`, `sync_offset`, `beat_offset`, `nudge_ms`, `key_lock`,
  `side`, `delta`; slot attributes `cue0-3`, `bpm`, `downbeat`) are saved: never rename or remove them.

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
- One version for the whole pack: `project(klaudiuszoweVST VERSION x.y.z)` in the root `CMakeLists.txt`
  (`KLAUD_VERSION`, passed to every `juce_add_plugin`). To release: bump it and the version line at the top of
  `docs/INSTALL.md`, merge to `main`, push the tag `v<same version>` (CI fails if the tag doesn't match).
- Pushing a `v*` tag on `main` builds all three systems and publishes a GitHub Release with
  `MakiPlugins-<OS>.zip` files (plugins + INSTALL.md + LICENSE.txt), `MakiPlugins-macOS-Installer.pkg` and
  `MakiPlugins-Windows-Setup.exe`; `docs/INSTALL.md` is the release notes. Asset names carry no version so
  `releases/latest/download/<name>` links always point at the newest one (README and INSTALL.md use them).
  Publishing a release from GitHub's Releases page (which creates the tag) also works: the job then attaches
  the zips to that release and appends the install guide under the notes written there.
  Keep it up to date when plugins are added (also the plugin lists in both installers).
- Windows installer: `installer/windows/build-installer.ps1 [-PluginDir] [-Version]` (Inno Setup 6, installed with
  winget) packs the .vst3 bundles found in `C:\gen plugins` into `installer/windows/output/MakiPlugins-Setup-<v>.exe`
  (installs to Common Files\VST3, per-plugin choice, uninstaller). CI builds it from its own build.
- macOS installer: `installer/macos/build-pkg.sh <plugin dir> <version> <out.pkg>` (pkgbuild + productbuild, CI only)
  installs every plugin's VST3 + AU into `/Library/Audio/Plug-Ins` for all users (shared studio Macs), one choice
  per plugin. Unsigned: only the .pkg needs "Open Anyway" once; Installer-written files carry no quarantine flag.
  Bundles are marked non-relocatable (otherwise Installer updates a hand-copied ~/Library copy instead). CI
  installs the .pkg on the runner and runs auval on the installed AUs.
- Licence: AGPLv3 (`LICENSE`), required because JUCE is used under its open-source licence.

## GitHub
- `gh`/git use a fine-grained token scoped to this repo only. Push or open PRs only when asked.
