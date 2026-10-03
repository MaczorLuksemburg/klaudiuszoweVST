#include "Parameters.h"

namespace dynmap
{
namespace
{
    using Range = juce::NormalisableRange<float>;

    Range logRange (float low, float high)
    {
        return { low, high,
                 [] (float start, float end, float t) { return start * std::pow (end / start, t); },
                 [] (float start, float end, float v) { return std::log (v / start) / std::log (end / start); },
                 [] (float start, float end, float v) { return juce::jlimit (start, end, v); } };
    }

    Range skewed (float low, float high, float centre)
    {
        Range r (low, high);
        r.setSkewForCentre (centre);
        return r;
    }

    float parseNumber (const juce::String& text)
    {
        const auto lower = text.trim().toLowerCase();
        const float multiplier = lower.containsChar ('k') ? 1000.0f : 1.0f;
        return lower.retainCharacters ("0123456789.-").getFloatValue() * multiplier;
    }

    juce::String percentText (float value, int) { return juce::String (juce::roundToInt (value)) + " %"; }
    juce::String dbText (float value, int)      { return formatDb (value); }
    juce::String plainDbText (float value, int) { return juce::String (value, 1) + " dB"; }

    juce::String msText (float ms, int)
    {
        if (ms < 0.005f) return "0 ms";
        if (ms < 1.0f)   return juce::String (ms, 2) + " ms";
        if (ms < 10.0f)  return juce::String (ms, 1) + " ms";
        if (ms < 1000.0f) return juce::String (juce::roundToInt (ms)) + " ms";
        return juce::String (ms / 1000.0f, 2) + " s";
    }

    float msFromText (const juce::String& text)
    {
        const auto lower = text.trim().toLowerCase();
        const float value = lower.retainCharacters ("0123456789.-").getFloatValue();
        return lower.endsWith ("s") && ! lower.endsWith ("ms") ? value * 1000.0f : value;
    }

    std::unique_ptr<juce::AudioParameterFloat> floatParam (const juce::String& id, const juce::String& name, Range range, float defaultValue,
                                                           std::function<juce::String (float, int)> toText,
                                                           std::function<float (const juce::String&)> fromText = parseNumber)
    {
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, defaultValue,
                                                            juce::AudioParameterFloatAttributes()
                                                                .withStringFromValueFunction (std::move (toText))
                                                                .withValueFromStringFunction (std::move (fromText)));
    }

    std::unique_ptr<juce::AudioParameterBool> boolParam (const juce::String& id, const juce::String& name, bool defaultValue = false)
    {
        return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, defaultValue);
    }

    std::unique_ptr<juce::AudioParameterChoice> choiceParam (const juce::String& id, const juce::String& name,
                                                             const juce::StringArray& choices, int defaultIndex)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, choices, defaultIndex);
    }

    auto dbParam (const juce::String& id, const juce::String& name, float low, float high, float defaultValue)
    {
        return floatParam (id, name, Range (low, high, 0.01f), defaultValue, dbText);
    }

    auto percentParam (const juce::String& id, const juce::String& name, float low, float high, float defaultValue)
    {
        return floatParam (id, name, Range (low, high, 0.1f), defaultValue, percentText);
    }

    auto msParam (const juce::String& id, const juce::String& name, Range range, float defaultValue)
    {
        return floatParam (id, name, range, defaultValue, msText, msFromText);
    }

    std::unique_ptr<juce::AudioProcessorParameterGroup> stageGroup (int stage)
    {
        const auto name = stageName (stage);
        auto id = [stage] (const char* n) { return stageParamId (stage, n); };
        auto label = [&name] (const char* n) { return name + " " + n; };

        auto group = std::make_unique<juce::AudioProcessorParameterGroup> (stagePrefix (stage), name, " | ");

        if (isBandStage (stage) && stage != bandStage (0))
        {
            const int band = stage - 1;
            group->addChild (boolParam (id (ids::bandOn), label ("On")),
                             floatParam (id (ids::freq), label ("Crossover"), logRange (20.0f, 20000.0f), defaultCrossoverHz (band),
                                         [] (float v, int) { return formatHz (v); }),
                             choiceParam (id (ids::slope), label ("Slope"), { "6 dB/oct", "12 dB/oct", "24 dB/oct", "48 dB/oct" }, slope24));
        }

        juce::StringArray lookaheads;
        for (float ms : lookaheadMs)
            lookaheads.add (ms == 0.0f ? "Off" : juce::String (ms, ms < 1.0f ? 1 : 0) + " ms");

        group->addChild (boolParam (id (ids::bypass), label ("Bypass")),
                         choiceParam (id (ids::mode), label ("Mode"), { "Dynamics", "Waveshaper" }, modeDynamics),
                         dbParam (id (ids::pre), label ("Pre Gain"), -24.0f, 24.0f, 0.0f),
                         dbParam (id (ids::post), label ("Post Gain"), -24.0f, 24.0f, 0.0f),
                         percentParam (id (ids::mix), label ("Mix"), 0.0f, 100.0f, 100.0f),
                         msParam (id (ids::attack), label ("Attack"), logRange (0.01f, 500.0f), 5.0f),
                         msParam (id (ids::hold), label ("Hold"), skewed (0.0f, 500.0f, 30.0f), 0.0f),
                         msParam (id (ids::release), label ("Release"), logRange (1.0f, 5000.0f), 120.0f),
                         percentParam (id (ids::relShape), label ("Release Shape"), 0.0f, 100.0f, 0.0f),
                         msParam (id (ids::rms), label ("RMS"), skewed (0.0f, 300.0f, 20.0f), 0.0f),
                         choiceParam (id (ids::lookahead), label ("Lookahead"), lookaheads, 0),
                         percentParam (id (ids::link), label ("Stereo Link"), 0.0f, 100.0f, 100.0f),
                         choiceParam (id (ids::stereo), label ("Stereo Mode"), { "Left/Right", "Mid/Side" }, stereoLR),
                         floatParam (id (ids::scFilter), label ("Detector HP"), logRange (10.0f, 2000.0f), 10.0f,
                                     [] (float v, int) { return v <= 10.5f ? juce::String ("Off") : formatHz (v); }),
                         choiceParam (id (ids::scSource), label ("Detector Source"), { "Own signal", "Sidechain", "Sidechain (full)" }, scInternal),
                         msParam (id (ids::trTime), label ("Transient Time"), logRange (5.0f, 500.0f), 40.0f),
                         floatParam (id (ids::maxBoost), label ("Max Boost"), Range (0.0f, 48.0f, 0.1f), 24.0f, plainDbText),
                         floatParam (id (ids::maxCut), label ("Max Cut"), Range (0.0f, 96.0f, 0.1f), 96.0f, plainDbText),
                         msParam (id (ids::smooth), label ("Smoothing"), skewed (0.0f, 50.0f, 2.0f), 0.5f),
                         choiceParam (id (ids::satType), label ("Saturation"), { "Off", "Tape", "Tube", "Hard", "Fold", "Crush" }, satOff),
                         dbParam (id (ids::drive), label ("Drive"), 0.0f, 36.0f, 0.0f),
                         choiceParam (id (ids::satPos), label ("Saturation Position"), { "After dynamics", "Before dynamics" }, satAfter),
                         choiceParam (id (ids::relLaw), label ("Release Mode"),
                                      { "Classic", "Accel 1", "Accel 2", "Accel 3", "Accel 4", "Accel 5", "Accel 6", "Accel 7", "Accel 8" }, 0));

        if (isBandStage (stage))
            group->addChild (percentParam (id (ids::width), label ("Width"), 0.0f, 200.0f, 100.0f),
                             boolParam (id (ids::solo), label ("Solo")),
                             boolParam (id (ids::mute), label ("Mute")));

        return group;
    }
}

juce::String stagePrefix (int stage)
{
    if (stage == inputStage)  return "in";
    if (stage == masterStage) return "mst";
    return "b" + juce::String (stage - 1);
}

juce::String stageName (int stage)
{
    if (stage == inputStage)  return "Input";
    if (stage == masterStage) return "Master";
    return "Band " + juce::String (stage);
}

juce::String stageParamId (int stage, const char* name)
{
    return stagePrefix (stage) + "_" + name;
}

float defaultCrossoverHz (int band)
{
    // Spread over the audible range so a freshly enabled slot never sits on top of another one.
    return 40.0f * std::pow (2.0f, (float) band * 0.8f);
}

juce::String formatHz (float hz)
{
    if (hz < 1000.0f)
        return juce::String (juce::roundToInt (hz)) + " Hz";

    return juce::String (hz / 1000.0f, hz < 10000.0f ? 2 : 1) + " kHz";
}

juce::String formatDb (float db, int decimals)
{
    if (std::abs (db) < 0.5f * std::pow (10.0f, (float) -decimals))
        db = 0.0f;

    return (db > 0.0f ? "+" : "") + juce::String (db, decimals) + " dB";
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto global = std::make_unique<juce::AudioProcessorParameterGroup> ("global", "Global", " | ");
    global->addChild (floatParam (ids::amount, "Amount", Range (-100.0f, 200.0f, 0.1f), 100.0f, percentText),
                      floatParam (ids::time, "Time", logRange (25.0f, 400.0f), 100.0f, percentText),
                      percentParam (ids::globalMix, "Mix", 0.0f, 100.0f, 100.0f),
                      dbParam (ids::inGain, "Input Gain", -24.0f, 24.0f, 0.0f),
                      dbParam (ids::outGain, "Output Gain", -24.0f, 24.0f, 0.0f),
                      choiceParam (ids::clip, "Clipper", { "Off", "Soft", "Hard" }, clipOff),
                      boolParam (ids::limiter, "Limiter"),
                      floatParam (ids::ceiling, "Ceiling", Range (-12.0f, 0.0f, 0.01f), -0.5f,
                                  [] (float v, int) { return juce::String (v, 1) + " dBTP"; }),
                      msParam (ids::limRel, "Limiter Release", logRange (1.0f, 1000.0f), 80.0f),
                      boolParam (ids::autoGain, "Auto Gain"),
                      boolParam (ids::delta, "Delta"),
                      dbParam (ids::scGain, "Sidechain Gain", -24.0f, 24.0f, 0.0f),
                      boolParam (ids::scListen, "Sidechain Listen"),
                      choiceParam (ids::quality, "Oversampling", { "Off", "2x", "4x", "8x" }, 1),
                      choiceParam (ids::phase, "Crossover Phase", { "Minimum phase", "Linear phase" }, phaseMinimum));
    layout.add (std::move (global));

    for (int stage = 0; stage < numStages; ++stage)
        layout.add (stageGroup (stage));

    return layout;
}
}
