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

    // Per-plugin colour scheme; the defaults are the neutral grey look.
    struct Palette
    {
        juce::Colour background   = colours::backgroundBottom;
        juce::Colour panel        = colours::panel;
        juce::Colour panelOutline = colours::panelOutline;
        juce::Colour track        = colours::track;
        juce::Colour knobTop      { 0xff3a414b };
        juce::Colour knobBottom   { 0xff1c2026 };
        juce::Colour text         = colours::text;
        juce::Colour textDim      = colours::textDim;
        juce::Colour accent       = colours::accentBlue;
    };

    inline juce::Font font (float height, bool bold = false)
    {
        juce::Font f { juce::FontOptions (height) };
        return bold ? f.boldened() : f;
    }

    // Slider properties understood by the look and feel.
    inline const juce::Identifier arcFromStartProperty { "klaud_arcFromStart" };  // value arc starts at the minimum
    inline const juce::Identifier noFillProperty       { "klaud_noFill" };        // linear slider: thumb only

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        explicit LookAndFeel (Palette p = {}) : palette (p)
        {
            setColour (juce::ResizableWindow::backgroundColourId, palette.background);

            setColour (juce::Slider::textBoxTextColourId,       palette.text);
            setColour (juce::Slider::textBoxOutlineColourId,    juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxHighlightColourId,  palette.accent.withAlpha (0.4f));
            setColour (juce::Slider::trackColourId,             palette.accent);
            setColour (juce::Slider::rotarySliderFillColourId,  palette.accent);

            setColour (juce::Label::textColourId,               palette.text);
            setColour (juce::Label::textWhenEditingColourId,    palette.text);
            setColour (juce::TextEditor::backgroundColourId,    palette.background);
            setColour (juce::TextEditor::textColourId,          palette.text);
            setColour (juce::TextEditor::outlineColourId,       palette.panelOutline);
            setColour (juce::TextEditor::highlightColourId,     palette.accent.withAlpha (0.4f));
            setColour (juce::TextEditor::focusedOutlineColourId, palette.accent.withAlpha (0.6f));
            setColour (juce::CaretComponent::caretColourId,     palette.text);

            setColour (juce::ComboBox::backgroundColourId,      palette.panel.brighter (0.06f));
            setColour (juce::ComboBox::outlineColourId,         palette.panelOutline);
            setColour (juce::ComboBox::textColourId,            palette.text);
            setColour (juce::ComboBox::arrowColourId,           palette.textDim);

            setColour (juce::PopupMenu::backgroundColourId,            palette.panel);
            setColour (juce::PopupMenu::textColourId,                  palette.text);
            setColour (juce::PopupMenu::headerTextColourId,            palette.textDim);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, palette.accent.withAlpha (0.25f));
            setColour (juce::PopupMenu::highlightedTextColourId,       palette.text);

            setColour (juce::TextButton::buttonColourId,   palette.panel);
            setColour (juce::TextButton::buttonOnColourId, palette.accent);
            setColour (juce::TextButton::textColourOffId,  palette.textDim);
            setColour (juce::TextButton::textColourOnId,   palette.background);

            setColour (juce::AlertWindow::backgroundColourId, palette.panel);
            setColour (juce::AlertWindow::textColourId,       palette.text);
            setColour (juce::AlertWindow::outlineColourId,    palette.panelOutline);
        }

        const Palette& getPalette() const { return palette; }

        // ---- rotary knobs ----------------------------------------------------------------------

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
            g.setColour (palette.track);
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
                g.setColour (palette.textDim);
                g.drawLine ({ tickStart, tickEnd }, 1.2f);
            }

            // Knob body.
            const float bodyRadius = juce::jmax (4.0f, arcRadius - 8.0f);
            const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);

            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillEllipse (body.translated (0.0f, 2.0f).expanded (1.0f));

            g.setGradientFill (juce::ColourGradient (active ? palette.knobTop.brighter (0.12f) : palette.knobTop,
                                                     centre.x, body.getY(),
                                                     palette.knobBottom, centre.x, body.getBottom(), false));
            g.fillEllipse (body);

            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.drawEllipse (body.reduced (0.5f), 1.0f);

            // Pointer.
            juce::Path pointer;
            pointer.addRoundedRectangle (-1.25f, -bodyRadius + 4.0f, 2.5f, bodyRadius * 0.42f, 1.25f);
            g.setColour (palette.text);
            g.fillPath (pointer, juce::AffineTransform::rotation (angle).translated (centre));
        }

        // ---- linear sliders and number boxes ---------------------------------------------------

        int getSliderThumbRadius (juce::Slider&) override { return 7; }

        void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                               float, float, juce::Slider::SliderStyle, juce::Slider& slider) override
        {
            const auto accent = slider.findColour (juce::Slider::trackColourId);
            const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();

            if (slider.isBar())
            {
                const auto box = bounds.reduced (0.5f);
                g.setColour (palette.background.withAlpha (0.7f));
                g.fillRoundedRectangle (box, 3.0f);
                g.setColour (accent.withAlpha (slider.isMouseOverOrDragging() ? 0.9f : 0.45f));
                g.drawRoundedRectangle (box, 3.0f, 1.0f);
                return;
            }

            const bool horizontal = slider.isHorizontal();
            juce::Point<float> start, end, thumb;

            if (horizontal)
            {
                const float cy = bounds.getCentreY();
                start = { bounds.getX(), cy };
                end   = { bounds.getRight(), cy };
                thumb = { sliderPos, cy };
            }
            else
            {
                const float cx = bounds.getCentreX();
                start = { cx, bounds.getBottom() };
                end   = { cx, bounds.getY() };
                thumb = { cx, sliderPos };
            }

            const juce::PathStrokeType stroke (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

            juce::Path track;
            track.startNewSubPath (start);
            track.lineTo (end);
            g.setColour (palette.track);
            g.strokePath (track, stroke);

            if (! (bool) slider.getProperties().getWithDefault (noFillProperty, false))
            {
                juce::Path fill;
                fill.startNewSubPath (start);
                fill.lineTo (thumb);
                g.setColour (accent);
                g.strokePath (fill, stroke);
            }

            const float thumbRadius = 6.5f;
            const auto thumbArea = juce::Rectangle<float> (thumbRadius * 2.0f, thumbRadius * 2.0f).withCentre (thumb);

            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.fillEllipse (thumbArea.translated (0.0f, 1.5f));
            g.setGradientFill (juce::ColourGradient (palette.knobTop.brighter (slider.isMouseOverOrDragging() ? 0.2f : 0.05f),
                                                     thumb.x, thumbArea.getY(),
                                                     palette.knobBottom, thumb.x, thumbArea.getBottom(), false));
            g.fillEllipse (thumbArea);
            g.setColour (accent);
            g.drawEllipse (thumbArea.reduced (0.75f), 1.5f);
        }

        juce::Label* createSliderTextBox (juce::Slider& slider) override
        {
            auto* label = juce::LookAndFeel_V4::createSliderTextBox (slider);
            label->setFont (font (13.0f));
            label->setJustificationType (juce::Justification::centred);
            label->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
            label->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
            return label;
        }

        juce::Font getLabelFont (juce::Label& label) override { return label.getFont(); }

        // ---- combo boxes and menus -------------------------------------------------------------

        void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
        {
            const auto area = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);

            g.setColour (box.findColour (juce::ComboBox::backgroundColourId).brighter (box.isMouseOver (true) ? 0.08f : 0.0f));
            g.fillRoundedRectangle (area, 4.0f);
            g.setColour (box.findColour (juce::ComboBox::outlineColourId).brighter (box.isMouseOver (true) ? 0.35f : 0.0f));
            g.drawRoundedRectangle (area, 4.0f, 1.0f);

            const float cx = (float) width - 11.0f, cy = (float) height * 0.5f;
            juce::Path chevron;
            chevron.startNewSubPath (cx - 3.5f, cy - 1.5f);
            chevron.lineTo (cx, cy + 2.0f);
            chevron.lineTo (cx + 3.5f, cy - 1.5f);
            g.setColour (box.findColour (juce::ComboBox::arrowColourId));
            g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        juce::Font getComboBoxFont (juce::ComboBox&) override { return font (12.5f); }

        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            label.setBounds (6, 1, box.getWidth() - 24, box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
        }

        juce::Font getPopupMenuFont() override { return font (13.5f); }

        // ---- buttons ---------------------------------------------------------------------------

        void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                   bool highlighted, bool down) override
        {
            const auto area = button.getLocalBounds().toFloat().reduced (0.5f);

            if (button.getToggleState())
            {
                g.setColour (button.findColour (juce::TextButton::buttonOnColourId).withAlpha (highlighted ? 1.0f : 0.85f));
                g.fillRoundedRectangle (area, 4.0f);
                return;
            }

            g.setColour (palette.panel.brighter (down ? 0.2f : highlighted ? 0.12f : 0.05f));
            g.fillRoundedRectangle (area, 4.0f);
            g.setColour (palette.panelOutline.brighter (highlighted ? 0.4f : 0.0f));
            g.drawRoundedRectangle (area, 4.0f, 1.0f);
        }

        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
        {
            return font (juce::jmin (13.0f, (float) buttonHeight * 0.55f), true);
        }

    private:
        // The value arc starts from the double-click default, so the default reads as "untouched".
        static float homePosition (juce::Slider& slider)
        {
            if ((bool) slider.getProperties().getWithDefault (arcFromStartProperty, false)
                || ! slider.isDoubleClickReturnEnabled())
                return 0.0f;

            return (float) slider.valueToProportionOfLength (slider.getDoubleClickReturnValue());
        }

        Palette palette;
    };
}
