#pragma once

#include "Components.h"

// Everything is laid out at a fixed base size and scaled as a whole when the window resizes.
class WaveRefMainView : public juce::Component
{
public:
    static constexpr int baseWidth = 1280;
    static constexpr int baseHeight = 802;

    WaveRefMainView (WaveRefProcessor&, klaud::LookAndFeel&);
    ~WaveRefMainView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    // Runs what the timers would (tests have no message loop).
    void refresh();

private:
    // Declared first so the look and feel is in place before any child component is built.
    struct LookAndFeelSetter
    {
        LookAndFeelSetter (juce::Component& c, juce::LookAndFeel& lf) { c.setLookAndFeel (&lf); }
    };

    WaveRefProcessor& processor;
    LookAndFeelSetter lookAndFeelSetter;
    waveref::ui::UiSettings settings;
    waveref::ui::SpectrumView spectrum;
    waveref::ui::StereoPanel stereoPanel;
    waveref::ui::AnalyserBar analyserBar;
    waveref::ui::WaveformView waveform;
    waveref::ui::SlotBar slotBar;
    waveref::ui::ControlPanel controls;
    waveref::ui::PresetBar presetBar;

    juce::TooltipWindow tooltips { this, 600 };
};

class WaveRefEditor : public juce::AudioProcessorEditor
{
public:
    explicit WaveRefEditor (WaveRefProcessor&);
    ~WaveRefEditor() override;

    void resized() override;
    void refresh() { view.refresh(); }

private:
    WaveRefProcessor& processor;
    klaud::LookAndFeel lookAndFeel { waveref::ui::palette() };
    WaveRefMainView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveRefEditor)
};
