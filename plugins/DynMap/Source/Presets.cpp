#include "Presets.h"
#include <map>

namespace dynmap
{
namespace
{
    const juce::Identifier presetNameId { "presetName" };
    const juce::StringArray uiProperties { "uiWidth", "selectedStage", "detectorAdvanced", "historyView" };   // kept when presets load
    constexpr const char* fileExtension = ".dynmappreset";

    struct CurveDef
    {
        int stage;
        CurveKind kind;
        int presetIndex = 0;        // Curve::preset index, used when custom is empty
        juce::String custom {};     // "x,y[,segment,tension];..."
    };

    struct FactoryPreset
    {
        juce::String name;
        std::vector<std::pair<juce::String, float>> values;   // real units; bools 0/1, choices by index
        std::vector<CurveDef> curves;
    };

    using Values = std::vector<std::pair<juce::String, float>>;

    std::pair<juce::String, float> sp (int stage, const char* name, float value)
    {
        return { stageParamId (stage, name), value };
    }

    // Turns band slot `slot` (1-11) on with its crossover at `hz`.
    void addBand (Values& v, int slot, float hz, int slope = slope24)
    {
        v.push_back (sp (bandStage (slot), ids::bandOn, 1.0f));
        v.push_back (sp (bandStage (slot), ids::freq, hz));
        v.push_back (sp (bandStage (slot), ids::slope, (float) slope));
    }

    void setTimes (Values& v, int stage, float attack, float release)
    {
        v.push_back (sp (stage, ids::attack, attack));
        v.push_back (sp (stage, ids::release, release));
    }

    // The detector styles' settings (see detectorStyles() in Components.cpp).
    void glueTiming (Values& v, int s)
    {
        setTimes (v, s, 10.0f, 120.0f);
        v.push_back (sp (s, ids::relShape, 40.0f));
        v.push_back (sp (s, ids::rms, 5.0f));
        v.push_back (sp (s, ids::lookahead, 2.0f));
        v.push_back (sp (s, ids::smooth, 1.0f));
        v.push_back (sp (s, ids::trTime, 50.0f));
        v.push_back (sp (s, ids::release2, 600.0f));
    }

    void masterTiming (Values& v, int s)
    {
        setTimes (v, s, 1.0f, 120.0f);
        v.push_back (sp (s, ids::attLaw, 2.0f));
        v.push_back (sp (s, ids::relLaw, (float) ids::accelRelease (3)));
        v.push_back (sp (s, ids::release2, 400.0f));
        v.push_back (sp (s, ids::lookahead, 3.0f));
    }

    void autoTiming (Values& v, int s, float attack = 3.0f, float release = 100.0f)
    {
        setTimes (v, s, attack, release);
        v.push_back (sp (s, ids::relLaw, (float) ids::relLawAuto));
        v.push_back (sp (s, ids::lookahead, 2.0f));
    }

    void vocalTiming (Values& v, int s)
    {
        setTimes (v, s, 15.0f, 150.0f);
        v.push_back (sp (s, ids::attLaw, 2.0f));
        v.push_back (sp (s, ids::relShape, 40.0f));
        v.push_back (sp (s, ids::rms, 10.0f));
        v.push_back (sp (s, ids::smooth, 1.0f));
        v.push_back (sp (s, ids::trTime, 50.0f));
        v.push_back (sp (s, ids::release2, 700.0f));
    }

    // Menu sections, in order; presets not listed here go under "Basics".
    const char* const categoryOrder[] { "Basics", "Mastering", "Mix & Bus", "Vocals", "Drums", "Sidechain",
                                        "OTT & Upward", "Sound Design", "Maximus" };

    juce::String categoryOf (const juce::String& name)
    {
        static const std::map<juce::String, juce::String> categories {
            { "Loud Master", "Mastering" }, { "Transparent Master", "Mastering" }, { "Mastering Glue", "Mastering" },
            { "Loud & Calm Master", "Mastering" },
            { "Mix Bus Glue", "Mix & Bus" }, { "Drum Bus Glue", "Mix & Bus" }, { "NY Compression", "Mix & Bus" },
            { "Mid/Side Glue", "Mix & Bus" }, { "Bass Control", "Mix & Bus" }, { "Sub Glue", "Mix & Bus" },
            { "Vocal Leveler", "Vocals" }, { "Vocal Presence", "Vocals" }, { "Smooth De-esser", "Vocals" },
            { "Narrow De-esser", "Vocals" },
            { "Drum Punch", "Drums" }, { "Multiband Drum Punch", "Drums" }, { "Drum Snap Extreme", "Drums" },
            { "Breakbeat Slam", "Drums" }, { "Transient Softener", "Drums" }, { "Tight Drums", "Drums" },
            { "Room Bloom", "Drums" }, { "Hard Gate", "Drums" },
            { "Sidechain Pump", "Sidechain" }, { "Kick Ducks Bass", "Sidechain" }, { "Multiband Sidechain Duck", "Sidechain" },
            { "OTT Style", "OTT & Upward" }, { "Extreme OTT", "OTT & Upward" }, { "Upward Glue", "OTT & Upward" },
            { "Stair Steps", "Sound Design" }, { "Fold Waveshaper", "Sound Design" }, { "Soft Clip Waveshaper", "Sound Design" },
            { "Inverted Dynamics", "Sound Design" }, { "Multiband Mangle", "Sound Design" }, { "Gated Mid", "Sound Design" },
            { "Dirty Behind Clean", "Sound Design" }, { "Lo-Fi Crush", "Sound Design" },
            { "Maximus Default", "Maximus" }, { "Maximus Punchy Drums", "Maximus" }, { "Maximus Max Loudness", "Maximus" } };

        const auto it = categories.find (name);
        return it != categories.end() ? it->second : juce::String ("Basics");
    }

    int categoryRank (const juce::String& category)
    {
        for (int i = 0; i < (int) std::size (categoryOrder); ++i)
            if (category == categoryOrder[i])
                return i;
        return 0;
    }

    const std::vector<FactoryPreset>& factoryPresets()
    {
        static const std::vector<FactoryPreset> presets = []
        {
            std::vector<FactoryPreset> list;
            constexpr auto level = CurveKind::level;
            constexpr auto transient = CurveKind::transient;
            const int b0 = bandStage (0), b1 = bandStage (1), b2 = bandStage (2), b3 = bandStage (3), b4 = bandStage (4);

            list.push_back ({ "Init", {}, {} });

            // Xfer OTT, measured: test material was rendered through the real plugin (PluginMeasure) and each
            // band got a simple 4-point curve that only rises, fitted to both a music-like kit (drums, bass,
            // pads, quiet passages, noise steps, vocal: per-band loudness within ~1.5 dB) and the sine kit
            // (~1 dB). Fitting tones alone gave exact but bumpy curves that were 2-5 dB off on music. OTT
            // detects RMS (5 ms here; 2 ms lookahead matches it better still). Bands at 88.3 Hz and 2.5 kHz; times keep OTT's band ratios. Like the
            // original it pins loud material at about -25 dB per band: turn Out up to taste, Mix down for the
            // usual "OTT at 30 %".
            auto ottBands = [&] (Values& v)
            {
                addBand (v, 1, 88.3f);
                addBand (v, 2, 2500.0f);
                setTimes (v, b0, 6.4f, 42.0f);
                setTimes (v, b1, 3.0f, 42.0f);
                setTimes (v, b2, 1.8f, 20.0f);
                for (int s : { b0, b1, b2 })
                {
                    v.push_back (sp (s, ids::maxBoost, 48.0f));
                    v.push_back (sp (s, ids::rms, 5.0f));
                    v.push_back (sp (s, ids::lookahead, 3.0f));   // 2 ms: hits after silence don't get the full upward boost
                }
            };

            {
                Values v;
                ottBands (v);
                list.push_back ({ "OTT Style", v,
                                  { { b0, level, 0, "-72,-34.25;-44.34,-31.67;-41.34,-25.68;12,-23.25" },
                                    { b1, level, 0, "-72,-43.54;-47.04,-36.44;-34.27,-26.16;12,-25.49" },
                                    { b2, level, 0, "-72,-38.57;-46.98,-32.14;-37.71,-26.57;12,-25.65" } } });
            }

            // OTT with upward and downward strength at 200 %, fitted the same way.
            {
                Values v;
                ottBands (v);
                list.push_back ({ "Extreme OTT", v,
                                  { { b0, level, 0, "-72,-24.45;-9.05,-24.19;-6.05,-21.8;12,-16.86" },
                                    { b1, level, 0, "-72,-33.14;-36.3,-30.12;-33.3,-26.6;12,-24.52" },
                                    { b2, level, 0, "-72,-26.06;-38.14,-26.04;-35.14,-25.21;12,-25.08" } } });
            }

            {
                Values v;
                addBand (v, 1, 150.0f);
                addBand (v, 2, 2000.0f);
                addBand (v, 3, 8000.0f);
                for (int s : { b0, b1, b2, b3 })
                    setTimes (v, s, 20.0f, 150.0f);
                v.push_back ({ ids::outGain, 4.0f });
                v.push_back ({ ids::clip, (float) clipSoft });
                v.push_back ({ ids::limiter, 1.0f });
                v.push_back ({ ids::ceiling, -0.3f });
                v.push_back ({ ids::limRel, 60.0f });
                v.push_back ({ ids::quality, 2.0f });
                const juce::String gentle = "-72,-72;-18,-18;12,-4";
                list.push_back ({ "Loud Master", v, { { b0, level, 0, gentle }, { b1, level, 0, gentle },
                                                      { b2, level, 0, gentle }, { b3, level, 0, gentle } } });
            }

            list.push_back ({ "Drum Punch", { sp (inputStage, ids::trTime, 30.0f) }, { { inputStage, transient, 1 } } });

            // Punch where it matters and fuller bodies: the kick and body bands get a little sustain and
            // gentle upward compression, the snap and air bands get sharp transient boosts.
            {
                Values v;
                addBand (v, 1, 120.0f);
                addBand (v, 2, 1200.0f);
                addBand (v, 3, 6000.0f);
                v.push_back (sp (b0, ids::trTime, 50.0f));
                v.push_back (sp (b1, ids::trTime, 40.0f));
                v.push_back (sp (b2, ids::trTime, 25.0f));
                v.push_back (sp (b3, ids::trTime, 15.0f));
                for (int s : { b0, b1 })
                {
                    setTimes (v, s, 30.0f, 250.0f);
                    v.push_back (sp (s, ids::maxBoost, 10.0f));
                }
                v.push_back ({ ids::clip, (float) clipSoft });
                v.push_back ({ ids::limiter, 1.0f });
                list.push_back ({ "Multiband Drum Punch", v,
                                  { { b0, transient, 0, "-24,2;-8,0;0,-3;10,5;24,7" },
                                    { b0, level, 0, "-72,-60;-30,-30;12,12" },
                                    { b1, transient, 0, "-24,4;-10,2;0,0;12,4;24,5" },
                                    { b1, level, 0, "-72,-54;-36,-36;12,12" },
                                    { b2, transient, 7 },
                                    { b3, transient, 0, "-24,-2;0,-3;6,6;24,8" } } });
            }

            list.push_back ({ "Drum Snap Extreme",
                              { sp (inputStage, ids::trTime, 20.0f), sp (inputStage, ids::satType, (float) satTape), sp (inputStage, ids::drive, 6.0f) },
                              { { inputStage, transient, 2 } } });

            list.push_back ({ "Transient Softener", { sp (inputStage, ids::trTime, 25.0f) }, { { inputStage, transient, 3 } } });
            list.push_back ({ "Tight Drums", { sp (inputStage, ids::trTime, 60.0f) }, { { inputStage, transient, 4 } } });
            list.push_back ({ "Room Bloom", { sp (inputStage, ids::trTime, 80.0f) }, { { inputStage, transient, 5 } } });

            {
                Values v;
                addBand (v, 1, 200.0f);
                addBand (v, 2, 3000.0f);
                for (int s : { b0, b1, b2 })
                {
                    setTimes (v, s, 30.0f, 400.0f);
                    v.push_back (sp (s, ids::maxBoost, 12.0f));
                }
                list.push_back ({ "Upward Glue", v, { { b0, level, 4 }, { b1, level, 4 }, { b2, level, 4 } } });
            }

            list.push_back ({ "Hard Gate",
                              { sp (inputStage, ids::attack, 0.1f), sp (inputStage, ids::hold, 20.0f), sp (inputStage, ids::release, 40.0f) },
                              { { inputStage, level, 8 } } });

            list.push_back ({ "Stair Steps",
                              { sp (inputStage, ids::attack, 0.5f), sp (inputStage, ids::release, 20.0f) },
                              { { inputStage, level, 10 } } });

            list.push_back ({ "Fold Waveshaper",
                              { sp (inputStage, ids::mode, (float) modeWaveshaper), sp (inputStage, ids::mix, 70.0f), { ids::quality, 3.0f } },
                              { { inputStage, level, 12 } } });

            list.push_back ({ "Soft Clip Waveshaper",
                              { sp (inputStage, ids::mode, (float) modeWaveshaper), { ids::quality, 2.0f } },
                              { { inputStage, level, 11 } } });

            list.push_back ({ "Inverted Dynamics",
                              { sp (inputStage, ids::rms, 20.0f), sp (inputStage, ids::attack, 5.0f), sp (inputStage, ids::release, 100.0f) },
                              { { inputStage, level, 9 } } });

            {
                Values v;
                addBand (v, 1, 120.0f, slope48);
                setTimes (v, b0, 10.0f, 120.0f);
                v.push_back (sp (b0, ids::lookahead, 4.0f));
                list.push_back ({ "Bass Control", v, { { b0, level, 2 } } });
            }

            {
                Values v;
                addBand (v, 1, 6000.0f);
                setTimes (v, b1, 0.5f, 60.0f);
                list.push_back ({ "Smooth De-esser", v, { { b1, level, 0, "-72,-72;-30,-30;12,-19.5" } } });
            }

            list.push_back ({ "Mid/Side Glue",
                              { sp (inputStage, ids::stereo, (float) stereoMS), sp (inputStage, ids::link, 0.0f),
                                sp (inputStage, ids::attack, 30.0f), sp (inputStage, ids::release, 200.0f) },
                              { { inputStage, level, 1 } } });

            list.push_back ({ "Sidechain Pump",
                              { sp (inputStage, ids::scSource, (float) scExternal), sp (inputStage, ids::attack, 1.0f),
                                sp (inputStage, ids::release, 180.0f), sp (inputStage, ids::relShape, 60.0f) },
                              { { inputStage, level, 0, "-72,-72;-30,-30;0,-24;12,-12" } } });

            // Sidechain a kick: only the bass band ducks, triggered by the kick's own low end, with 1 ms
            // lookahead so the duck is already down when the kick lands.
            {
                Values v;
                addBand (v, 1, 120.0f);
                setTimes (v, b0, 0.5f, 120.0f);
                v.push_back (sp (b0, ids::scSource, (float) scExternal));
                v.push_back (sp (b0, ids::relShape, 50.0f));
                v.push_back (sp (b0, ids::lookahead, 2.0f));
                list.push_back ({ "Kick Ducks Bass", v, { { b0, level, 14 } } });
            }

            // Maximus's default patch (master band, bend at 25 %), matched by measurement: linear curve
            // giving +10.6 dB to quiet signals and bending into 0 dBFS, 2 ms lookahead, REL 85.53 ms with
            // release curve 3 (Accel 3).
            list.push_back ({ "Maximus Default",
                              { sp (masterStage, ids::lookahead, 3.0f), sp (masterStage, ids::attack, 0.2f),
                                sp (masterStage, ids::release, 85.53f), sp (masterStage, ids::relLaw, (float) ids::accelRelease (3)) },
                              { { masterStage, level, 15 } } });

            // Maximus factory presets rebuilt from screenshots (curves on linear axes, bands at 187 Hz and
            // 2.79 kHz) and fitted to renders of the measurement kit: static curves within about 1 dB.
            // Release times are the presets' own REL values (read from the .fst files) with release curve 3.
            auto maximusBands = [&] (Values& v)
            {
                addBand (v, 1, 187.0f);
                addBand (v, 2, 2790.0f);
                v.push_back (sp (masterStage, ids::lookahead, 3.0f));
                v.push_back (sp (masterStage, ids::attack, 0.2f));
                for (int s : { b0, b1, b2, masterStage })
                {
                    v.push_back (sp (s, ids::release, 85.53f));
                    v.push_back (sp (s, ids::relLaw, (float) ids::accelRelease (3)));
                }
            };

            auto bandGains = [&] (Values& v, int stage, float pre, float post)
            {
                v.push_back (sp (stage, ids::pre, pre));
                v.push_back (sp (stage, ids::post, post));
            };

            {
                Values v;
                maximusBands (v);
                bandGains (v, b0, 15.0f, -3.8f);
                bandGains (v, b1, 16.0f, -8.3f);
                bandGains (v, b2, 16.0f, -12.9f);
                // Maximus: ATT 68 / 44 / 2 ms, REL 174 / 154 / 85.53 ms. Its attack eases in, so the mid band's
                // 44 ms matches our 30 ms (fitted to the render); the low band is scaled the same way.
                v.push_back (sp (b0, ids::attack, 46.0f));
                v.push_back (sp (b0, ids::release, 174.0f));
                v.push_back (sp (b1, ids::attack, 30.0f));
                v.push_back (sp (b1, ids::release, 154.0f));
                v.push_back (sp (b2, ids::attack, 2.0f));
                list.push_back ({ "Maximus Punchy Drums", v,
                                  { { b0, level, 0, "lin;0,0,0,0;0.703,0.696,0,0.5;2,1" },
                                    { b1, level, 0, "lin;0,0,0,0.275;0.964,0.821,0,0;2,1.027" },
                                    { b2, level, 0, "lin;0,0,0,0;0.667,0.696,0,0.45;2,1.018" },
                                    { masterStage, level, 0, "lin;0,0,0,0;0.964,1.018,0,0;2,1.018" } } });
            }

            {
                Values v;
                maximusBands (v);
                bandGains (v, b0, 14.0f, 0.3f);
                bandGains (v, b1, 8.0f, -3.1f);
                bandGains (v, b2, 15.0f, -1.6f);
                v.push_back (sp (b0, ids::release, 137.5f));
                for (int s : { b0, b1, b2 })
                {
                    v.push_back (sp (s, ids::attack, 0.5f));
                    v.push_back (sp (s, ids::lookahead, 3.0f));
                }
                list.push_back ({ "Maximus Max Loudness", v,
                                  { { b0, level, 0, "lin;0,0,0,0;0.964,1.036,0,0;2,1.036" },
                                    { b1, level, 0, "lin;0,0,0,0;0.81,0.8125,0,0.36;2,1.232" },
                                    { b2, level, 0, "lin;0,0,0,0.07;0.991,1,0,0;2,1" },
                                    { masterStage, level, 0, "lin;0,0,0,0;0.964,1.027,0,0.22;2,1.241" } } });
            }

            // Any hit on the sidechain ducks lows deeply, mids a little and the top barely.
            {
                Values v;
                addBand (v, 1, 150.0f);
                addBand (v, 2, 2500.0f);
                for (int s : { b0, b1, b2 })
                {
                    setTimes (v, s, 1.0f, 200.0f);
                    v.push_back (sp (s, ids::relShape, 60.0f));
                    v.push_back (sp (s, ids::scSource, (float) scExternalFull));
                }
                list.push_back ({ "Multiband Sidechain Duck", v, { { b0, level, 0, "-72,-72;-40,-40;-10,-34;12,-12" },
                                                                   { b1, level, 0, "-72,-72;-40,-40;-10,-18;12,4" },
                                                                   { b2, level, 0, "-72,-72;-40,-40;-10,-13;12,9" } } });
            }

            {
                Values v;
                addBand (v, 1, 100.0f);
                addBand (v, 2, 400.0f);
                addBand (v, 3, 1600.0f);
                addBand (v, 4, 6400.0f);
                setTimes (v, b1, 0.5f, 15.0f);
                setTimes (v, b3, 1.0f, 30.0f);
                v.push_back (sp (b2, ids::mode, (float) modeWaveshaper));
                v.push_back (sp (b4, ids::rms, 15.0f));
                v.push_back ({ ids::quality, 2.0f });
                list.push_back ({ "Multiband Mangle", v,
                                  { { b0, level, 1 }, { b1, level, 10 }, { b2, level, 12 }, { b3, level, 10 }, { b4, level, 9 },
                                    { b4, transient, 2 } } });
            }

            // ---- Mastering ------------------------------------------------------------------------
            // Three gentle bands with program-dependent release, then a calm Maximus-style master limit.
            {
                Values v;
                addBand (v, 1, 120.0f);
                addBand (v, 2, 3500.0f);
                for (int s : { b0, b1, b2 })
                    autoTiming (v, s, 10.0f, 120.0f);
                masterTiming (v, masterStage);
                v.push_back ({ ids::lowCut, 25.0f });
                v.push_back ({ ids::limiter, 1.0f });
                v.push_back ({ ids::ceiling, -1.0f });
                const juce::String gentle = "-72,-72;-20,-20;12,-4";
                list.push_back ({ "Transparent Master", v, { { b0, level, 0, gentle }, { b1, level, 0, gentle }, { b2, level, 0, gentle },
                                                             { masterStage, level, 0, "-72,-72;-4,-4;12,-1" } } });
            }

            // One band of 2:1 glue: quick release that settles slowly (REL 2), so it holds the mix together
            // without pumping.
            {
                Values v;
                glueTiming (v, b0);
                v.push_back ({ ids::lowCut, 20.0f });
                v.push_back ({ ids::limiter, 1.0f });
                v.push_back ({ ids::ceiling, -1.0f });
                list.push_back ({ "Mastering Glue", v, { { b0, level, 0, "-72,-72;-18,-18;12,-3" } } });
            }

            // Loud but not crushed: four bands with eased attacks (transients keep half their edge) and the
            // Maximus-style release, soft clipper and limiter.
            {
                Values v;
                addBand (v, 1, 120.0f);
                addBand (v, 2, 1000.0f);
                addBand (v, 3, 6000.0f);
                for (int s : { b0, b1, b2, b3 })
                    masterTiming (v, s);
                v.push_back ({ ids::lowCut, 25.0f });
                v.push_back ({ ids::outGain, 4.0f });
                v.push_back ({ ids::clip, (float) clipSoft });
                v.push_back ({ ids::limiter, 1.0f });
                v.push_back ({ ids::ceiling, -0.3f });
                v.push_back ({ ids::quality, 2.0f });
                const juce::String firm = "-72,-72;-16,-16;12,-6";
                list.push_back ({ "Loud & Calm Master", v, { { b0, level, 0, firm }, { b1, level, 0, firm },
                                                             { b2, level, 0, firm }, { b3, level, 0, firm } } });
            }

            // ---- Mix and bus ----------------------------------------------------------------------
            {
                Values v;
                autoTiming (v, b0, 10.0f, 150.0f);
                v.push_back (sp (b0, ids::rms, 5.0f));
                list.push_back ({ "Mix Bus Glue", v, { { b0, level, 0, "-72,-72;-24,-24;12,-6" } } });
            }

            {
                Values v;
                setTimes (v, b0, 10.0f, 100.0f);
                v.push_back (sp (b0, ids::attLaw, 1.0f));
                v.push_back (sp (b0, ids::release2, 300.0f));
                v.push_back (sp (b0, ids::lookahead, 2.0f));
                v.push_back (sp (b0, ids::post, 5.0f));
                list.push_back ({ "Drum Bus Glue", v, { { b0, level, 0, "-72,-72;-20,-20;12,-11" } } });
            }

            // Parallel ("New York") compression: a crushed copy under the dry signal, inside the stage.
            {
                Values v;
                setTimes (v, b0, 2.0f, 100.0f);
                v.push_back (sp (b0, ids::lookahead, 2.0f));
                v.push_back (sp (b0, ids::mix, 40.0f));
                v.push_back (sp (b0, ids::post, 10.0f));
                list.push_back ({ "NY Compression", v, { { b0, level, 0, "-72,-72;-30,-30;12,-24" } } });
            }

            // The sub band held steady (Auto release, so long bass notes don't pump), everything above untouched.
            {
                Values v;
                addBand (v, 1, 90.0f, slope48);
                autoTiming (v, b0, 15.0f, 120.0f);
                v.push_back ({ ids::lowCut, 20.0f });
                list.push_back ({ "Sub Glue", v, { { b0, level, 0, "-72,-72;-24,-24;12,-15" } } });
            }

            // ---- Vocals ---------------------------------------------------------------------------
            // Levels in both directions: quiet words come up a little, loud ones down 3:1, with the soft
            // vocal detector (eased attack, short RMS, quick release that settles slowly).
            {
                Values v;
                vocalTiming (v, b0);
                v.push_back (sp (b0, ids::maxBoost, 12.0f));
                v.push_back ({ ids::lowCut, 80.0f });
                list.push_back ({ "Vocal Leveler", v, { { b0, level, 0, "-72,-60;-40,-34;-24,-24;12,-12" } } });
            }

            // Only the sibilance band (4.5-9 kHz) is pressed down, hard and fast; the air above stays open.
            {
                Values v;
                addBand (v, 1, 4500.0f);
                addBand (v, 2, 9000.0f);
                setTimes (v, b1, 0.5f, 50.0f);
                v.push_back (sp (b1, ids::lookahead, 1.0f));
                list.push_back ({ "Narrow De-esser", v, { { b1, level, 0, "-72,-72;-36,-36;12,-28" } } });
            }

            // Boxy low mids held 2:1, presence and air lifted when quiet (detail without harshness).
            {
                Values v;
                addBand (v, 1, 250.0f);
                addBand (v, 2, 3000.0f);
                vocalTiming (v, b1);
                vocalTiming (v, b2);
                v.push_back (sp (b2, ids::maxBoost, 8.0f));
                v.push_back ({ ids::lowCut, 80.0f });
                list.push_back ({ "Vocal Presence", v, { { b1, level, 0, "-72,-72;-28,-28;12,-8" },
                                                         { b2, level, 0, "-72,-60;-36,-36;12,12" } } });
            }

            // ---- Drums ----------------------------------------------------------------------------
            // Smashed in three bands, but the eased attack keeps half of every hit, and the clipper takes
            // what's left.
            {
                Values v;
                addBand (v, 1, 150.0f);
                addBand (v, 2, 3000.0f);
                for (int s : { b0, b1, b2 })
                {
                    setTimes (v, s, 10.0f, 60.0f);
                    v.push_back (sp (s, ids::attLaw, 1.0f));
                    v.push_back (sp (s, ids::relLaw, (float) ids::accelRelease (3)));
                }
                v.push_back ({ ids::outGain, 3.0f });
                v.push_back ({ ids::clip, (float) clipSoft });
                v.push_back ({ ids::limiter, 1.0f });
                v.push_back ({ ids::quality, 2.0f });
                list.push_back ({ "Breakbeat Slam", v, { { b0, level, 6 }, { b1, level, 6 }, { b2, level, 6 } } });
            }

            // ---- Sound design ---------------------------------------------------------------------
            // Only the mid band is gated: the lows and highs ring on, the middle chops.
            {
                Values v;
                addBand (v, 1, 200.0f);
                addBand (v, 2, 3000.0f);
                v.push_back (sp (b1, ids::attack, 0.5f));
                v.push_back (sp (b1, ids::hold, 30.0f));
                v.push_back (sp (b1, ids::release, 80.0f));
                list.push_back ({ "Gated Mid", v, { { b1, level, 8 } } });
            }

            // A smashed, driven copy tucked under the clean signal.
            list.push_back ({ "Dirty Behind Clean",
                              { sp (inputStage, ids::satType, (float) satTape), sp (inputStage, ids::drive, 18.0f),
                                sp (inputStage, ids::mix, 35.0f), sp (inputStage, ids::attack, 2.0f), sp (inputStage, ids::release, 60.0f),
                                { ids::quality, 2.0f } },
                              { { inputStage, level, 6 } } });

            list.push_back ({ "Lo-Fi Crush",
                              { sp (inputStage, ids::satType, (float) satCrush), sp (inputStage, ids::drive, 18.0f),
                                sp (inputStage, ids::mix, 60.0f), { ids::lowCut, 150.0f } },
                              { { inputStage, level, 1 } } });

            // Group by category (the menu shows them in sections; previous/next follow this order).
            std::stable_sort (list.begin() + 1, list.end(), [] (const FactoryPreset& a, const FactoryPreset& b)
                              { return categoryRank (categoryOf (a.name)) < categoryRank (categoryOf (b.name)); });

            return list;
        }();

        return presets;
    }
}

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state, CurveBank& bank)
    : apvts (state), curves (bank)
{
    rescanUserPresets();
}

juce::File PresetManager::getUserPresetFolder()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif

    return base.getChildFile ("Maki plugins").getChildFile ("DynMap").getChildFile ("Presets");
}

void PresetManager::rescanUserPresets()
{
    userPresets = getUserPresetFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
    userPresets.sort();
}

int PresetManager::getNumFactoryPresets() const
{
    return (int) factoryPresets().size();
}

juce::StringArray PresetManager::getPresetNames() const
{
    juce::StringArray names;

    for (const auto& preset : factoryPresets())
        names.add (preset.name);

    for (const auto& file : userPresets)
        names.add (file.getFileNameWithoutExtension());

    return names;
}

void PresetManager::resetToDefaults()
{
    for (auto* p : apvts.processor.getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (p))
            param->setValueNotifyingHost (param->getDefaultValue());

    curves.resetAll();
}

void PresetManager::loadPreset (int index)
{
    const int numFactory = getNumFactoryPresets();

    if (juce::isPositiveAndBelow (index, numFactory))
    {
        const auto& preset = factoryPresets()[(size_t) index];
        resetToDefaults();

        for (const auto& [id, value] : preset.values)
            if (auto* param = apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (value));

        for (const auto& def : preset.curves)
            curves.set (def.stage, def.kind, def.custom.isNotEmpty() ? Curve::fromString (def.kind, def.custom)
                                                                    : Curve::preset (def.kind, def.presetIndex));

        setCurrentPresetName (preset.name);

        if (onStateReplaced)
            onStateReplaced();
        return;
    }

    const int userIndex = index - numFactory;

    if (! juce::isPositiveAndBelow (userIndex, userPresets.size()))
        return;

    const auto file = userPresets[userIndex];

    if (auto xml = juce::XmlDocument::parse (file))
    {
        auto tree = juce::ValueTree::fromXml (*xml);

        if (! tree.hasType (apvts.state.getType()))
            return;

        tree.setProperty (presetNameId, file.getFileNameWithoutExtension(), nullptr);

        for (const auto& property : uiProperties)
            if (apvts.state.hasProperty (property))
                tree.setProperty (property, apvts.state.getProperty (property), nullptr);

        replaceState (tree);
    }
}

bool PresetManager::saveUserPreset (const juce::String& name)
{
    const auto cleanName = juce::File::createLegalFileName (name.trim());

    if (cleanName.isEmpty())
        return false;

    const auto folder = getUserPresetFolder();

    if (! folder.createDirectory())
        return false;

    auto state = copyState();
    state.setProperty (presetNameId, cleanName, nullptr);

    for (const auto& property : uiProperties)
        state.removeProperty (property, nullptr);

    const auto xml = state.createXml();

    if (xml == nullptr || ! xml->writeTo (folder.getChildFile (cleanName + fileExtension)))
        return false;

    rescanUserPresets();
    setCurrentPresetName (cleanName);
    return true;
}

juce::ValueTree PresetManager::copyState() const
{
    auto state = apvts.copyState();
    curves.writeTo (state);
    return state;
}

void PresetManager::replaceState (const juce::ValueTree& tree)
{
    apvts.replaceState (tree);
    curves.readFrom (tree);

    if (onStateReplaced)
        onStateReplaced();
}

juce::String PresetManager::getCurrentPresetName() const
{
    return apvts.state.getProperty (presetNameId, "Init").toString();
}

int PresetManager::getCurrentPresetIndex() const
{
    return getPresetNames().indexOf (getCurrentPresetName());
}

void PresetManager::setCurrentPresetName (const juce::String& name)
{
    apvts.state.setProperty (presetNameId, name, nullptr);
}
}

namespace dynmap
{
juce::String PresetManager::getCategory (int index) const
{
    return juce::isPositiveAndBelow (index, getNumFactoryPresets()) ? categoryOf (factoryPresets()[(size_t) index].name)
                                                                     : juce::String ("User");
}
}
