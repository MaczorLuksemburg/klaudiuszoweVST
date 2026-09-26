#pragma once

#include "PluginProcessor.h"
#include <KlaudLookAndFeel.h>

class StereoScaleEditor : public juce::AudioProcessorEditor
{
public:
    explicit StereoScaleEditor (StereoScaleProcessor&);
    ~StereoScaleEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        juce::String title;
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void setUpKnob (Knob&, const juce::String& paramId, const juce::String& title, juce::Colour accent);

    StereoScaleProcessor& processor;
    klaud::LookAndFeel lookAndFeel;

    Knob left, right, mid, side;
    juce::Rectangle<int> lrPanel, msPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoScaleEditor)
};
