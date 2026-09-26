#include "PluginEditor.h"

namespace
{
    constexpr int headerHeight = 40;
    constexpr int margin       = 12;
    constexpr int titleHeight  = 20;
    constexpr int valueHeight  = 20;
}

StereoScaleEditor::StereoScaleEditor (StereoScaleProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setLookAndFeel (&lookAndFeel);

    setUpKnob (left,  "left",  "LEFT",  klaud::colours::accentBlue);
    setUpKnob (right, "right", "RIGHT", klaud::colours::accentBlue);
    setUpKnob (mid,   "mid",   "MID",   klaud::colours::accentAmber);
    setUpKnob (side,  "side",  "SIDE",  klaud::colours::accentAmber);

    setSize (460, 230);
}

StereoScaleEditor::~StereoScaleEditor()
{
    setLookAndFeel (nullptr);
}

void StereoScaleEditor::setUpKnob (Knob& knob, const juce::String& paramId, const juce::String& title, juce::Colour accent)
{
    knob.title = title;

    auto& s = knob.slider;
    s.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, valueHeight);
    s.setColour (juce::Slider::rotarySliderFillColourId, accent);
    s.setDoubleClickReturnValue (true, 100.0);
    s.setMouseDragSensitivity (220);
    s.setVelocityBasedMode (false);
    s.setPopupDisplayEnabled (false, false, nullptr);
    addAndMakeVisible (s);

    knob.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (processor.apvts, paramId, s);
}

void StereoScaleEditor::paint (juce::Graphics& g)
{
    using namespace klaud;

    g.setGradientFill (juce::ColourGradient (colours::backgroundTop, 0.0f, 0.0f,
                                             colours::backgroundBottom, 0.0f, (float) getHeight(), false));
    g.fillAll();

    // Header.
    auto header = getLocalBounds().removeFromTop (headerHeight).reduced (margin + 4, 0);
    g.setColour (colours::text);
    g.setFont (font (17.0f, true));
    g.drawFittedText ("StereoScale", header, juce::Justification::centredLeft, 1);

    g.setColour (colours::textDim);
    g.setFont (font (11.0f));
    g.drawFittedText (JucePlugin_Manufacturer, header, juce::Justification::centredRight, 1);

    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.fillRect (0, headerHeight - 1, getWidth(), 1);

    // Panels.
    for (auto* panel : { &lrPanel, &msPanel })
    {
        g.setColour (colours::panel);
        g.fillRoundedRectangle (panel->toFloat(), 6.0f);
        g.setColour (colours::panelOutline);
        g.drawRoundedRectangle (panel->toFloat().reduced (0.5f), 6.0f, 1.0f);
    }

    // Knob titles.
    g.setFont (font (11.0f, true));
    for (auto* knob : { &left, &right, &mid, &side })
    {
        const auto area = knob->slider.getBounds().withHeight (titleHeight).translated (0, -titleHeight);
        g.setColour (colours::textDim);
        g.drawText (knob->title, area, juce::Justification::centred);
    }
}

void StereoScaleEditor::resized()
{
    auto area = getLocalBounds().withTrimmedTop (headerHeight).reduced (margin);

    lrPanel = area.removeFromLeft ((area.getWidth() - margin) / 2);
    area.removeFromLeft (margin);
    msPanel = area;

    auto placePair = [] (juce::Rectangle<int> panel, Knob& a, Knob& b)
    {
        auto inner = panel.reduced (6).withTrimmedTop (titleHeight + 4);
        a.slider.setBounds (inner.removeFromLeft (inner.getWidth() / 2));
        b.slider.setBounds (inner);
    };

    placePair (lrPanel, left, right);
    placePair (msPanel, mid, side);
}
