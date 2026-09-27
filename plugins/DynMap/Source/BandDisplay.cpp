#include "BandDisplay.h"

namespace dynmap::ui
{
namespace
{
    constexpr float minFreq = 20.0f, maxFreq = 20000.0f;
    constexpr float gainRange = 24.0f;              // band lines: +-24 dB over the height
    constexpr float spectrumFloor = -90.0f;
    constexpr float minCrossoverRatio = 1.08f;      // neighbouring crossovers keep this distance

    // Every per-stage parameter copied when a band is split (not solo/mute or the crossover).
    const char* const stageParamNames[] { ids::bypass, ids::mode, ids::pre, ids::post, ids::mix, ids::attack, ids::hold,
                                          ids::release, ids::relShape, ids::rms, ids::lookahead, ids::link, ids::stereo,
                                          ids::scFilter, ids::scSource, ids::trTime, ids::maxBoost, ids::maxCut, ids::smooth,
                                          ids::satType, ids::drive, ids::satPos, ids::width };

    juce::String slopeText (int slope) { return juce::String (6 << slope); }
}

//==============================================================================
namespace bands
{
    void copyStage (DynMapProcessor& p, int fromStage, int toStage)
    {
        for (const char* name : stageParamNames)
        {
            auto* from = p.apvts.getParameter (stageParamId (fromStage, name));
            auto* to = p.apvts.getParameter (stageParamId (toStage, name));

            if (from != nullptr && to != nullptr)
            {
                to->beginChangeGesture();
                to->setValueNotifyingHost (from->getValue());
                to->endChangeGesture();
            }
        }

        for (auto kind : { CurveKind::level, CurveKind::transient })
            p.engine.curves.set (toStage, kind, p.engine.curves.get (fromStage, kind));
    }

    int split (DynMapProcessor& p, float freq)
    {
        const auto layout = p.engine.getLayout();
        int position = 0;
        for (int i = 1; i < layout.numBands; ++i)
            if (freq >= layout.freqs[(size_t) i])
                position = i;

        // Keep a little room from the neighbouring crossovers.
        const float low = position > 0 ? layout.freqs[(size_t) position] : minFreq;
        const float high = position + 1 < layout.numBands ? layout.freqs[(size_t) position + 1] : maxFreq;
        if (freq < low * minCrossoverRatio || freq > high / minCrossoverRatio)
            return -1;

        int freeSlot = -1;
        for (int slot = 1; slot < maxBands && freeSlot < 0; ++slot)
            if (p.apvts.getRawParameterValue (stageParamId (bandStage (slot), ids::bandOn))->load() < 0.5f)
                freeSlot = slot;

        if (freeSlot < 0)
            return -1;

        const int source = bandStage (layout.slots[(size_t) position]);
        const int target = bandStage (freeSlot);

        copyStage (p, source, target);
        setParameter (p.apvts, stageParamId (target, ids::solo), 0.0f);
        setParameter (p.apvts, stageParamId (target, ids::mute), 0.0f);
        setParameter (p.apvts, stageParamId (target, ids::freq), freq);
        setParameter (p.apvts, stageParamId (target, ids::bandOn), 1.0f);
        return target;
    }

    int remove (DynMapProcessor& p, int slot)
    {
        const auto layout = p.engine.getLayout();
        const int position = layout.positionOf (slot);

        if (layout.numBands <= 1 || position < 0)
            return bandStage (slot);

        if (slot == 0)
        {
            // The lowest band has no crossover of its own: the band above takes over its range.
            const int upper = layout.slots[1];
            copyStage (p, bandStage (upper), bandStage (0));
            setParameter (p.apvts, stageParamId (bandStage (0), ids::solo), p.apvts.getRawParameterValue (stageParamId (bandStage (upper), ids::solo))->load());
            setParameter (p.apvts, stageParamId (bandStage (0), ids::mute), p.apvts.getRawParameterValue (stageParamId (bandStage (upper), ids::mute))->load());
            setParameter (p.apvts, stageParamId (bandStage (upper), ids::bandOn), 0.0f);
            return bandStage (0);
        }

        setParameter (p.apvts, stageParamId (bandStage (slot), ids::bandOn), 0.0f);
        return bandStage (layout.slots[(size_t) position - 1]);
    }

    void reset (DynMapProcessor& p, int stage)
    {
        for (const char* name : stageParamNames)
            if (auto* param = p.apvts.getParameter (stageParamId (stage, name)))
                setParameter (p.apvts, param->getParameterID(), param->convertFrom0to1 (param->getDefaultValue()));

        p.engine.curves.set (stage, CurveKind::level, Curve (CurveKind::level));
        p.engine.curves.set (stage, CurveKind::transient, Curve (CurveKind::transient));
    }
}

//==============================================================================
BandDisplay::BandDisplay (DynMapProcessor& p, std::function<int()> getSelectedStage, std::function<void (int)> selectStage)
    : processor (p), getSelected (std::move (getSelectedStage)), select (std::move (selectStage))
{
    setTooltip ("Bands: double-click (or click +) to split, drag a crossover to move it, double-click a crossover to remove it, "
                "drag a band's line to set its level, click the slope badge to change the slope, right-click for more.");
    processor.engine.preSpectrum.setEnabled (true);
    processor.engine.postSpectrum.setEnabled (true);
    layout = processor.engine.getLayout();
    startTimerHz (30);
}

BandDisplay::~BandDisplay()
{
    processor.engine.preSpectrum.setEnabled (false);
    processor.engine.postSpectrum.setEnabled (false);
}

juce::RangedAudioParameter* BandDisplay::param (int stage, const char* name) const
{
    return processor.apvts.getParameter (stageParamId (stage, name));
}

juce::Rectangle<float> BandDisplay::plotArea() const
{
    return getLocalBounds().toFloat().withTrimmedBottom (16.0f).withTrimmedRight (26.0f);
}

float BandDisplay::xForFreq (float freq) const
{
    const auto a = plotArea();
    return a.getX() + a.getWidth() * std::log (juce::jlimit (minFreq, maxFreq, freq) / minFreq) / std::log (maxFreq / minFreq);
}

float BandDisplay::freqForX (float x) const
{
    const auto a = plotArea();
    return minFreq * std::pow (maxFreq / minFreq, juce::jlimit (0.0f, 1.0f, (x - a.getX()) / a.getWidth()));
}

float BandDisplay::yForDb (float db) const
{
    const auto a = plotArea();
    return a.getCentreY() - juce::jlimit (-gainRange, gainRange, db) / gainRange * (a.getHeight() * 0.5f - 10.0f);
}

float BandDisplay::dbForY (float y) const
{
    const auto a = plotArea();
    return juce::jlimit (-gainRange, gainRange, (a.getCentreY() - y) / (a.getHeight() * 0.5f - 10.0f) * gainRange);
}

float BandDisplay::bandLeft (int position) const
{
    return position == 0 ? plotArea().getX() : xForFreq (layout.freqs[(size_t) position]);
}

float BandDisplay::bandRight (int position) const
{
    return position + 1 >= layout.numBands ? plotArea().getRight() : xForFreq (layout.freqs[(size_t) position + 1]);
}

int BandDisplay::positionAt (float x) const
{
    for (int p = 0; p < layout.numBands; ++p)
        if (x < bandRight (p))
            return p;
    return layout.numBands - 1;
}

juce::Rectangle<float> BandDisplay::badgeArea (int position) const
{
    return juce::Rectangle<float> (24.0f, 15.0f).withCentre ({ xForFreq (layout.freqs[(size_t) position]), plotArea().getY() + 10.0f });
}

juce::Rectangle<float> BandDisplay::plusArea() const
{
    return juce::Rectangle<float> (18.0f, 18.0f).withCentre ({ hoverX, plotArea().getBottom() - 14.0f });
}

BandDisplay::Hit BandDisplay::hitTest (juce::Point<float> pos) const
{
    for (int p = 1; p < layout.numBands; ++p)
        if (badgeArea (p).expanded (2.0f).contains (pos))
            return { Hit::slopeBadge, p };

    for (int p = 1; p < layout.numBands; ++p)
        if (std::abs (pos.x - xForFreq (layout.freqs[(size_t) p])) < 5.0f)
            return { Hit::crossover, p };

    const int position = positionAt (pos.x);
    const int stage = bandStage (layout.slots[(size_t) position]);

    if (auto* post = param (stage, ids::post))
    {
        const float lineY = yForDb (post->convertFrom0to1 (post->getValue()));
        if (std::abs (pos.y - lineY) < 6.0f)
            return { Hit::level, position };
    }

    if (hoverX >= 0.0f && layout.numBands < maxBands && plusArea().expanded (2.0f).contains (pos))
        return { Hit::plus, position };

    return { Hit::band, position };
}

void BandDisplay::timerCallback()
{
    processor.engine.preSpectrum.process();
    processor.engine.postSpectrum.process();

    layout = processor.engine.getLayout();

    for (int p = 0; p < layout.numBands; ++p)
    {
        const int slot = layout.slots[(size_t) p];
        const float target = processor.engine.getStage (bandStage (slot)).meter.gainDb.load();
        auto& shown = shownGain[(size_t) slot];
        shown += (std::abs (target) > std::abs (shown) ? 0.6f : 0.2f) * (target - shown);
    }

    repaint();
}

void BandDisplay::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto a = plotArea();
    const int selected = getSelected();

    g.setColour (pal.background.withAlpha (0.7f));
    g.fillRoundedRectangle (a, 5.0f);

    // Frequency and gain grid.
    g.setFont (klaud::font (9.5f));
    for (float f : { 30.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xForFreq (f);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
        g.setColour (pal.textDim.withAlpha (0.8f));
        g.drawText (f >= 1000.0f ? juce::String ((int) (f / 1000.0f)) + "k" : juce::String ((int) f),
                    (int) x - 16, (int) a.getBottom() + 2, 32, 12, juce::Justification::centred, false);
    }

    for (float db : { -18.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 18.0f })
    {
        const float y = yForDb (db);
        g.setColour (juce::Colours::white.withAlpha (db == 0.0f ? 0.09f : 0.035f));
        g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
        g.setColour (pal.textDim.withAlpha (0.7f));
        g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), (int) a.getRight() + 2, (int) y - 6, 24, 12, juce::Justification::centredLeft, false);
    }

    // Spectrum: signal into the bands (filled) and the output (line).
    {
        auto spectrumY = [&] (float db) { return a.getBottom() - juce::jlimit (0.0f, 1.0f, (db - spectrumFloor) / -spectrumFloor) * a.getHeight(); };
        juce::Path pre, post;
        pre.startNewSubPath (a.getX(), a.getBottom());

        for (float x = a.getX(); x <= a.getRight(); x += 2.0f)
        {
            const float f = freqForX (x);
            pre.lineTo (x, spectrumY (processor.engine.preSpectrum.getLevelAt (f)));
            const float yPost = spectrumY (processor.engine.postSpectrum.getLevelAt (f));
            if (x == a.getX()) post.startNewSubPath (x, yPost); else post.lineTo (x, yPost);
        }

        pre.lineTo (a.getRight(), a.getBottom());
        pre.closeSubPath();

        g.saveState();
        g.reduceClipRegion (a.toNearestInt());
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.fillPath (pre);
        g.setColour (juce::Colours::white.withAlpha (0.28f));
        g.strokePath (post, juce::PathStrokeType (1.0f));
        g.restoreState();
    }

    // Bands.
    bool anySolo = false;
    for (int p = 0; p < layout.numBands; ++p)
        anySolo = anySolo || param (bandStage (layout.slots[(size_t) p]), ids::solo)->getValue() > 0.5f;

    for (int p = 0; p < layout.numBands; ++p)
    {
        const int slot = layout.slots[(size_t) p];
        const int stage = bandStage (slot);
        const auto colour = stageColour (processor, stage);
        const float left = bandLeft (p), right = bandRight (p);
        const auto region = juce::Rectangle<float> (left, a.getY(), right - left, a.getHeight());
        const bool isSelected = stage == selected;
        const bool muted = param (stage, ids::mute)->getValue() > 0.5f || (anySolo && param (stage, ids::solo)->getValue() < 0.5f);
        const bool bypassed = param (stage, ids::bypass)->getValue() > 0.5f;
        const float dim = muted ? 0.35f : 1.0f;

        g.setColour (colour.withAlpha ((isSelected && layout.numBands > 1 ? 0.10f : 0.035f) * dim));
        g.fillRect (region);

        if (isSelected)
        {
            g.setColour (colour.withAlpha (0.8f));
            g.fillRect (region.withHeight (2.0f));
        }

        // Output level line with the live gain change around it.
        auto* post = param (stage, ids::post);
        const float postDb = post->convertFrom0to1 (post->getValue());
        const float lineY = yForDb (postDb);
        const float gainY = yForDb (postDb + (bypassed ? 0.0f : shownGain[(size_t) slot]));
        const float inset = juce::jmin (6.0f, region.getWidth() * 0.15f);

        g.setColour (colour.withAlpha (0.28f * dim));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (left + inset, juce::jmin (lineY, gainY), right - inset, juce::jmax (lineY, gainY)));

        const bool hot = (hover.type == Hit::level && hover.position == p) || (drag.type == Hit::level && drag.position == p);
        g.setColour (colour.withAlpha ((hot ? 1.0f : 0.75f) * dim));
        g.drawLine (left + inset, lineY, right - inset, lineY, hot ? 2.2f : 1.6f);
        g.fillEllipse (juce::Rectangle<float> (hot ? 10.0f : 8.0f, hot ? 10.0f : 8.0f).withCentre ({ 0.5f * (left + right), lineY }));

        // Label.
        juce::String label = juce::String (p + 1);
        if (param (stage, ids::solo)->getValue() > 0.5f) label << " S";
        if (param (stage, ids::mute)->getValue() > 0.5f) label << " M";
        if (bypassed) label << " off";

        g.setFont (klaud::font (11.0f, true));
        g.setColour (colour.withAlpha (dim));
        g.drawText (label, (int) left + 6, (int) a.getY() + 20, (int) juce::jmax (20.0f, right - left - 8.0f), 14,
                    juce::Justification::centredLeft, true);

        if (drag.type == Hit::level && drag.position == p)
        {
            g.setColour (pal.text);
            g.drawText (formatDb (postDb), (int) left + 6, (int) lineY - 18, (int) (right - left) - 12, 14, juce::Justification::centred, false);
        }
    }

    if (layout.numBands == 1 && hoverX < 0.0f)
    {
        g.setFont (klaud::font (12.0f));
        g.setColour (pal.textDim.withAlpha (0.7f));
        g.drawText ("double-click to split into bands", a.withTrimmedTop (a.getHeight() * 0.62f).toNearestInt(),
                    juce::Justification::centredTop, false);
    }

    // Crossovers.
    for (int p = 1; p < layout.numBands; ++p)
    {
        const float x = xForFreq (layout.freqs[(size_t) p]);
        const bool hot = (hover.position == p && (hover.type == Hit::crossover || hover.type == Hit::slopeBadge))
                         || (drag.type == Hit::crossover && drag.position == p);

        g.setColour (juce::Colours::white.withAlpha (hot ? 0.6f : 0.22f));
        g.drawLine (x, a.getY() + 18.0f, x, a.getBottom(), hot ? 1.6f : 1.0f);

        const auto badge = badgeArea (p);
        g.setColour (pal.panel.brighter (hot ? 0.25f : 0.1f));
        g.fillRoundedRectangle (badge, 3.0f);
        g.setColour (juce::Colours::white.withAlpha (hot ? 0.6f : 0.25f));
        g.drawRoundedRectangle (badge, 3.0f, 1.0f);
        g.setFont (klaud::font (9.5f, true));
        g.setColour (pal.text);
        g.drawText (slopeText (layout.slopes[(size_t) p]), badge.toNearestInt(), juce::Justification::centred, false);

        if (hot)
        {
            g.setFont (klaud::font (10.5f, true));
            g.drawText (formatHz (layout.freqs[(size_t) p]), (int) x - 40, (int) a.getBottom() - 30, 80, 14, juce::Justification::centred, false);
        }
    }

    // "+" to split the band under the mouse.
    if (hoverX >= 0.0f && layout.numBands < maxBands && drag.type == Hit::none
        && hover.type != Hit::crossover && hover.type != Hit::slopeBadge && hover.type != Hit::level)
    {
        const auto plus = plusArea();
        const bool hot = hover.type == Hit::plus;
        g.setColour (pal.panel.brighter (hot ? 0.3f : 0.12f).withAlpha (0.9f));
        g.fillEllipse (plus);
        g.setColour (juce::Colours::white.withAlpha (hot ? 0.9f : 0.5f));
        g.drawEllipse (plus, 1.0f);
        g.drawLine (plus.getCentreX() - 4.0f, plus.getCentreY(), plus.getCentreX() + 4.0f, plus.getCentreY(), 1.5f);
        g.drawLine (plus.getCentreX(), plus.getCentreY() - 4.0f, plus.getCentreX(), plus.getCentreY() + 4.0f, 1.5f);

        if (hot)
        {
            g.setFont (klaud::font (10.5f, true));
            g.drawText ("split at " + formatHz (freqForX (hoverX)), (int) hoverX - 60, (int) plus.getY() - 16, 120, 14,
                        juce::Justification::centred, false);
        }
    }
}

void BandDisplay::mouseMove (const juce::MouseEvent& e)
{
    hoverX = plotArea().contains (e.position) ? e.position.x : -1.0f;
    hover = hitTest (e.position);
    setMouseCursor (hover.type == Hit::crossover ? juce::MouseCursor::LeftRightResizeCursor
                    : hover.type == Hit::level   ? juce::MouseCursor::UpDownResizeCursor
                    : hover.type == Hit::slopeBadge || hover.type == Hit::plus ? juce::MouseCursor::PointingHandCursor
                                                                                : juce::MouseCursor::NormalCursor);
}

void BandDisplay::mouseExit (const juce::MouseEvent&)
{
    hoverX = -1.0f;
    hover = {};
}

void BandDisplay::mouseDown (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);
    drag = {};

    if (h.position >= 0 && (h.type == Hit::band || h.type == Hit::level))
        select (bandStage (layout.slots[(size_t) h.position]));

    if (e.mods.isPopupMenu())
    {
        if (h.type == Hit::slopeBadge || h.type == Hit::crossover)
            showSlopeMenu (h.position);
        else if (h.position >= 0)
            showBandMenu (h.position);
        return;
    }

    switch (h.type)
    {
        case Hit::slopeBadge:
            showSlopeMenu (h.position);
            break;

        case Hit::plus:
        {
            const int stage = bands::split (processor, freqForX (e.position.x));
            if (stage >= 0)
                select (stage);
            break;
        }

        case Hit::crossover:
            dragParam = param (bandStage (layout.slots[(size_t) h.position]), ids::freq);
            break;

        case Hit::level:
            dragParam = param (bandStage (layout.slots[(size_t) h.position]), ids::post);
            break;

        case Hit::band:
        case Hit::none:
            break;
    }

    if (dragParam != nullptr)
    {
        drag = h;
        dragParam->beginChangeGesture();
    }
}

void BandDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragParam == nullptr)
        return;

    if (drag.type == Hit::crossover)
    {
        const int p = drag.position;
        const float low = p > 1 ? layout.freqs[(size_t) p - 1] * minCrossoverRatio : minFreq;
        const float high = p + 1 < layout.numBands ? layout.freqs[(size_t) p + 1] / minCrossoverRatio : maxFreq;
        dragParam->setValueNotifyingHost (dragParam->convertTo0to1 (juce::jlimit (low, high, freqForX (e.position.x))));
    }
    else if (drag.type == Hit::level)
    {
        float db = dbForY (e.position.y);
        if (! e.mods.isShiftDown())
            db = std::round (db * 2.0f) * 0.5f;
        dragParam->setValueNotifyingHost (dragParam->convertTo0to1 (db));
    }
}

void BandDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragParam != nullptr)
        dragParam->endChangeGesture();

    dragParam = nullptr;
    drag = {};
}

void BandDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto h = hitTest (e.position);

    if (h.type == Hit::crossover || h.type == Hit::slopeBadge)
    {
        select (bands::remove (processor, layout.slots[(size_t) h.position]));
    }
    else if (h.type == Hit::level)
    {
        setParameter (processor.apvts, stageParamId (bandStage (layout.slots[(size_t) h.position]), ids::post), 0.0f);
    }
    else if (h.type == Hit::band)
    {
        const int stage = bands::split (processor, freqForX (e.position.x));
        if (stage >= 0)
            select (stage);
    }
}

void BandDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto h = hitTest (e.position);
    if (h.position < 0)
        return;

    auto* post = param (bandStage (layout.slots[(size_t) h.position]), ids::post);
    const float db = post->convertFrom0to1 (post->getValue()) + (wheel.deltaY > 0 ? 0.5f : -0.5f);
    setParameter (processor.apvts, post->getParameterID(), juce::jlimit (-24.0f, 24.0f, db));
}

void BandDisplay::showBandMenu (int position)
{
    const int slot = layout.slots[(size_t) position];
    const int stage = bandStage (slot);

    juce::PopupMenu menu;
    menu.addSectionHeader (stageLabel (processor, stage) + " (" + (position == 0 ? juce::String ("below ") + formatHz (layout.numBands > 1 ? layout.freqs[1] : maxFreq)
                                                                                 : "from " + formatHz (layout.freqs[(size_t) position])) + ")");
    menu.addItem (1, "Solo", true, param (stage, ids::solo)->getValue() > 0.5f);
    menu.addItem (2, "Mute", true, param (stage, ids::mute)->getValue() > 0.5f);
    menu.addItem (3, "Bypass", true, param (stage, ids::bypass)->getValue() > 0.5f);
    menu.addSeparator();
    menu.addItem (4, "Split here", layout.numBands < maxBands);
    menu.addItem (5, "Remove band", layout.numBands > 1);
    menu.addItem (6, "Reset band");

    const float freq = hoverX >= 0.0f ? freqForX (hoverX) : 1000.0f;
    juce::Component::SafePointer<BandDisplay> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safeThis, stage, slot, freq] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        auto& self = *safeThis;
        auto toggle = [&self, stage] (const char* name)
        {
            setParameter (self.processor.apvts, stageParamId (stage, name), self.param (stage, name)->getValue() > 0.5f ? 0.0f : 1.0f);
        };

        switch (result)
        {
            case 1: toggle (ids::solo); break;
            case 2: toggle (ids::mute); break;
            case 3: toggle (ids::bypass); break;
            case 4: { const int s = bands::split (self.processor, freq); if (s >= 0) self.select (s); break; }
            case 5: self.select (bands::remove (self.processor, slot)); break;
            case 6: bands::reset (self.processor, stage); break;
            default: break;
        }
    });
}

void BandDisplay::showSlopeMenu (int position)
{
    const int stage = bandStage (layout.slots[(size_t) position]);
    juce::PopupMenu menu;
    menu.addSectionHeader ("Crossover at " + formatHz (layout.freqs[(size_t) position]));

    for (int s = 0; s < 4; ++s)
        menu.addItem (s + 1, slopeText (s) + " dB/oct", true, layout.slopes[(size_t) position] == s);

    menu.addSeparator();
    menu.addItem (10, "Remove crossover");

    juce::Component::SafePointer<BandDisplay> safeThis (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safeThis, stage] (int result)
    {
        if (safeThis == nullptr || result == 0)
            return;

        if (result <= 4)
            setParameter (safeThis->processor.apvts, stageParamId (stage, ids::slope), (float) (result - 1));
        else if (result == 10)
            safeThis->select (bands::remove (safeThis->processor, stage - 1));
    });
}

//==============================================================================
StageTab::StageTab (DynMapProcessor& p, int s, std::function<int()> getSelectedStage, std::function<void (int)> selectStage)
    : processor (p), stage (s), getSelected (std::move (getSelectedStage)), select (std::move (selectStage))
{
    setTooltip (stage == inputStage ? "Input stage: one full-band map before the band split. Right-click to bypass."
                                    : "Master stage: one full-band map after the bands are summed, before the clipper and limiter. Right-click to bypass.");
    startTimerHz (30);
}

void StageTab::timerCallback()
{
    const float target = processor.engine.getStage (stage).meter.gainDb.load();
    shownGain += (std::abs (target) > std::abs (shownGain) ? 0.6f : 0.2f) * (target - shownGain);
    repaint();
}

void StageTab::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        const auto id = stageParamId (stage, ids::bypass);
        const bool bypassed = processor.apvts.getRawParameterValue (id)->load() > 0.5f;
        juce::PopupMenu menu;
        menu.addItem (1, "Bypass", true, bypassed);
        menu.addItem (2, "Reset");

        juce::Component::SafePointer<StageTab> safeThis (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safeThis, id, bypassed] (int result)
        {
            if (safeThis == nullptr) return;
            if (result == 1) setParameter (safeThis->processor.apvts, id, bypassed ? 0.0f : 1.0f);
            if (result == 2) bands::reset (safeThis->processor, safeThis->stage);
        });
    }

    select (stage);
}

void StageTab::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto colour = stageColour (processor, stage);
    const bool isSelected = getSelected() == stage;
    const bool bypassed = processor.apvts.getRawParameterValue (stageParamId (stage, ids::bypass))->load() > 0.5f;
    auto area = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (isSelected ? colour.withAlpha (0.14f) : pal.panel);
    g.fillRoundedRectangle (area, 5.0f);
    g.setColour (isSelected ? colour.withAlpha (0.8f) : pal.panelOutline);
    g.drawRoundedRectangle (area, 5.0f, 1.0f);

    g.setFont (klaud::font (11.0f, true));
    g.setColour (bypassed ? pal.textDim : colour);
    g.drawText (stage == inputStage ? "INPUT" : "MASTER", area.removeFromTop (26.0f).toNearestInt(), juce::Justification::centred, false);

    if (bypassed)
    {
        g.setFont (klaud::font (10.0f));
        g.drawText ("off", area.removeFromBottom (18.0f).toNearestInt(), juce::Justification::centred, false);
    }

    // Live gain around a 0 dB line (+-24 dB).
    const auto meter = area.reduced (area.getWidth() * 0.5f - 5.0f, 12.0f);
    g.setColour (pal.track);
    g.fillRoundedRectangle (meter, 3.0f);

    const float zero = meter.getCentreY();
    const float y = zero - juce::jlimit (-24.0f, 24.0f, bypassed ? 0.0f : shownGain) / 24.0f * meter.getHeight() * 0.5f;
    g.setColour ((shownGain < 0.0f ? colours::cut : colours::boost).withAlpha (0.9f));
    g.fillRect (juce::Rectangle<float>::leftTopRightBottom (meter.getX(), juce::jmin (zero, y), meter.getRight(), juce::jmax (zero, y)));
    g.setColour (juce::Colours::white.withAlpha (0.4f));
    g.drawHorizontalLine ((int) zero, meter.getX() - 3.0f, meter.getRight() + 3.0f);

    g.setFont (klaud::font (10.0f));
    g.setColour (pal.textDim);
    g.drawText (juce::String (shownGain, 1), getLocalBounds().removeFromBottom (bypassed ? 34 : 18), juce::Justification::centred, false);
}
}
