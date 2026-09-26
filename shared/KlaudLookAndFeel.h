#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Dark, flat look shared by the Maki plugins (loosely FabFilter-inspired):
// a thin value arc drawn from the knob's default position, a soft gradient knob
// body and small uppercase labels.
namespace klaud
{
    namespace colours
    {
        const juce::Colour backgroundTop    { 0xff20252d };
        const juce::Colour backgroundBottom { 0xff14171c };
        const juce::Colour panel            { 0xff1a1e24 };
        const juce::Colour panelOutline     { 0xff2a3039 };
        const juce::Colour track            { 0xff2c323b };
        const juce::Colour text             { 0xffd6dbe2 };
        const juce::Colour textDim          { 0xff7d8794 };
        const juce::Colour accentBlue       { 0xff5ab4f0 };
        const juce::Colour accentAmber      { 0xfff0b35a };
    }

    inline juce::Font font (float height, bool bold = false)
    {
        juce::Font f { juce::FontOptions (height) };
        return bold ? f.boldened() : f;
    }

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel()
        {
            setColour (juce::Slider::textBoxTextColourId,       colours::text);
            setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxHighlightColourId,  colours::accentBlue.withAlpha (0.4f));
            setColour (juce::Label::textWhenEditingColourId,    colours::text);
            setColour (juce::TextEditor::highlightColourId,     colours::accentBlue.withAlpha (0.4f));
            setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::CaretComponent::caretColourId,     colours::text);
        }

        void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                               float startAngle, float endAngle, juce::Slider& slider) override
        {
            const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
            const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
            const auto centre = bounds.getCentre();
            const auto accent = slider.findColour (juce::Slider::rotarySliderFillColourId);
            const bool active = slider.isMouseOverOrDragging() && slider.isEnabled();

            const float angle     = startAngle + sliderPos * (endAngle - startAngle);
            const float homeAngle = startAngle + homePosition (slider) * (endAngle - startAngle);

            // Track and value arc.
            const float arcRadius = radius - 3.0f;
            const juce::PathStrokeType stroke (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

            juce::Path track;
            track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
            g.setColour (colours::track);
            g.strokePath (track, stroke);

            if (std::abs (angle - homeAngle) > 0.001f)
            {
                juce::Path value;
                value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                     juce::jmin (angle, homeAngle), juce::jmax (angle, homeAngle), true);
                g.setColour (active ? accent.brighter (0.25f) : accent);
                g.strokePath (value, stroke);
            }

            // Tick at the default position.
            {
                const auto tickStart = centre.getPointOnCircumference (arcRadius + 4.0f, homeAngle);
                const auto tickEnd   = centre.getPointOnCircumference (arcRadius + 7.0f, homeAngle);
                g.setColour (colours::textDim);
                g.drawLine ({ tickStart, tickEnd }, 1.2f);
            }

            // Knob body.
            const float bodyRadius = arcRadius - 8.0f;
            const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);

            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillEllipse (body.translated (0.0f, 2.0f).expanded (1.0f));

            g.setGradientFill (juce::ColourGradient (juce::Colour (active ? 0xff444c57 : 0xff3a414b), centre.x, body.getY(),
                                                     juce::Colour (0xff1c2026), centre.x, body.getBottom(), false));
            g.fillEllipse (body);

            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.drawEllipse (body.reduced (0.5f), 1.0f);

            // Pointer.
            juce::Path pointer;
            pointer.addRoundedRectangle (-1.25f, -bodyRadius + 4.0f, 2.5f, bodyRadius * 0.42f, 1.25f);
            g.setColour (colours::text);
            g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
        }

        juce::Label* createSliderTextBox (juce::Slider& slider) override
        {
            auto* label = juce::LookAndFeel_V4::createSliderTextBox (slider);
            label->setFont (font (13.0f));
            label->setJustificationType (juce::Justification::centred);
            return label;
        }

        juce::Font getLabelFont (juce::Label& label) override
        {
            return label.getFont();
        }

    private:
        // The value arc starts from the double-click default, so 100 % reads as "untouched".
        static float homePosition (juce::Slider& slider)
        {
            if (! slider.isDoubleClickReturnEnabled())
                return 0.0f;

            return (float) slider.valueToProportionOfLength (slider.getDoubleClickReturnValue());
        }
    };
}
