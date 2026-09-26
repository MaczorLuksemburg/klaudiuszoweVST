#include "Parameters.h"

namespace floormatch
{
namespace
{
    using Range = juce::NormalisableRange<float>;

    float parseNumber (const juce::String& text)
    {
        return text.retainCharacters ("0123456789.-").getFloatValue();
    }

    std::unique_ptr<juce::AudioParameterFloat> floatParam (const char* id, const juce::String& name, Range range, float defaultValue,
                                                           std::function<juce::String (float, int)> toText)
    {
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, range, defaultValue,
                                                            juce::AudioParameterFloatAttributes()
                                                                .withStringFromValueFunction (std::move (toText))
                                                                .withValueFromStringFunction (parseNumber));
    }
}

juce::String formatDb (float db, int decimals)
{
    return juce::String (db, decimals) + " dB";
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (floatParam (ids::target, "Target", Range (-96.0f, -24.0f, 0.1f), -60.0f,
                            [] (float v, int) { return formatDb (v); }),
                floatParam (ids::match, "Colour Match", Range (0.0f, 100.0f, 0.1f), 100.0f,
                            [] (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; }),
                floatParam (ids::maxReduction, "Max Reduction", Range (0.0f, 40.0f, 0.1f), 12.0f,
                            [] (float v, int) { return formatDb (v); }),
                std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ids::lookahead, 1 }, "Lookahead",
                                                              juce::StringArray { "Off", "0.5 s", "1 s", "2 s" }, 2,
                                                              juce::AudioParameterChoiceAttributes().withAutomatable (false)),
                std::make_unique<juce::AudioParameterBool> (juce::ParameterID { ids::fill, 1 }, "Fill", false),
                std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ids::listen, 1 }, "Listen",
                                                              juce::StringArray { "Output", "Removed" }, listenOutput));
    return layout;
}
}
