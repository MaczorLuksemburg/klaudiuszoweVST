#include "Parameters.h"

namespace msc
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

    // 0-500 ms, exponential so the 1-100 ms region (where Haas timing matters) gets
    // about 60 % of the knob travel: 1 ms sits at ~16 %, 100 ms at ~77 %.
    Range haasRange()
    {
        constexpr float curve = 6.95f;
        const float scale = std::exp (curve) - 1.0f;

        return { 0.0f, 500.0f,
                 [=] (float start, float end, float t) { return start + (end - start) * (std::exp (curve * t) - 1.0f) / scale; },
                 [=] (float start, float end, float v) { return std::log (1.0f + (v - start) / (end - start) * scale) / curve; },
                 [] (float start, float end, float v) { return juce::jlimit (start, end, v); } };
    }

    float parseNumber (const juce::String& text)
    {
        const auto lower = text.trim().toLowerCase();
        const float multiplier = lower.containsChar ('k') ? 1000.0f : 1.0f;
        return lower.retainCharacters ("0123456789.-").getFloatValue() * multiplier;
    }

    juce::String percentText (float value, int) { return juce::String (juce::roundToInt (value)) + " %"; }

    juce::String msText (float ms, int)
    {
        if (ms < 0.005f) return "0 ms";
        if (ms < 10.0f)  return juce::String (ms, 2) + " ms";
        if (ms < 100.0f) return juce::String (ms, 1) + " ms";
        return juce::String (juce::roundToInt (ms)) + " ms";
    }

    juce::String shapeText (float v, int)
    {
        if (v <= 0.01f)                    return "Low-pass";
        if (std::abs (v - 0.5f) <= 0.01f)  return "Band-pass";
        if (v >= 0.99f)                    return "High-pass";

        return v < 0.5f ? "LP > BP " + juce::String (juce::roundToInt (v * 200.0f)) + " %"
                        : "BP > HP " + juce::String (juce::roundToInt ((v - 0.5f) * 200.0f)) + " %";
    }

    float shapeFromText (const juce::String& text)
    {
        const auto lower = text.toLowerCase();
        if (lower.contains ("low"))  return 0.0f;
        if (lower.contains ("band")) return 0.5f;
        if (lower.contains ("high")) return 1.0f;
        return juce::jlimit (0.0f, 1.0f, parseNumber (text) / 100.0f);
    }

    juce::String balanceText (float v, int)
    {
        const int amount = juce::roundToInt (std::abs (v));
        if (amount == 0) return "C";
        return (v < 0.0f ? "L " : "R ") + juce::String (amount);
    }

    float balanceFromText (const juce::String& text)
    {
        const auto lower = text.trim().toLowerCase();
        const float amount = std::abs (parseNumber (lower));
        return lower.startsWith ("l") ? -amount : amount;
    }

    std::unique_ptr<juce::AudioParameterFloat> floatParam (const char* id, const juce::String& name, Range range, float defaultValue,
                                                           std::function<juce::String (float, int)> toText,
                                                           std::function<float (const juce::String&)> fromText = parseNumber)
    {
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, defaultValue,
                                                            juce::AudioParameterFloatAttributes()
                                                                .withStringFromValueFunction (std::move (toText))
                                                                .withValueFromStringFunction (std::move (fromText)));
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

    auto hzParam (const char* id, const juce::String& name, float low, float high, float defaultValue)
    {
        return floatParam (id, name, logRange (low, high), defaultValue, [] (float v, int) { return formatHz (v); });
    }

    auto shapeParam (const char* id, const juce::String& name, float defaultValue)
    {
        return floatParam (id, name, Range (0.0f, 1.0f), defaultValue, shapeText, shapeFromText);
    }

    auto percentParam (const char* id, const juce::String& name, float low, float high, float defaultValue)
    {
        return floatParam (id, name, Range (low, high, 0.1f), defaultValue, percentText);
    }
}

juce::String formatHz (float hz)
{
    if (hz < 1000.0f)
        return juce::String (juce::roundToInt (hz)) + " Hz";

    return juce::String (hz / 1000.0f, hz < 10000.0f ? 2 : 1) + " kHz";
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    const juce::StringArray sources { "Stereo", "Mono", "Left", "Right" };
    const juce::StringArray slopes  { "12 dB/oct", "24 dB/oct", "48 dB/oct" };

    auto input = std::make_unique<juce::AudioProcessorParameterGroup> ("input", "Input", " | ");
    input->addChild (boolParam (ids::inOn, "Input On"),
                     shapeParam (ids::inShape, "Input Filter Shape", 1.0f),
                     hzParam (ids::inCutoff, "Input Cutoff", 20.0f, 22000.0f, 200.0f),
                     choiceParam (ids::inSlope, "Input Slope", slopes, 1),
                     choiceParam (ids::inWetSrc, "Input Processed Source", sources, sourceStereo),
                     choiceParam (ids::inDrySrc, "Input Unprocessed Source", sources, sourceStereo));

    auto dynPan = std::make_unique<juce::AudioProcessorParameterGroup> ("dynpan", "Dynamic Pan", " | ");
    dynPan->addChild (boolParam (ids::dpOn, "Dyn Pan On"),
                      floatParam (ids::dpAmount, "Dyn Pan Amount", Range (0.0f, 1.0f), 0.0f,
                                  [] (float v, int) { return juce::String (juce::roundToInt (v * 100.0f)) + " % of max"; },
                                  [] (const juce::String& t) { return juce::jlimit (0.0f, 1.0f, parseNumber (t) / 100.0f); }),
                      floatParam (ids::dpMax, "Dyn Pan Max", Range (10.0f, 1000.0f, 1.0f), 200.0f, percentText),
                      shapeParam (ids::dpShape, "Dyn Pan Filter Shape", 0.0f),
                      hzParam (ids::dpCutoff, "Dyn Pan Cutoff", 20.0f, 22000.0f, 22000.0f),
                      choiceParam (ids::dpSlope, "Dyn Pan Slope", slopes, 0),
                      choiceParam (ids::dpClip, "Dyn Pan Mod Clip", { "Off", "Hard", "Soft", "Extreme" }, clipSoft),
                      choiceParam (ids::dpSource, "Dyn Pan Mod Source", { "Sum", "Left", "Right" }, modSum),
                      choiceParam (ids::dpComp, "Dyn Pan Mod Comp", { "Off", "2:1", "4:1", "8:1" }, 0),
                      floatParam (ids::dpThresh, "Dyn Pan Mod Threshold", Range (-60.0f, 0.0f, 0.1f), -30.0f,
                                  [] (float v, int) { return juce::String (v, 1) + " dB"; }),
                      choiceParam (ids::dpMakeup, "Dyn Pan Mod Makeup", { "0 %", "25 %", "50 %", "75 %", "100 %" }, 2));

    auto haas = std::make_unique<juce::AudioProcessorParameterGroup> ("haas", "Haas", " | ");
    haas->addChild (boolParam (ids::hsOn, "Haas On"),
                    floatParam (ids::hsLeft, "Haas Left Delay", haasRange(), 0.0f, msText),
                    floatParam (ids::hsRight, "Haas Right Delay", haasRange(), 0.0f, msText),
                    boolParam (ids::hsInvL, "Haas Left Polarity"),
                    boolParam (ids::hsInvR, "Haas Right Polarity"));

    auto chorus = std::make_unique<juce::AudioProcessorParameterGroup> ("chorus", "Chorus", " | ");
    chorus->addChild (boolParam (ids::chOn, "Chorus On"),
                      choiceParam (ids::chMode, "Chorus Mode", { "I", "II", "I+II" }, 0),
                      percentParam (ids::chDepth, "Chorus Depth", 0.0f, 100.0f, 100.0f),
                      percentParam (ids::chWidth, "Chorus Width", 0.0f, 100.0f, 100.0f),
                      hzParam (ids::chTone, "Chorus Tone", 1000.0f, 20000.0f, 8000.0f),
                      percentParam (ids::chMix, "Chorus Mix", 0.0f, 100.0f, 50.0f));

    auto image = std::make_unique<juce::AudioProcessorParameterGroup> ("image", "Image", " | ");
    image->addChild (boolParam (ids::imOn, "Image On"),
                     floatParam (ids::imBalance, "Image Balance", Range (-100.0f, 100.0f, 0.1f), 0.0f, balanceText, balanceFromText),
                     percentParam (ids::imMid, "Image Mid", 0.0f, 200.0f, 100.0f),
                     percentParam (ids::imSide, "Image Side", 0.0f, 200.0f, 100.0f));

    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::move (input), std::move (dynPan), std::move (haas), std::move (chorus), std::move (image));
    return layout;
}
}
