#include "Parameters.h"

namespace waveref
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

    float parseNumber (const juce::String& text)
    {
        const auto lower = text.trim().toLowerCase();
        const float multiplier = lower.containsChar ('k') ? 1000.0f : 1.0f;
        return lower.retainCharacters ("0123456789.-").getFloatValue() * multiplier;
    }

    juce::String formatHz (float hz)
    {
        if (hz < 1000.0f)
            return juce::String (juce::roundToInt (hz)) + " Hz";

        return juce::String (hz / 1000.0f, hz < 10000.0f ? 2 : 1) + " kHz";
    }

    // Symmetric range with fine control around zero: the knob travel is cubic.
    Range offsetRange (float limit)
    {
        return { -limit, limit,
                 [] (float, float end, float t) { const float u = t * 2.0f - 1.0f; return (u < 0.0f ? -1.0f : 1.0f) * std::abs (u * u * u) * end; },
                 [] (float, float end, float v) { const float u = std::cbrt (std::abs (v) / end); return ((v < 0.0f ? -u : u) + 1.0f) * 0.5f; },
                 [] (float start, float end, float v) { return juce::jlimit (start, end, v); } };
    }

    juce::String secondsText (float seconds, int)
    {
        const float a = std::abs (seconds);
        const juce::String sign = seconds > 0.0005f ? "+" : seconds < -0.0005f ? "-" : "";

        if (a < 1.0f)
            return sign + juce::String (juce::roundToInt (a * 1000.0f)) + " ms";

        return sign + juce::String (a, a < 10.0f ? 3 : 2) + " s";
    }

    float secondsFromText (const juce::String& text)
    {
        const bool ms = text.toLowerCase().contains ("ms");
        return juce::jlimit (-60.0f, 60.0f, parseNumber (text) / (ms ? 1000.0f : 1.0f));
    }

    juce::String beatsText (float beats, int)
    {
        return (beats > 0.0f ? "+" : "") + juce::String (beats, 2) + " beats";
    }

    std::unique_ptr<juce::AudioParameterBool> boolParam (const char* id, const juce::String& name, bool defaultValue = false)
    {
        return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, defaultValue);
    }

    std::unique_ptr<juce::AudioParameterChoice> choiceParam (const char* id, const juce::String& name,
                                                             const juce::StringArray& choices, int defaultIndex)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, choices, defaultIndex);
    }

    std::unique_ptr<juce::AudioParameterFloat> floatParam (const char* id, const juce::String& name, Range range, float defaultValue,
                                                           std::function<juce::String (float, int)> toText,
                                                           std::function<float (const juce::String&)> fromText)
    {
        return std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name, range, defaultValue,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction (std::move (toText))
                                                 .withValueFromStringFunction (std::move (fromText)));
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::StringArray slots;
    for (int i = 1; i <= numSlots; ++i)
        slots.add ("Slot " + juce::String (i));

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (boolParam (ids::ab, "Ref (B)"),
                choiceParam (ids::slot, "Reference Slot", slots, 0),
                boolParam (ids::matchOn, "Loudness Match", true),
                choiceParam (ids::matchMode, "Match Window", { "Short (3 s)", "Long (30 s)" }, matchShort),
                floatParam (ids::trim, "Reference Trim", Range (-24.0f, 24.0f, 0.1f), 0.0f,
                            [] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; },
                            [] (const juce::String& t) { return juce::jlimit (-24.0f, 24.0f, parseNumber (t)); }),
                boolParam (ids::mono, "Mono"),
                boolParam (ids::filterOn, "Listening Band"),
                floatParam (ids::filterFreq, "Listening Band Frequency", logRange (30.0f, 18000.0f), 1000.0f,
                            [] (float v, int) { return formatHz (v); },
                            [] (const juce::String& t) { return juce::jlimit (30.0f, 18000.0f, parseNumber (t)); }),
                floatParam (ids::filterWidth, "Listening Band Width", Range (0.25f, 4.0f, 0.01f), 1.0f,
                            [] (float v, int) { return juce::String (v, 2) + " oct"; },
                            [] (const juce::String& t) { return juce::jlimit (0.25f, 4.0f, parseNumber (t)); }),
                boolParam (ids::restartSwitch, "Restart On Switch", false),
                boolParam (ids::restartPlay, "Restart On Play", true),
                boolParam (ids::blind, "Blind Test"),
                boolParam (ids::refPlay, "Reference Play", true),
                choiceParam (ids::refMode, "Reference Sync Mode", { "Free", "Host time", "Beats" }, modeFree),
                floatParam (ids::syncOffset, "Sync Offset", offsetRange (60.0f), 0.0f, secondsText, secondsFromText),
                floatParam (ids::beatOffset, "Beat Offset", offsetRange (16.0f), 0.0f, beatsText,
                            [] (const juce::String& t) { return juce::jlimit (-16.0f, 16.0f, parseNumber (t)); }),
                floatParam (ids::nudge, "Beat Nudge", Range (-250.0f, 250.0f, 0.1f), 0.0f,
                            [] (float v, int) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " ms"; },
                            [] (const juce::String& t) { return juce::jlimit (-250.0f, 250.0f, parseNumber (t)); }),
                boolParam (ids::keyLock, "Key Lock", true),
                boolParam (ids::side, "Side"),
                boolParam (ids::delta, "Delta"));

    return layout;
}
}
