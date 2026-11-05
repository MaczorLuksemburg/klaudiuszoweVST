#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "KlaudLookAndFeel.h"

namespace klaud
{
    // A short choice parameter (two or three options) as one row of segments: every option is visible and
    // one click picks it, instead of a menu. setVertical (true) stacks the segments as a column instead. The selected segment is filled with TextButton::buttonOnColourId
    // (set it on this component for a per-module accent), its text in TextButton::textColourOnId.
    class ChoiceButtons : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        ChoiceButtons() { setRepaintsOnMouseActivity (true); }

        // `shortLabels` replace the parameter's choice names (same order) where those are too long to fit.
        void attach (juce::RangedAudioParameter& param, const juce::StringArray& shortLabels = {})
        {
            attachment.reset();

            labels = shortLabels;
            if (labels.isEmpty())
                if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&param))
                    labels = choice->choices;

            jassert (labels.size() >= 2);
            attachment = std::make_unique<juce::ParameterAttachment> (param, [this] (float value)
            {
                selected = juce::roundToInt (value);
                repaint();
            });
            attachment->sendInitialUpdate();
        }

        // Without a parameter (apps): plain options, the selection is reported through onChange.
        void setOptions (const juce::StringArray& options, int initiallySelected = 0)
        {
            attachment.reset();
            labels = options;
            selected = initiallySelected;
            repaint();
        }

        void setSelected (int index) { if (index != selected) { selected = index; repaint(); } }  // no onChange
        std::function<void (int)> onChange;

        int getSelected() const { return selected; }

        // Stack the segments top to bottom, each the same height (e.g. next to a knob).
        void setVertical (bool shouldBeVertical) { vertical = shouldBeVertical; repaint(); }

        void paint (juce::Graphics& g) override
        {
            const int n = labels.size();
            if (n == 0)
                return;

            const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
            const auto outline = findColour (juce::ComboBox::outlineColourId);
            const auto accent = findColour (juce::TextButton::buttonOnColourId);
            const int hovered = isMouseOver() && isEnabled() ? segmentAt (getMouseXYRelative()) : -1;

            if (! isEnabled())
                g.beginTransparencyLayer (0.5f);

            juce::Path shape;
            shape.addRoundedRectangle (bounds, 4.0f);
            g.setColour (findColour (juce::ComboBox::backgroundColourId));
            g.fillPath (shape);

            {
                juce::Graphics::ScopedSaveState clip (g);
                g.reduceClipRegion (shape);

                for (int i = 0; i < n; ++i)
                {
                    const auto r = segment (i);

                    if (i == selected)
                        g.setColour (accent.withAlpha (hovered == i ? 1.0f : 0.85f));
                    else
                        g.setColour (juce::Colours::white.withAlpha (hovered == i ? 0.06f : 0.0f));

                    g.fillRect (r);

                    // Dividers only between two unselected segments: the filled one has its own edge.
                    if (i > 0 && i != selected && i - 1 != selected)
                    {
                        g.setColour (outline);
                        g.fillRect (vertical ? r.withHeight (1.0f) : r.withWidth (1.0f));
                    }
                }
            }

            g.setColour (outline.brighter (hovered >= 0 ? 0.3f : 0.0f));
            g.strokePath (shape, juce::PathStrokeType (1.0f));

            const float height = textHeight();
            for (int i = 0; i < n; ++i)
            {
                const bool on = i == selected;
                g.setFont (font (height, on));
                g.setColour (on ? findColour (juce::TextButton::textColourOnId)
                                : hovered == i ? findColour (juce::ComboBox::textColourId)
                                               : findColour (juce::TextButton::textColourOffId).brighter (0.25f));
                g.drawFittedText (labels[i], segment (i).toNearestInt().reduced (3, 0), juce::Justification::centred, 1, 0.85f);
            }

            if (! isEnabled())
                g.endTransparencyLayer();
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int index = segmentAt (e.getPosition());
            if (index < 0 || index == selected || ! isEnabled())
                return;
            if (attachment != nullptr)
                attachment->setValueAsCompleteGesture ((float) index);
            else
            {
                selected = index;
                repaint();
                if (onChange)
                    onChange (index);
            }
        }

    private:
        static constexpr float padding = 10.0f;

        float labelsWidth (float height) const
        {
            float total = 0.0f;
            for (auto& label : labels)
                total += juce::GlyphArrangement::getStringWidth (font (height, true), label);
            return total;
        }

        // 12 px text, smaller (down to 9.5 px) when the labels wouldn't fit side by side.
        float textHeight() const
        {
            if (vertical)
            {
                float widest = 0.0f;
                for (auto& label : labels)
                    widest = juce::jmax (widest, juce::GlyphArrangement::getStringWidth (font (12.0f, true), label));
                const float room = (float) getWidth() - padding;
                return widest <= room || widest <= 0.0f ? 12.0f : juce::jmax (9.5f, 12.0f * room / widest);
            }

            const float room = (float) getWidth() - padding * (float) labels.size(), needed = labelsWidth (12.0f);
            return needed <= room || needed <= 0.0f ? 12.0f : juce::jmax (9.5f, 12.0f * room / needed);
        }

        // Each segment is its label's width plus an equal share of the rest, so a long label isn't cut short.
        float edge (int i) const
        {
            const int n = labels.size();
            const auto f = font (textHeight(), true);
            float total = 0.0f, before = 0.0f;
            for (int k = 0; k < n; ++k)
            {
                const float w = juce::GlyphArrangement::getStringWidth (f, labels[k]) + padding;
                total += w;
                if (k < i)
                    before += w;
            }

            const float spare = (float) getWidth() - total;
            return spare >= 0.0f ? before + spare * (float) i / (float) n
                                 : before * (float) getWidth() / total;
        }

        juce::Rectangle<float> segment (int i) const
        {
            if (vertical)
            {
                const float h = (float) getHeight() / (float) juce::jmax (1, labels.size());
                const float top = std::round (h * (float) i);
                return { 0.0f, top, (float) getWidth(), std::round (h * (float) (i + 1)) - top };
            }

            const float left = std::round (edge (i));
            return { left, 0.0f, std::round (edge (i + 1)) - left, (float) getHeight() };
        }

        int segmentAt (juce::Point<int> position) const
        {
            for (int i = 0; i < labels.size(); ++i)
                if (vertical ? (float) position.y < segment (i).getBottom() : (float) position.x < segment (i).getRight())
                    return i;
            return labels.size() - 1;
        }

        juce::StringArray labels;
        int selected = 0;
        bool vertical = false;
        std::unique_ptr<juce::ParameterAttachment> attachment;
    };
}
