#pragma once

#include "Components.h"

// Everything is laid out at a fixed base size and scaled as a whole when the window resizes.
class MscMainView : public juce::Component
{
public:
    static constexpr int baseWidth = 1240;
    static constexpr int baseHeight = 540;

    MscMainView (MscProcessor&, klaud::LookAndFeel&);
    ~MscMainView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // Declared first so the look and feel is in place before any child component is built.
    struct LookAndFeelSetter
    {
        LookAndFeelSetter (juce::Component& c, juce::LookAndFeel& lf) { c.setLookAndFeel (&lf); }
    };

    LookAndFeelSetter lookAndFeelSetter;
    msc::ui::PresetBar presetBar;
    msc::ui::InputModule inputModule;
    msc::ui::DynamicPanModule dynPanModule;
    msc::ui::HaasModule haasModule;
    msc::ui::ChorusModule chorusModule;
    msc::ui::ImageModule imageModule;

    juce::TooltipWindow tooltips { this, 600 };
};

class MscEditor : public juce::AudioProcessorEditor
{
public:
    explicit MscEditor (MscProcessor&);
    ~MscEditor() override;

    void resized() override;

private:
    MscProcessor& processor;
    klaud::LookAndFeel lookAndFeel { msc::ui::palette() };
    MscMainView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MscEditor)
};
