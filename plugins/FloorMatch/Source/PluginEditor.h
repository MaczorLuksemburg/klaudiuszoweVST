#pragma once

#include "PluginProcessor.h"
#include <KlaudLookAndFeel.h>

namespace floormatch::ui
{
    using APVTS = juce::AudioProcessorValueTreeState;

    namespace colours
    {
        const juce::Colour accent { 0xff5fd3a8 };   // target and output noise
        const juce::Colour input  { 0xff8795a8 };   // input noise
        const juce::Colour learn  { 0xfff0b35a };   // learning / attention
    }

    const klaud::Palette& palette();

    // Rotary knob with a caption above and the value below.
    class Knob : public juce::Component
    {
    public:
        Knob (APVTS&, const juce::String& paramId, const juce::String& title, bool arcFromStart = false);

        void paint (juce::Graphics&) override;
        void resized() override;

        juce::Slider slider;

    private:
        juce::String title;
        std::unique_ptr<APVTS::SliderAttachment> attachment;
    };

    // Caption above a combo box.
    class Choice : public juce::Component
    {
    public:
        Choice (APVTS&, const juce::String& paramId, const juce::String& title);

        void paint (juce::Graphics&) override;
        void resized() override;

        juce::ComboBox box;

    private:
        juce::String title;
        std::unique_ptr<APVTS::ComboBoxAttachment> attachment;
    };

    // Noise spectra per 1/6 octave: input noise floor, target and output noise floor.
    class SpectrumView : public juce::Component
    {
    public:
        explicit SpectrumView (FloorMatchProcessor&);

        void update (const floormatch::dsp::Snapshot&, bool hasSnapshot);
        void paint (juce::Graphics&) override;

    private:
        float xFor (float hz) const;
        float yFor (float db) const;
        juce::Path curve (const std::array<float, floormatch::dsp::numProfileBands>&, bool closed) const;

        FloorMatchProcessor& processor;
        floormatch::dsp::Snapshot shown;
        bool active = false;
        float topDb = -40.0f;
        static constexpr float rangeDb = 80.0f;
    };

    // Vertical in/out noise floor meters with the target marked across both.
    class FloorMeter : public juce::Component
    {
    public:
        explicit FloorMeter (APVTS&);

        void update (const floormatch::dsp::Snapshot&, bool hasSnapshot);
        void paint (juce::Graphics&) override;

    private:
        float yFor (float db, juce::Rectangle<float> area) const;

        std::atomic<float>* target;
        float inDb = -150.0f, outDb = -150.0f;
        bool active = false;
        static constexpr float topDb = -20.0f, bottomDb = -100.0f;
    };
}

// Everything is laid out at a fixed base size and scaled as a whole when the window resizes.
class FloorMatchMainView : public juce::Component, private juce::Timer
{
public:
    static constexpr int baseWidth = 860;
    static constexpr int baseHeight = 500;

    FloorMatchMainView (FloorMatchProcessor&, klaud::LookAndFeel&);
    ~FloorMatchMainView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();   // pull meters and spectrum from the processor (the timer does this 30 times a second)

private:
    void timerCallback() override;
    void updateButtons();

    struct LookAndFeelSetter
    {
        LookAndFeelSetter (juce::Component& c, juce::LookAndFeel& lf) { c.setLookAndFeel (&lf); }
    };

    FloorMatchProcessor& processor;
    LookAndFeelSetter lookAndFeelSetter;

    floormatch::ui::SpectrumView spectrum;
    floormatch::ui::FloorMeter meter;
    floormatch::ui::Knob target, maxReduction, match;
    floormatch::ui::Choice lookahead, listen;
    juce::TextButton learnButton { "Learn" }, quietestButton { "Use quietest" }, resetQuietestButton { "Reset" };
    juce::TextButton fillButton { "Room tone fill" }, clearProfileButton { "Clear" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> fillAttachment;

    juce::String profileText;
    int blink = 0;

    juce::TooltipWindow tooltips { this, 600 };
};

class FloorMatchEditor : public juce::AudioProcessorEditor
{
public:
    explicit FloorMatchEditor (FloorMatchProcessor&);
    ~FloorMatchEditor() override;

    void resized() override;
    void refresh();

private:
    FloorMatchProcessor& processor;
    klaud::LookAndFeel lookAndFeel { floormatch::ui::palette() };
    FloorMatchMainView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FloorMatchEditor)
};
