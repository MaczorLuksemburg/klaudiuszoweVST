#include "Components.h"

namespace waveref::ui
{
namespace
{
    const char* const channelNames[] { "MIX MID", "MIX SIDE", "REF MID", "REF SIDE" };
    const char* const channelTitles[] { "Mix mid", "Mix side", "Ref mid", "Ref side" };

    juce::String formatFreq (float hz)
    {
        return hz < 1000.0f ? juce::String (juce::roundToInt (hz)) + " Hz"
                            : juce::String (hz / 1000.0f, hz < 10000.0f ? 2 : 1) + " kHz";
    }

    juce::String formatTime (double seconds)
    {
        const int minutes = (int) (seconds / 60.0);
        const double rest = seconds - minutes * 60.0;
        return juce::String (minutes) + ":" + (rest < 10.0 ? "0" : "") + juce::String (rest, 2);
    }

    juce::String formatLufs (float lufs)
    {
        return lufs <= -100.0f ? juce::String ("--") : juce::String (lufs, 1);
    }

    void setParameter (APVTS& apvts, const char* id, float plainValue)
    {
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
            p->endChangeGesture();
        }
    }

    float getParameter (APVTS& apvts, const char* id)
    {
        return apvts.getRawParameterValue (id)->load();
    }

    juce::File storageFolderOverride;

    struct FactoryPreset
    {
        const char* name;
        std::vector<std::pair<const char*, double>> values;
        bool resetColours;
    };

    const std::vector<FactoryPreset>& factoryPresets()
    {
        static const std::vector<FactoryPreset> presets
        {
            { "Default", { { "fftOrder", 13 }, { "speed", 0.4 }, { "smooth", 6 }, { "tilt", 4.5 }, { "span", 90 }, { "top", 0 },
                           { "fill", 1 }, { "diffView", 0 }, { "diffSpan", 12 },
                           { "show0", 1 }, { "show1", 1 }, { "show2", 1 }, { "show3", 1 } }, true },
            { "EDM (+9 dB top)", { { "top", 9 }, { "span", 90 }, { "smooth", 3 }, { "tilt", 4.5 }, { "fftOrder", 13 },
                                   { "speed", 0.3 }, { "diffView", 0 } }, false },
            { "Bass detail", { { "fftOrder", 15 }, { "smooth", 12 }, { "tilt", 3 }, { "speed", 0.6 }, { "diffView", 0 } }, false },
            { "Fast and rough", { { "fftOrder", 11 }, { "smooth", 6 }, { "speed", 0.08 }, { "diffView", 0 } }, false },
            { "Mix minus reference", { { "diffView", 1 }, { "diffSpan", 12 }, { "smooth", 3 }, { "fftOrder", 13 }, { "speed", 0.4 } }, false }
        };

        return presets;
    }

    const int smoothingDivisors[] { 0, 24, 12, 6, 3, 1 };
    const float tiltValues[] { 0.0f, 3.0f, 4.5f, 6.0f };
    const float spanValues[] { 60.0f, 90.0f, 120.0f };
    const float diffSpanValues[] { 6.0f, 12.0f, 24.0f, 36.0f };
    const float topValues[] { 0.0f, 3.0f, 6.0f, 9.0f, 12.0f };
}

const klaud::Palette& palette()
{
    static const klaud::Palette p = []
    {
        klaud::Palette q;
        q.background = juce::Colour (0xff12151b);
        q.panel = juce::Colour (0xff191d25);
        q.panelOutline = juce::Colour (0xff272d38);
        q.accent = juce::Colour (0xff8c9eff);
        return q;
    }();

    return p;
}

void drawCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, juce::Justification justification)
{
    g.setColour (palette().textDim);
    g.setFont (klaud::font (10.5f, true));
    g.drawText (text.toUpperCase(), area, justification, false);
}

bool isAudioFile (const juce::File& file)
{
    return file.hasFileExtension ("wav;wave;aif;aiff;flac;ogg;mp3");
}

//==============================================================================
// UiSettings

UiSettings::UiSettings (juce::ValueTree uiState) : tree (std::move (uiState))
{
    if (! tree.hasProperty ("initialised"))
    {
        loadGlobal();
        tree.setProperty ("initialised", true, nullptr);
    }
}

juce::Colour UiSettings::getDefaultColour (int channel)
{
    static const juce::Colour defaults[] { juce::Colour (0xffff5a4d),    // mix mid: red
                                           juce::Colour (0xffffa63d),    // mix side: orange
                                           juce::Colour (0xff3d7bff),    // ref mid: blue
                                           juce::Colour (0xff7fd4ff) };  // ref side: light blue
    return defaults[channel];
}

juce::Colour UiSettings::getColour (int channel) const
{
    const auto key = "col" + juce::String (channel);
    return tree.hasProperty (key) ? juce::Colour ((juce::uint32) (juce::int64) tree[key]) : getDefaultColour (channel);
}

void UiSettings::setColour (int channel, juce::Colour colour)
{
    set (("col" + juce::String (channel)).toRawUTF8(), (juce::int64) colour.getARGB());
}

void UiSettings::resetColours()
{
    for (int c = 0; c < SpectrumAnalyser::numChannels; ++c)
        tree.removeProperty ("col" + juce::String (c), nullptr);

    saveGlobal();
}

bool UiSettings::isVisible (int channel) const
{
    return (bool) tree.getProperty ("show" + juce::String (channel), true);
}

void UiSettings::setVisible (int channel, bool shouldShow)
{
    set (("show" + juce::String (channel)).toRawUTF8(), shouldShow);
}

void UiSettings::set (const char* key, const juce::var& value)
{
    tree.setProperty (key, value, nullptr);
    saveGlobal();
}

juce::File UiSettings::getStorageFolder()
{
    if (storageFolderOverride != juce::File())
        return storageFolderOverride;

    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("Maki plugins").getChildFile ("WaveRef");
}

void UiSettings::setStorageFolder (const juce::File& folder) { storageFolderOverride = folder; }
juce::File UiSettings::getPresetFolder() { return getStorageFolder().getChildFile ("Presets"); }
juce::File UiSettings::getGlobalFile()    { return getStorageFolder().getChildFile ("Settings.xml"); }

std::unique_ptr<juce::XmlElement> UiSettings::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement> ("WaveRefSettings");

    for (int c = 0; c < SpectrumAnalyser::numChannels; ++c)
    {
        xml->setAttribute ("col" + juce::String (c), juce::String ((juce::int64) getColour (c).getARGB()));
        xml->setAttribute ("show" + juce::String (c), isVisible (c) ? 1 : 0);
    }

    xml->setAttribute ("fftOrder", fftOrder());
    xml->setAttribute ("speed", (double) speed());
    xml->setAttribute ("smooth", smoothing());
    xml->setAttribute ("tilt", (double) tilt());
    xml->setAttribute ("span", (double) span());
    xml->setAttribute ("top", (double) top());
    xml->setAttribute ("fill", fill() ? 1 : 0);
    xml->setAttribute ("diffView", diffView() ? 1 : 0);
    xml->setAttribute ("diffSpan", (double) diffSpan());
    return xml;
}

void UiSettings::applyXml (const juce::XmlElement& xml)
{
    for (int c = 0; c < SpectrumAnalyser::numChannels; ++c)
    {
        const auto colourKey = "col" + juce::String (c), showKey = "show" + juce::String (c);

        if (xml.hasAttribute (colourKey))
            tree.setProperty (colourKey, (juce::int64) xml.getStringAttribute (colourKey).getLargeIntValue(), nullptr);

        if (xml.hasAttribute (showKey))
            tree.setProperty (showKey, xml.getBoolAttribute (showKey), nullptr);
    }

    for (const char* key : { "fftOrder", "smooth" })
        if (xml.hasAttribute (key))
            tree.setProperty (key, xml.getIntAttribute (key), nullptr);

    for (const char* key : { "speed", "tilt", "span", "top", "diffSpan" })
        if (xml.hasAttribute (key))
            tree.setProperty (key, xml.getDoubleAttribute (key), nullptr);

    for (const char* key : { "fill", "diffView" })
        if (xml.hasAttribute (key))
            tree.setProperty (key, xml.getBoolAttribute (key), nullptr);
}

void UiSettings::loadGlobal()
{
    if (auto xml = juce::XmlDocument::parse (getGlobalFile()))
    {
        applyXml (*xml);

        if (xml->hasAttribute ("preset"))
            tree.setProperty ("preset", xml->getStringAttribute ("preset"), nullptr);
    }
}

void UiSettings::saveGlobal() const
{
    auto xml = toXml();
    xml->setAttribute ("preset", getPresetName());

    const auto file = getGlobalFile();
    file.getParentDirectory().createDirectory();
    xml->writeTo (file);
}

juce::StringArray UiSettings::getFactoryPresetNames()
{
    juce::StringArray names;
    for (const auto& preset : factoryPresets())
        names.add (preset.name);
    return names;
}

juce::StringArray UiSettings::getUserPresetNames()
{
    juce::StringArray names;

    for (const auto& file : getPresetFolder().findChildFiles (juce::File::findFiles, false, "*.xml"))
        names.add (file.getFileNameWithoutExtension());

    names.sortNatural();
    return names;
}

bool UiSettings::loadPreset (const juce::String& name)
{
    for (const auto& preset : factoryPresets())
    {
        if (name != preset.name)
            continue;

        if (preset.resetColours)
            for (int c = 0; c < SpectrumAnalyser::numChannels; ++c)
                tree.removeProperty ("col" + juce::String (c), nullptr);

        juce::XmlElement xml ("WaveRefSettings");
        for (const auto& [key, value] : preset.values)
            xml.setAttribute (key, value);

        applyXml (xml);
        tree.setProperty ("preset", name, nullptr);
        saveGlobal();
        return true;
    }

    const auto file = getPresetFolder().getChildFile (juce::File::createLegalFileName (name) + ".xml");

    if (auto xml = juce::XmlDocument::parse (file))
    {
        applyXml (*xml);
        tree.setProperty ("preset", name, nullptr);
        saveGlobal();
        return true;
    }

    return false;
}

juce::String UiSettings::saveUserPreset (const juce::String& wantedName)
{
    auto name = juce::File::createLegalFileName (wantedName.trim());

    if (name.isEmpty())
        return {};

    if (getFactoryPresetNames().contains (name, true))
        name += " (mine)";

    const auto folder = getPresetFolder();
    folder.createDirectory();

    if (! toXml()->writeTo (folder.getChildFile (name + ".xml")))
        return {};

    tree.setProperty ("preset", name, nullptr);
    saveGlobal();
    return name;
}

bool UiSettings::deleteUserPreset (const juce::String& name)
{
    return getPresetFolder().getChildFile (juce::File::createLegalFileName (name) + ".xml").deleteFile();
}

//==============================================================================
// SpectrumView

SpectrumView::SpectrumView (WaveRefProcessor& p, UiSettings& s) : processor (p), settings (s)
{
    processor.analyser.setEnabled (true);
    startTimerHz (30);
}

SpectrumView::~SpectrumView()
{
    processor.analyser.setEnabled (false);
}

void SpectrumView::resized()
{
    const int n = juce::jmax (64, (int) getPlotArea().getWidth());

    for (auto& c : curves)
        c.assign ((size_t) n, SpectrumAnalyser::floorDb);

    diffMid.assign ((size_t) n, 0.0f);
    diffSide.assign ((size_t) n, 0.0f);
}

juce::Rectangle<float> SpectrumView::getPlotArea() const
{
    return getLocalBounds().toFloat().withTrimmedLeft (40.0f).withTrimmedRight (12.0f)
                                     .withTrimmedTop (10.0f).withTrimmedBottom (22.0f);
}

float SpectrumView::freqToX (float freq, juce::Rectangle<float> plot) const
{
    return plot.getX() + plot.getWidth() * std::log (freq / 20.0f) / std::log (1000.0f);
}

float SpectrumView::dbToY (float db, juce::Rectangle<float> plot) const
{
    const float t = (settings.top() - db) / settings.span();
    return plot.getY() + plot.getHeight() * t;
}

void SpectrumView::timerCallback()
{
    auto& analyser = processor.analyser;
    analyser.setOrder (settings.fftOrder());
    analyser.setAveragingSeconds (settings.speed());
    analyser.setFrozen (settings.freeze());
    analyser.process();
    processor.collectGarbage();

    blind = getParameter (processor.apvts, ids::blind) > 0.5f;

    if (! blind && isVisible())
    {
        const size_t n = curves[0].size();
        const float fMax = juce::jmin (20000.0f, (float) analyser.getSampleRate() * 0.49f);

        for (int c = 0; c < SpectrumAnalyser::numChannels; ++c)
        {
            if (curves[(size_t) c].size() != n)
                curves[(size_t) c].assign (n, SpectrumAnalyser::floorDb);

            analyser.getCurve (c, (int) n, 20.0f, fMax, settings.smoothing(), settings.tilt(), curves[(size_t) c].data());
            present[(size_t) c] = *std::max_element (curves[(size_t) c].begin(), curves[(size_t) c].end()) > -120.0f;
        }

        diffPresent = present[SpectrumAnalyser::mixMid] && present[SpectrumAnalyser::refMid];
        diffSidePresent = diffPresent && present[SpectrumAnalyser::mixSide] && present[SpectrumAnalyser::refSide];

        if (diffPresent)
        {
            for (size_t i = 0; i < n; ++i)
            {
                diffMid[i] = curves[SpectrumAnalyser::mixMid][i] - curves[SpectrumAnalyser::refMid][i];
                diffSide[i] = diffSidePresent ? curves[SpectrumAnalyser::mixSide][i] - curves[SpectrumAnalyser::refSide][i] : 0.0f;
            }
        }

    }

    repaint();
}

void SpectrumView::drawFrequencyGrid (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const float freqs[] { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    const char* labels[] { "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k" };

    g.setFont (klaud::font (10.5f));

    for (int i = 0; i < 10; ++i)
    {
        const float x = freqToX (freqs[i], plot);
        g.setColour (juce::Colours::white.withAlpha (i == 0 || i == 9 ? 0.0f : 0.05f));
        g.fillRect (x, plot.getY(), 1.0f, plot.getHeight());

        g.setColour (palette().textDim);
        g.drawText (labels[i], juce::Rectangle<float> (40.0f, 14.0f).withCentre ({ x, plot.getBottom() + 10.0f }).toNearestInt(),
                    juce::Justification::centred, false);
    }
}

void SpectrumView::drawGrid (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    drawFrequencyGrid (g, plot);

    const float step = settings.span() > 100.0f ? 20.0f : 10.0f;
    const float top = settings.top(), bottom = top - settings.span();

    g.setFont (klaud::font (10.5f));

    for (float db = std::floor (top / step) * step; db >= bottom; db -= step)
    {
        const float y = dbToY (db, plot);
        if (y < plot.getY() - 0.5f || y > plot.getBottom() + 0.5f)
            continue;

        g.setColour (juce::Colours::white.withAlpha (std::abs (db) < 0.01f ? 0.12f : 0.05f));
        g.fillRect (plot.getX(), y, plot.getWidth(), 1.0f);
        g.setColour (palette().textDim);
        g.drawText ((db > 0.5f ? "+" : "") + juce::String ((int) db),
                    juce::Rectangle<int> (34, 14).withCentre ({ (int) plot.getX() - 21, (int) y }),
                    juce::Justification::centredRight, false);
    }
}

void SpectrumView::drawDiffGrid (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    drawFrequencyGrid (g, plot);

    const float range = settings.diffSpan();
    const float step = range <= 6.0f ? 3.0f : range <= 24.0f ? 6.0f : 12.0f;

    g.setFont (klaud::font (10.5f));

    for (float db = -std::floor (range / step) * step; db <= range + 0.01f; db += step)
    {
        const float y = plot.getCentreY() - db / range * plot.getHeight() * 0.5f;
        g.setColour (juce::Colours::white.withAlpha (std::abs (db) < 0.01f ? 0.18f : 0.05f));
        g.fillRect (plot.getX(), y, plot.getWidth(), 1.0f);
        g.setColour (palette().textDim);
        g.drawText ((db > 0.01f ? "+" : "") + juce::String ((int) db),
                    juce::Rectangle<int> (34, 14).withCentre ({ (int) plot.getX() - 21, (int) y }),
                    juce::Justification::centredRight, false);
    }

    g.setFont (klaud::font (10.5f, true));
    g.setColour (settings.getColour (SpectrumAnalyser::mixMid).withAlpha (0.85f));
    g.drawText ("MIX LOUDER", (int) plot.getX() + 8, (int) plot.getY() + 4, 120, 14, juce::Justification::centredLeft, false);
    g.setColour (settings.getColour (SpectrumAnalyser::refMid).withAlpha (0.85f));
    g.drawText ("REF LOUDER", (int) plot.getX() + 8, (int) plot.getBottom() - 18, 120, 14, juce::Justification::centredLeft, false);
}

void SpectrumView::drawCurve (juce::Graphics& g, int channel, juce::Rectangle<float> plot) const
{
    const auto& values = curves[(size_t) channel];
    const size_t n = values.size();

    if (n < 2 || ! present[(size_t) channel] || ! settings.isVisible (channel))
        return;

    juce::Path line;
    const float step = plot.getWidth() / (float) (n - 1);

    for (size_t i = 0; i < n; ++i)
    {
        const float x = plot.getX() + (float) i * step;
        const float y = juce::jlimit (plot.getY() - 4.0f, plot.getBottom() + 4.0f, dbToY (values[i], plot));

        if (i == 0) line.startNewSubPath (x, y);
        else        line.lineTo (x, y);
    }

    const bool mid = channel == SpectrumAnalyser::mixMid || channel == SpectrumAnalyser::refMid;
    const auto colour = settings.getColour (channel);

    if (settings.fill() && mid)
    {
        juce::Path area (line);
        area.lineTo (plot.getRight(), plot.getBottom());
        area.lineTo (plot.getX(), plot.getBottom());
        area.closeSubPath();

        g.setGradientFill (juce::ColourGradient (colour.withAlpha (0.16f), 0.0f, plot.getY(),
                                                 colour.withAlpha (0.0f), 0.0f, plot.getBottom(), false));
        g.fillPath (area);
    }

    g.setColour (colour);
    g.strokePath (line, juce::PathStrokeType (mid ? 1.7f : 1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void SpectrumView::paintDiff (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    const auto& pal = palette();

    if (! diffPresent)
    {
        g.setColour (pal.textDim);
        g.setFont (klaud::font (13.0f));
        g.drawText ("Load a reference and play some audio to see mix minus reference", plot.toNearestInt(),
                    juce::Justification::centred, false);
        return;
    }

    juce::Graphics::ScopedSaveState state (g);
    g.reduceClipRegion (plot.toNearestInt());

    const float range = settings.diffSpan();
    const float zero = plot.getCentreY();
    const float half = plot.getHeight() * 0.5f;

    auto buildLine = [&] (const std::vector<float>& values)
    {
        juce::Path line;
        const size_t n = values.size();
        const float step = plot.getWidth() / (float) (n - 1);

        for (size_t i = 0; i < n; ++i)
        {
            const float y = zero - juce::jlimit (-range * 1.1f, range * 1.1f, values[i]) / range * half;
            if (i == 0) line.startNewSubPath (plot.getX(), y);
            else        line.lineTo (plot.getX() + (float) i * step, y);
        }

        return line;
    };

    const bool midOn = settings.isVisible (SpectrumAnalyser::mixMid) && settings.isVisible (SpectrumAnalyser::refMid);
    const bool sideOn = diffSidePresent && settings.isVisible (SpectrumAnalyser::mixSide) && settings.isVisible (SpectrumAnalyser::refSide);

    if (sideOn)
    {
        g.setColour (pal.textDim);
        g.strokePath (buildLine (diffSide), juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (midOn)
    {
        const auto line = buildLine (diffMid);
        juce::Path area (line);
        area.lineTo (plot.getRight(), zero);
        area.lineTo (plot.getX(), zero);
        area.closeSubPath();

        {
            juce::Graphics::ScopedSaveState above (g);
            g.reduceClipRegion (plot.withBottom (zero).toNearestInt());
            g.setColour (settings.getColour (SpectrumAnalyser::mixMid).withAlpha (0.28f));
            g.fillPath (area);
        }
        {
            juce::Graphics::ScopedSaveState below (g);
            g.reduceClipRegion (plot.withTop (zero).toNearestInt());
            g.setColour (settings.getColour (SpectrumAnalyser::refMid).withAlpha (0.28f));
            g.fillPath (area);
        }

        g.setColour (pal.text);
        g.strokePath (line, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (pal.panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (pal.panelOutline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    const auto plot = getPlotArea();
    const bool diffMode = settings.diffView() && ! blind;

    if (diffMode) drawDiffGrid (g, plot);
    else          drawGrid (g, plot);

    drawBand (g, plot);

    if (blind)
    {
        g.setColour (pal.textDim);
        g.setFont (klaud::font (15.0f, true));
        g.drawText ("BLIND TEST", plot.toNearestInt(), juce::Justification::centred, false);
        g.setFont (klaud::font (12.0f));
        g.drawText ("The curves come back when the test is off", plot.translated (0.0f, 24.0f).toNearestInt(),
                    juce::Justification::centred, false);
        return;
    }

    if (diffMode)
    {
        paintDiff (g, plot);
    }
    else
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plot.expanded (0.0f, 3.0f).toNearestInt());

        // Side underneath, mid on top; the reference below the mix.
        for (int c : { (int) SpectrumAnalyser::refSide, (int) SpectrumAnalyser::mixSide,
                       (int) SpectrumAnalyser::refMid,  (int) SpectrumAnalyser::mixMid })
            drawCurve (g, c, plot);
    }

    // Readout under the mouse.
    if (plot.contains (mouse))
    {
        const size_t n = curves[0].size();
        const float t = (mouse.x - plot.getX()) / plot.getWidth();
        const size_t index = (size_t) juce::jlimit (0, (int) n - 1, juce::roundToInt (t * (float) (n - 1)));
        const float freq = 20.0f * std::pow (1000.0f, t);

        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.fillRect (mouse.x, plot.getY(), 1.0f, plot.getHeight());

        juce::StringArray lines { formatFreq (freq) };
        std::vector<juce::Colour> dots { pal.text };

        if (diffMode)
        {
            if (diffPresent)
            {
                lines.add ("Mid  " + juce::String (diffMid[index], 1) + " dB");
                dots.push_back (pal.text);

                if (diffSidePresent)
                {
                    lines.add ("Side  " + juce::String (diffSide[index], 1) + " dB");
                    dots.push_back (pal.textDim);
                }
            }
        }
        else
        {
            for (int c = 0; c < SpectrumAnalyser::numChannels; ++c)
            {
                if (present[(size_t) c] && settings.isVisible (c))
                {
                    lines.add (juce::String (channelTitles[c]) + "  " + juce::String (curves[(size_t) c][index], 1) + " dB");
                    dots.push_back (settings.getColour (c));
                }
            }
        }

        const float boxWidth = 130.0f, boxHeight = 8.0f + 15.0f * (float) lines.size();
        auto box = juce::Rectangle<float> (boxWidth, boxHeight).withPosition (mouse.x + 12.0f, plot.getY() + 6.0f);
        if (box.getRight() > bounds.getRight() - 4.0f)
            box.setX (mouse.x - 12.0f - boxWidth);

        g.setColour (pal.background.withAlpha (0.88f));
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (pal.panelOutline);
        g.drawRoundedRectangle (box, 4.0f, 1.0f);

        g.setFont (klaud::font (11.5f));
        for (int i = 0; i < lines.size(); ++i)
        {
            g.setColour (dots[(size_t) i]);
            g.fillEllipse (box.getX() + 7.0f, box.getY() + 8.0f + 15.0f * (float) i + 3.0f, 5.0f, 5.0f);
            g.setColour (pal.text);
            g.drawText (lines[i], (int) box.getX() + 17, (int) (box.getY() + 4.0f + 15.0f * (float) i), (int) boxWidth - 20, 15,
                        juce::Justification::centredLeft, false);
        }
    }
}

float SpectrumView::xToFreq (float x, juce::Rectangle<float> plot) const
{
    return 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, (x - plot.getX()) / plot.getWidth()));
}

void SpectrumView::setBand (float low, float high)
{
    low = juce::jlimit (30.0f, 18000.0f, low);
    high = juce::jlimit (30.0f, 18000.0f, high);

    const float width = juce::jlimit (0.25f, 4.0f, std::log2 (juce::jmax (high, low * 1.01f) / low));
    const float centre = juce::jlimit (30.0f, 18000.0f, std::sqrt (low * high));

    const std::pair<const char*, float> values[] { { ids::filterOn, 1.0f }, { ids::filterFreq, centre }, { ids::filterWidth, width } };

    for (const auto& [id, value] : values)
        if (auto* p = processor.apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
}

void SpectrumView::drawBand (juce::Graphics& g, juce::Rectangle<float> plot) const
{
    if (getParameter (processor.apvts, ids::filterOn) < 0.5f)
        return;

    const float centre = getParameter (processor.apvts, ids::filterFreq);
    const float half = std::pow (2.0f, getParameter (processor.apvts, ids::filterWidth) * 0.5f);
    const float x0 = juce::jlimit (plot.getX(), plot.getRight(), freqToX (centre / half, plot));
    const float x1 = juce::jlimit (plot.getX(), plot.getRight(), freqToX (centre * half, plot));
    const auto accent = palette().accent;

    g.setColour (accent.withAlpha (0.10f));
    g.fillRect (x0, plot.getY(), x1 - x0, plot.getHeight());
    g.setColour (accent.withAlpha (0.8f));
    g.fillRect (x0 - 0.5f, plot.getY(), 1.5f, plot.getHeight());
    g.fillRect (x1 - 0.5f, plot.getY(), 1.5f, plot.getHeight());

    g.setFont (klaud::font (10.5f, true));
    g.drawText ("LISTENING  " + formatFreq (centre), juce::Rectangle<int> ((int) x0 + 6, (int) plot.getBottom() - 18, 160, 14),
                juce::Justification::centredLeft, false);
}

void SpectrumView::mouseDown (const juce::MouseEvent& e)
{
    bandDrag = BandDrag::none;
    const auto plot = getPlotArea();

    if (! plot.contains (e.position))
        return;

    for (auto* id : { ids::filterOn, ids::filterFreq, ids::filterWidth })
        if (auto* p = processor.apvts.getParameter (id))
            p->beginChangeGesture();

    const float f = xToFreq (e.position.x, plot);
    bandDrag = BandDrag::create;
    bandAnchor = f;

    if (getParameter (processor.apvts, ids::filterOn) > 0.5f)
    {
        const float centre = getParameter (processor.apvts, ids::filterFreq);
        const float half = std::pow (2.0f, getParameter (processor.apvts, ids::filterWidth) * 0.5f);
        const float x0 = freqToX (centre / half, plot), x1 = freqToX (centre * half, plot);

        if (std::abs (e.position.x - x0) <= 6.0f)       { bandDrag = BandDrag::left;  bandAnchor = centre * half; }
        else if (std::abs (e.position.x - x1) <= 6.0f)  { bandDrag = BandDrag::right; bandAnchor = centre / half; }
        else if (e.position.x > x0 && e.position.x < x1) { bandDrag = BandDrag::move;  bandGrab = std::log2 (f / centre); }
    }
}

void SpectrumView::mouseDrag (const juce::MouseEvent& e)
{
    const auto plot = getPlotArea();
    const float f = xToFreq (e.position.x, plot);
    constexpr float minRatio = 1.1892f;   // a quarter of an octave

    switch (bandDrag)
    {
        case BandDrag::create:
            if (std::abs (e.getDistanceFromDragStartX()) > 4)
                setBand (juce::jmin (bandAnchor, f), juce::jmax (bandAnchor, f));
            break;

        case BandDrag::left:   setBand (juce::jmin (f, bandAnchor / minRatio), bandAnchor); break;
        case BandDrag::right:  setBand (bandAnchor, juce::jmax (f, bandAnchor * minRatio)); break;

        case BandDrag::move:
            if (auto* p = processor.apvts.getParameter (ids::filterFreq))
                p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (30.0f, 18000.0f, f / std::pow (2.0f, bandGrab))));
            break;

        case BandDrag::none:
            break;
    }
}

void SpectrumView::mouseUp (const juce::MouseEvent&)
{
    if (bandDrag != BandDrag::none)
        for (auto* id : { ids::filterOn, ids::filterFreq, ids::filterWidth })
            if (auto* p = processor.apvts.getParameter (id))
                p->endChangeGesture();

    bandDrag = BandDrag::none;
}

void SpectrumView::mouseDoubleClick (const juce::MouseEvent&)
{
    if (auto* p = processor.apvts.getParameter (ids::filterOn))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (0.0f);
        p->endChangeGesture();
    }
}

void SpectrumView::mouseMove (const juce::MouseEvent& e)
{
    mouse = e.position;

    bool nearEdge = false;

    if (getParameter (processor.apvts, ids::filterOn) > 0.5f)
    {
        const auto plot = getPlotArea();
        const float centre = getParameter (processor.apvts, ids::filterFreq);
        const float half = std::pow (2.0f, getParameter (processor.apvts, ids::filterWidth) * 0.5f);
        nearEdge = std::abs (e.position.x - freqToX (centre / half, plot)) <= 6.0f || std::abs (e.position.x - freqToX (centre * half, plot)) <= 6.0f;
    }

    setMouseCursor (nearEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void SpectrumView::mouseExit (const juce::MouseEvent&)
{
    mouse = { -1.0f, -1.0f };
}

//==============================================================================
// WaveformView

void CueButton::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    const juce::Colour cue (0xffffd166);

    g.setColour (isSet ? cue.withAlpha (0.18f) : pal.panel.brighter (0.04f));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (isSet ? cue : pal.panelOutline);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    g.setColour (isSet ? cue : pal.textDim);
    g.setFont (klaud::font (12.0f, true));
    g.drawText (juce::String (number), getLocalBounds(), juce::Justification::centred, false);
}

void CueButton::mouseUp (const juce::MouseEvent& e)
{
    if (contains (e.getPosition()) && onClick)
        onClick (e.mods.isPopupMenu() || e.mods.isShiftDown());
}

WaveformView::WaveformView (WaveRefProcessor& p, UiSettings& s) : processor (p), settings (s)
{
    for (auto* b : { &loopButton, &fitButton, &zoomButton, &playButton })
        addAndMakeVisible (b);

    for (int i = 0; i < 4; ++i)
    {
        cueButtons[(size_t) i] = std::make_unique<CueButton> (i + 1);
        cueButtons[(size_t) i]->onClick = [this, i] (bool popup) { cueClicked (i, popup); };
        cueButtons[(size_t) i]->setTooltip ("Cue " + juce::String (i + 1) + ": click to jump there and play (or to set it at the playhead when empty). "
                                            "Right-click or Shift-click for more. Keys Q W E R. Drag its flag on the waveform to move it");
        addAndMakeVisible (*cueButtons[(size_t) i]);
    }

    loopButton.setClickingTogglesState (false);
    loopButton.onClick = [this] { toggleLoop(); };
    fitButton.onClick = [this] { fitView(); repaint(); };
    zoomButton.onClick = [this] { zoomToLoop(); repaint(); };

    playButton.setClickingTogglesState (true);
    playAttach = std::make_unique<APVTS::ButtonAttachment> (processor.apvts, ids::refPlay, playButton);

    // Beat grid.
    bpmLabel.setEditable (true);
    bpmLabel.setJustificationType (juce::Justification::centred);
    bpmLabel.setColour (juce::Label::outlineColourId, palette().panelOutline);
    bpmLabel.setFont (klaud::font (13.0f, true));
    bpmLabel.onTextChange = [this] { setBpm (bpmLabel.getText().retainCharacters ("0123456789.").getDoubleValue()); };
    bpmLabel.setTooltip ("Tempo of this reference (beats per minute). Detected when the file loads: type a value, tap it, or halve/double it");

    tapButton.onClick = [this] { tap(); };
    halfButton.onClick = [this] { setBpm (processor.getSlotInfo (selectedSlot()).bpm * 0.5); };
    doubleButton.onClick = [this] { setBpm (processor.getSlotInfo (selectedSlot()).bpm * 2.0); };
    detectButton.onClick = [this] { processor.detectGridAgain (selectedSlot()); };
    barButton.onClick = [this]
    {
        const auto info = processor.getSlotInfo (selectedSlot());
        processor.setGrid (selectedSlot(), info.bpm, processor.getPlayheadSeconds());
    };
    gridButton.setClickingTogglesState (true);
    gridButton.setToggleState (settings.get ("grid", true), juce::dontSendNotification);
    gridButton.onClick = [this] { settings.set ("grid", gridButton.getToggleState()); repaint(); };

    tapButton.setTooltip ("Tap the beat to set the tempo");
    halfButton.setTooltip ("Half the tempo (for when the detector found double)");
    doubleButton.setTooltip ("Double the tempo (for when the detector found half)");
    detectButton.setTooltip ("Detect the tempo and first bar again");
    barButton.setTooltip ("Make the playhead the first beat of a bar. Alt-click on the waveform does the same where you click. "
                          "The grid, tempo sync and the beat offset are measured from here");
    gridButton.setTooltip ("Show the beat grid on the waveform");

    for (auto* b : { &tapButton, &halfButton, &doubleButton, &detectButton, &barButton, &gridButton })
        addAndMakeVisible (b);

    addAndMakeVisible (bpmLabel);

    playButton.setTooltip ("Start and stop the reference (space bar when the plugin has focus). Double-click the waveform to play from there");
    loopButton.setTooltip ("Loop a section of the reference. Drag on the waveform to pick it; drag its edges or its middle to change it");
    fitButton.setTooltip ("Show the whole file");
    zoomButton.setTooltip ("Zoom in on the loop");

    processor.addChangeListener (this);
    startTimerHz (30);
}

WaveformView::~WaveformView()
{
    processor.removeChangeListener (this);
}

void WaveformView::playFrom (double seconds)
{
    // Jumping around only makes sense when the reference runs on its own.
    setParameter (processor.apvts, ids::refMode, 0.0f);
    setParameter (processor.apvts, ids::refPlay, 1.0f);
    processor.seekReference (seconds);
}

void WaveformView::setBpm (double bpm)
{
    if (track == nullptr || track->isEmpty())
        return;

    const auto info = processor.getSlotInfo (selectedSlot());
    processor.setGrid (selectedSlot(), juce::jlimit (0.0, 400.0, bpm), info.downbeat);
}

void WaveformView::tap()
{
    const double now = juce::Time::getMillisecondCounterHiRes();

    if (! taps.empty() && now - taps.back() > 2000.0)
        taps.clear();

    taps.push_back (now);

    if (taps.size() > 9)
        taps.erase (taps.begin());

    if (taps.size() >= 3)
        setBpm (std::round (60000.0 / ((taps.back() - taps.front()) / (double) (taps.size() - 1)) * 100.0) / 100.0);
}

int WaveformView::cueAt (juce::Point<float> position) const
{
    const auto area = getWaveArea();
    const auto info = processor.getSlotInfo (selectedSlot());

    for (int i = 3; i >= 0; --i)
    {
        if (info.cues[i] < 0.0)
            continue;

        // The numbered box of the flag.
        if (juce::Rectangle<float> (timeToX (info.cues[i]) - 1.0f, (float) area.getY(), 16.0f, 14.0f).contains (position))
            return i;
    }

    return -1;
}

void WaveformView::togglePlay()
{
    setParameter (processor.apvts, ids::refMode, 0.0f);
    setParameter (processor.apvts, ids::refPlay, getParameter (processor.apvts, ids::refPlay) > 0.5f ? 0.0f : 1.0f);
}

void WaveformView::setCueHere (int index)
{
    if (track != nullptr && ! track->isEmpty())
        processor.setCue (selectedSlot(), index, processor.getPlayheadSeconds());
}

void WaveformView::cueClicked (int index, bool popup)
{
    if (track == nullptr || track->isEmpty())
        return;

    const auto info = processor.getSlotInfo (selectedSlot());
    const bool isSet = info.cues[index] >= 0.0;

    if (popup)
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Set to the playhead");
        menu.addItem (2, "Clear", isSet);

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (cueButtons[(size_t) index].get()),
                            [this, index] (int result)
        {
            if (result == 1)       setCueHere (index);
            else if (result == 2)  processor.setCue (selectedSlot(), index, -1.0);
        });

        return;
    }

    if (isSet)
        playFrom (info.cues[index]);
    else
        setCueHere (index);
}

int WaveformView::selectedSlot() const
{
    return juce::jlimit (0, numSlots - 1, (int) std::lround (getParameter (processor.apvts, ids::slot)));
}

juce::Rectangle<int> WaveformView::getWaveArea() const
{
    return getLocalBounds().reduced (6, 0).withTrimmedTop (56).withTrimmedBottom (18);
}

double WaveformView::xToTime (float x) const
{
    const auto area = getWaveArea();
    return viewStart + (double) (x - (float) area.getX()) / (double) area.getWidth() * (viewEnd - viewStart);
}

float WaveformView::timeToX (double seconds) const
{
    const auto area = getWaveArea();
    return (float) area.getX() + (float) ((seconds - viewStart) / (viewEnd - viewStart) * area.getWidth());
}

void WaveformView::fitView()
{
    viewStart = 0.0;
    viewEnd = track != nullptr && ! track->isEmpty() ? juce::jmax (0.1, track->getSeconds()) : 1.0;
}

void WaveformView::zoomToLoop()
{
    const auto info = processor.getSlotInfo (selectedSlot());

    if (track == nullptr || track->isEmpty() || ! info.loopOn)
        return;

    const double end = info.loopEnd > 0.0 ? info.loopEnd : track->getSeconds();
    const double margin = (end - info.loopStart) * 0.08;
    viewStart = juce::jmax (0.0, info.loopStart - margin);
    viewEnd = juce::jmin (track->getSeconds(), end + margin);
}

void WaveformView::applyLoop (double start, double end)
{
    if (track == nullptr || track->isEmpty())
        return;

    const double length = track->getSeconds();
    start = juce::jlimit (0.0, length, start);
    end = juce::jlimit (0.0, length, end);

    if (end - start < 0.02)
        end = juce::jmin (length, start + 0.02);

    processor.setLoop (selectedSlot(), start, end, true);
}

void WaveformView::toggleLoop()
{
    if (track == nullptr || track->isEmpty())
        return;

    const int slot = selectedSlot();
    const auto info = processor.getSlotInfo (slot);

    if (info.loopOn)
    {
        processor.setLoop (slot, info.loopStart, info.loopEnd, false);
    }
    else if (info.loopEnd > info.loopStart)
    {
        processor.setLoop (slot, info.loopStart, info.loopEnd, true);
    }
    else
    {
        // First time: loop what is on screen.
        processor.setLoop (slot, juce::jmax (0.0, viewStart), juce::jmin (track->getSeconds(), viewEnd), true);
    }
}

void WaveformView::resized()
{
    auto header = getLocalBounds().reduced (6, 3).removeFromTop (22);
    zoomButton.setBounds (header.removeFromRight (84));
    header.removeFromRight (4);
    fitButton.setBounds (header.removeFromRight (44));
    header.removeFromRight (4);
    loopButton.setBounds (header.removeFromRight (56));
    header.removeFromRight (14);

    for (int i = 3; i >= 0; --i)
    {
        cueButtons[(size_t) i]->setBounds (header.removeFromRight (26));
        header.removeFromRight (3);
    }

    header.removeFromRight (11);
    playButton.setBounds (header.removeFromRight (62));

    auto row = getLocalBounds().reduced (6, 3).withTrimmedTop (27).removeFromTop (22);
    row.removeFromLeft (34);
    bpmLabel.setBounds (row.removeFromLeft (64));
    row.removeFromLeft (6);
    tapButton.setBounds (row.removeFromLeft (44));
    row.removeFromLeft (3);
    halfButton.setBounds (row.removeFromLeft (34));
    row.removeFromLeft (3);
    doubleButton.setBounds (row.removeFromLeft (34));
    row.removeFromLeft (3);
    detectButton.setBounds (row.removeFromLeft (60));
    row.removeFromLeft (14);
    barButton.setBounds (row.removeFromLeft (110));
    row.removeFromLeft (6);
    gridButton.setBounds (row.removeFromLeft (48));
}

void WaveformView::timerCallback()
{
    const int slot = selectedSlot();
    auto latest = processor.getTrack (slot);

    if (slot != shownSlot || latest != track)
    {
        shownSlot = slot;
        track = latest;
        fitView();
        needsRefresh = true;
    }

    const int64_t playhead = processor.getPlayheadSlot() == slot ? processor.getPlayhead() : -1;

    if (playhead != lastPlayhead)
    {
        lastPlayhead = playhead;
        repaint();
    }

    {
        const auto info = processor.getSlotInfo (slot);

        if (! bpmLabel.isBeingEdited())
            bpmLabel.setText (info.bpm > 0.0 ? juce::String (info.bpm, 2) : juce::String ("--"), juce::dontSendNotification);

        const bool hasTrack = track != nullptr && ! track->isEmpty();

        for (auto* b : { &tapButton, &halfButton, &doubleButton, &detectButton, &barButton })
            b->setEnabled (hasTrack);

        bpmLabel.setEnabled (hasTrack);
    }

    const bool playing = getParameter (processor.apvts, ids::refPlay) > 0.5f;
    const bool freeMode = getParameter (processor.apvts, ids::refMode) < 0.5f;
    playButton.setButtonText (playing && freeMode ? "STOP" : "PLAY");
    playButton.setEnabled (freeMode);

    if (needsRefresh)
    {
        needsRefresh = false;
        const auto info = processor.getSlotInfo (slot);
        loopButton.setToggleState (info.loopOn, juce::dontSendNotification);

        for (int i = 0; i < 4; ++i)
        {
            cueButtons[(size_t) i]->isSet = info.cues[i] >= 0.0;
            cueButtons[(size_t) i]->repaint();
        }

        repaint();
    }
}

void WaveformView::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (pal.panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (dropHighlight ? pal.accent : pal.panelOutline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, dropHighlight ? 2.0f : 1.0f);

    const int slot = selectedSlot();
    const auto info = processor.getSlotInfo (slot);
    const auto area = getWaveArea();

    drawCaption (g, "BPM", juce::Rectangle<int> (12, 32, 34, 18), juce::Justification::centredLeft);

    // Header: slot, file name and length.
    g.setColour (pal.text);
    g.setFont (klaud::font (13.0f, true));
    juce::String title = "Slot " + juce::String (slot + 1);
    g.drawText (title, 12, 6, 60, 18, juce::Justification::centredLeft, false);

    g.setFont (klaud::font (12.5f));
    g.setColour (pal.textDim);

    if (info.loading)
        g.drawText ("loading...", 70, 6, 300, 18, juce::Justification::centredLeft, false);
    else if (track != nullptr && ! track->isEmpty())
        g.drawText (track->name + "   " + formatTime (track->getSeconds()), 70, 6, juce::jmax (100, getWidth() - 560), 18,
                    juce::Justification::centredLeft, true);
    else if (info.error.isNotEmpty())
        g.drawText (info.error, 70, 6, juce::jmax (100, getWidth() - 560), 18, juce::Justification::centredLeft, true);
    else
        g.drawText ("empty - drop an audio file here or click the slot below", 70, 6, juce::jmax (100, getWidth() - 560), 18,
                    juce::Justification::centredLeft, true);

    if (track == nullptr || track->isEmpty())
        return;

    const double sampleRate = track->sampleRate;
    const float centre = (float) area.getCentreY();
    const float half = (float) area.getHeight() * 0.5f - 1.0f;

    // Waveform.
    const auto colour = settings.getColour (SpectrumAnalyser::refMid);
    g.setColour (colour.withAlpha (0.85f));

    for (int x = area.getX(); x < area.getRight(); ++x)
    {
        const double t0 = xToTime ((float) x), t1 = xToTime ((float) x + 1.0f);

        if (t1 < 0.0 || t0 > track->getSeconds())
            continue;

        float low = 0.0f, high = 0.0f;
        track->getPeaks ((int64_t) (t0 * sampleRate), (int64_t) (t1 * sampleRate) + 1, low, high);

        const float yTop = centre - juce::jlimit (-1.0f, 1.0f, high) * half;
        const float yBottom = centre - juce::jlimit (-1.0f, 1.0f, low) * half;
        g.fillRect ((float) x, yTop, 1.0f, juce::jmax (1.0f, yBottom - yTop));
    }

    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.fillRect (area.getX(), (int) centre, area.getWidth(), 1);

    // Loop region.
    if (info.loopOn)
    {
        const double end = info.loopEnd > 0.0 ? info.loopEnd : track->getSeconds();
        const float x0 = juce::jlimit ((float) area.getX(), (float) area.getRight(), timeToX (info.loopStart));
        const float x1 = juce::jlimit ((float) area.getX(), (float) area.getRight(), timeToX (end));

        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRect ((float) area.getX(), (float) area.getY(), x0 - (float) area.getX(), (float) area.getHeight());
        g.fillRect (x1, (float) area.getY(), (float) area.getRight() - x1, (float) area.getHeight());

        g.setColour (pal.accent.withAlpha (0.10f));
        g.fillRect (x0, (float) area.getY(), x1 - x0, (float) area.getHeight());
        g.setColour (pal.accent);
        g.fillRect (x0 - 1.0f, (float) area.getY(), 2.0f, (float) area.getHeight());
        g.fillRect (x1 - 1.0f, (float) area.getY(), 2.0f, (float) area.getHeight());

        g.setFont (klaud::font (11.0f));
        g.drawText (juce::String ((end - info.loopStart), 2) + " s  /  " + formatLufs (processor.getRefLufs()) + " LUFS",
                    juce::Rectangle<int> ((int) x0 + 6, area.getY() + 2, 160, 14), juce::Justification::centredLeft, false);
    }

    // Ruler.
    {
        const double span = viewEnd - viewStart;
        const double steps[] { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300 };
        double step = steps[std::size (steps) - 1];

        for (double candidate : steps)
            if (candidate / span * area.getWidth() >= 70.0) { step = candidate; break; }

        g.setFont (klaud::font (10.5f));

        for (double t = std::ceil (viewStart / step) * step; t < viewEnd; t += step)
        {
            const float x = timeToX (t);
            g.setColour (pal.textDim.withAlpha (0.5f));
            g.fillRect (x, (float) area.getBottom(), 1.0f, 4.0f);
            if (x < (float) area.getX() + 22.0f || x > (float) area.getRight() - 22.0f)
                continue;

            g.setColour (pal.textDim);
            g.drawText (step < 1.0 ? formatTime (t) : formatTime (t).upToLastOccurrenceOf (".", false, false),
                        juce::Rectangle<int> (60, 12).withCentre ({ (int) x, area.getBottom() + 11 }),
                        juce::Justification::centred, false);
        }
    }

    // Beat grid: a line per beat, stronger at the start of each bar.
    if (info.bpm > 1.0 && settings.get ("grid", true))
    {
        const double beat = 60.0 / info.bpm;
        const float pixelsPerBeat = (float) (beat / (viewEnd - viewStart) * area.getWidth());

        if (pixelsPerBeat >= 5.0f)
        {
            const int firstBeat = (int) std::floor ((viewStart - info.downbeat) / beat);
            g.setFont (klaud::font (9.5f, true));

            for (int k = firstBeat; ; ++k)
            {
                const double t = info.downbeat + k * beat;

                if (t > viewEnd)
                    break;

                const float x = timeToX (t);

                if (x < (float) area.getX() || x > (float) area.getRight())
                    continue;

                const bool bar = ((k % 4) + 4) % 4 == 0;
                g.setColour (juce::Colours::white.withAlpha (bar ? 0.40f : 0.14f));
                g.fillRect (x, (float) area.getY() + (bar ? 0.0f : 10.0f), 1.0f, (float) area.getHeight() - (bar ? 0.0f : 20.0f));

                if (bar && pixelsPerBeat * 4.0f >= 36.0f)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.65f));
                    g.drawText (juce::String (juce::roundToInt (std::floor ((double) k / 4.0)) + 1),
                                juce::Rectangle<int> ((int) x + 3, area.getBottom() - 14, 30, 12), juce::Justification::centredLeft, false);
                }
            }
        }
    }

    // Cue flags.
    for (int i = 0; i < 4; ++i)
    {
        if (info.cues[i] < 0.0)
            continue;

        const float x = timeToX (info.cues[i]);

        if (x < (float) area.getX() || x > (float) area.getRight())
            continue;

        const juce::Colour cue (0xffffd166);
        g.setColour (cue.withAlpha (0.75f));
        g.fillRect (x, (float) area.getY(), 1.0f, (float) area.getHeight());
        g.fillRoundedRectangle (x, (float) area.getY(), 14.0f, 13.0f, 2.0f);
        g.setColour (juce::Colour (0xff12151b));
        g.setFont (klaud::font (10.0f, true));
        g.drawText (juce::String (i + 1), juce::Rectangle<float> (x, (float) area.getY(), 14.0f, 13.0f).toNearestInt(),
                    juce::Justification::centred, false);
    }

    // Playhead.
    if (lastPlayhead >= 0)
    {
        const float x = timeToX ((double) lastPlayhead / sampleRate);

        if (x >= (float) area.getX() && x <= (float) area.getRight())
        {
            g.setColour (pal.text);
            g.fillRect (x, (float) area.getY(), 1.5f, (float) area.getHeight());
        }
    }
}

void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    const auto info = processor.getSlotInfo (selectedSlot());
    bool nearEdge = false;

    if (info.loopOn && track != nullptr)
    {
        const double end = info.loopEnd > 0.0 ? info.loopEnd : track->getSeconds();
        nearEdge = std::abs (e.position.x - timeToX (info.loopStart)) <= 6.0f || std::abs (e.position.x - timeToX (end)) <= 6.0f;
    }

    if (cueAt (e.position) >= 0)
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    else
        setMouseCursor (nearEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    drag = Drag::none;

    if (track == nullptr || track->isEmpty() || ! getWaveArea().contains (e.getPosition()))
        return;

    const auto info = processor.getSlotInfo (selectedSlot());
    const double t = xToTime (e.position.x);

    if (e.mods.isAltDown())
    {
        processor.setGrid (selectedSlot(), info.bpm, juce::jlimit (0.0, track->getSeconds(), t));
        return;
    }

    if (const int cue = cueAt (e.position); cue >= 0)
    {
        drag = Drag::cue;
        dragCue = cue;
        cueGrab = e.position.x - timeToX (info.cues[cue]);
        return;
    }

    dragAnchor = t;
    dragStartLoop = info.loopStart;
    dragEndLoop = info.loopEnd > 0.0 ? info.loopEnd : track->getSeconds();

    if (info.loopOn)
    {
        const float sx = timeToX (dragStartLoop), ex = timeToX (dragEndLoop);

        if (std::abs (e.position.x - sx) <= 6.0f)       drag = Drag::start;
        else if (std::abs (e.position.x - ex) <= 6.0f)  drag = Drag::end;
        else if (e.position.x > sx && e.position.x < ex)
        {
            drag = Drag::region;
            dragGrab = t - dragStartLoop;
        }
    }

    if (drag == Drag::none)
        drag = Drag::create;
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::none || track == nullptr)
        return;

    const double length = track->getSeconds();
    const double t = juce::jlimit (0.0, length, xToTime (e.position.x));

    switch (drag)
    {
        case Drag::create:
            if (std::abs (e.getDistanceFromDragStartX()) > 3)
                applyLoop (juce::jmin (dragAnchor, t), juce::jmax (dragAnchor, t));
            break;

        case Drag::start:
            applyLoop (juce::jmin (t, dragEndLoop - 0.02), dragEndLoop);
            break;

        case Drag::end:
            applyLoop (dragStartLoop, juce::jmax (t, dragStartLoop + 0.02));
            break;

        case Drag::region:
        {
            const double loopLength = dragEndLoop - dragStartLoop;
            const double start = juce::jlimit (0.0, juce::jmax (0.0, length - loopLength), t - dragGrab);
            applyLoop (start, start + loopLength);
            break;
        }

        case Drag::cue:
            processor.setCue (selectedSlot(), dragCue, juce::jlimit (0.0, length, xToTime (e.position.x - cueGrab)));
            break;

        case Drag::none:
            break;
    }
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    // A click on a flag without moving it jumps to the cue, like its button.
    if (drag == Drag::cue && e.getDistanceFromDragStart() < 3)
        cueClicked (dragCue, false);

    drag = Drag::none;
    dragCue = -1;
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (track == nullptr || track->isEmpty() || ! getWaveArea().contains (e.getPosition()) || cueAt (e.position) >= 0)
        return;

    playFrom (juce::jlimit (0.0, track->getSeconds(), xToTime (e.position.x)));
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (track == nullptr || track->isEmpty())
        return;

    const double length = track->getSeconds();
    const double span = viewEnd - viewStart;

    if (e.mods.isShiftDown() || wheel.deltaX != 0.0f)
    {
        const double delta = (wheel.deltaX != 0.0f ? wheel.deltaX : wheel.deltaY) * span * -0.5;
        const double start = juce::jlimit (0.0, juce::jmax (0.0, length - span), viewStart + delta);
        viewStart = start;
        viewEnd = start + span;
    }
    else
    {
        const double anchor = xToTime (e.position.x);
        const double ratio = (anchor - viewStart) / span;
        const double newSpan = juce::jlimit (0.01, length, span * std::exp (-wheel.deltaY * 0.8));
        viewStart = juce::jlimit (0.0, juce::jmax (0.0, length - newSpan), anchor - ratio * newSpan);
        viewEnd = viewStart + newSpan;
    }

    repaint();
}

bool WaveformView::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && isAudioFile (juce::File (files[0]));
}

void WaveformView::filesDropped (const juce::StringArray& files, int, int)
{
    dropHighlight = false;

    if (files.size() == 1 && isAudioFile (juce::File (files[0])))
        processor.loadFile (selectedSlot(), juce::File (files[0]));

    repaint();
}

//==============================================================================
// Slots

SlotButton::SlotButton (WaveRefProcessor& p, int slotIndex) : processor (p), index (slotIndex)
{
    setRepaintsOnMouseActivity (true);
}

void SlotButton::select()
{
    setParameter (processor.apvts, ids::slot, (float) index);
}

void SlotButton::chooseFile()
{
    static juce::File lastFolder;

    chooser = std::make_unique<juce::FileChooser> ("Load a reference into slot " + juce::String (index + 1), lastFolder,
                                                   "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        if (file.existsAsFile())
        {
            lastFolder = file.getParentDirectory();
            processor.loadFile (index, file);
            select();
        }
    });
}

void SlotButton::mouseUp (const juce::MouseEvent& e)
{
    if (! contains (e.getPosition()))
        return;

    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        const auto info = processor.getSlotInfo (index);
        menu.addItem (1, "Load file...");
        menu.addItem (2, "Clear slot", info.path.isNotEmpty());
        menu.addItem (3, "Show file in folder", info.path.isNotEmpty() && juce::File (info.path).existsAsFile());

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                            [this] (int result)
        {
            if (result == 1)       chooseFile();
            else if (result == 2)  processor.clearSlot (index);
            else if (result == 3)  juce::File (processor.getSlotInfo (index).path).revealToUser();
        });

        return;
    }

    select();

    if (processor.getSlotInfo (index).path.isEmpty())
        chooseFile();
}

bool SlotButton::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && isAudioFile (juce::File (files[0]));
}

void SlotButton::filesDropped (const juce::StringArray& files, int, int)
{
    if (files.size() == 1 && isAudioFile (juce::File (files[0])))
    {
        processor.loadFile (index, juce::File (files[0]));
        select();
    }
}

void SlotButton::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    const bool selected = (int) std::lround (getParameter (processor.apvts, ids::slot)) == index;
    const auto info = processor.getSlotInfo (index);
    const auto track = processor.getTrack (index);
    const bool loaded = track != nullptr && ! track->isEmpty();

    g.setColour (selected ? pal.accent.withAlpha (0.16f) : pal.panel.brighter (isMouseOver() ? 0.08f : 0.02f));
    g.fillRoundedRectangle (bounds, 5.0f);
    g.setColour (selected ? pal.accent : pal.panelOutline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 5.0f, selected ? 1.5f : 1.0f);

    g.setColour (selected ? pal.accent : pal.textDim);
    g.setFont (klaud::font (13.0f, true));
    g.drawText (juce::String (index + 1), 8, 0, 16, getHeight(), juce::Justification::centredLeft, false);

    juce::String text;
    juce::Colour colour = pal.text;

    if (info.loading)                text = "loading...", colour = pal.textDim;
    else if (loaded)                 text = track->name;
    else if (info.path.isNotEmpty()) text = "missing: " + juce::File (info.path).getFileName(), colour = juce::Colour (0xffe0795a);
    else                             text = "empty", colour = pal.textDim.withAlpha (0.6f);

    g.setColour (colour);
    g.setFont (klaud::font (12.0f));
    g.drawText (text, 26, 0, getWidth() - 32, getHeight(), juce::Justification::centredLeft, true);
}

SlotBar::SlotBar (WaveRefProcessor& p) : processor (p)
{
    for (int i = 0; i < numSlots; ++i)
    {
        buttons[(size_t) i] = std::make_unique<SlotButton> (processor, i);
        addAndMakeVisible (*buttons[(size_t) i]);
        buttons[(size_t) i]->setTooltip ("Click to play this reference. Right-click for more. You can also drop a file here.");
    }

    processor.addChangeListener (this);
    startTimerHz (8);
}

SlotBar::~SlotBar()
{
    processor.removeChangeListener (this);
}

void SlotBar::resized()
{
    constexpr int gap = 6;
    const int width = (getWidth() - gap * (numSlots - 1)) / numSlots;

    for (int i = 0; i < numSlots; ++i)
        buttons[(size_t) i]->setBounds (i * (width + gap), 0, width, getHeight());
}

void SlotBar::repaintAll()
{
    for (auto& b : buttons)
        b->repaint();
}

//==============================================================================
// StereoPanel

StereoPanel::StereoPanel (WaveRefProcessor& p, UiSettings& s) : processor (p), settings (s)
{
    processor.stereo.setEnabled (true);
    startTimerHz (30);
}

StereoPanel::~StereoPanel()
{
    processor.stereo.setEnabled (false);
}

void StereoPanel::drawBar (juce::Graphics& g, juce::Rectangle<float> area, const juce::String& name, bool reference)
{
    const auto& pal = palette();
    const auto colour = settings.getColour (reference ? SpectrumAnalyser::refMid : SpectrumAnalyser::mixMid);
    const bool signal = processor.stereo.hasSignal (reference);
    const float correlation = processor.stereo.getCorrelation (reference);
    const float width = processor.stereo.getWidth (reference);

    g.setColour (pal.textDim);
    g.setFont (klaud::font (10.5f, true));
    g.drawText (name, area.withWidth (34.0f).toNearestInt(), juce::Justification::centredLeft, false);

    const auto bar = juce::Rectangle<float> (area.getX() + 36.0f, area.getY() + 4.0f, area.getWidth() - 36.0f - 70.0f, 8.0f);
    g.setColour (pal.track);
    g.fillRoundedRectangle (bar, 4.0f);

    if (signal)
    {
        const float zero = bar.getCentreX();
        const float x = zero + correlation * bar.getWidth() * 0.5f;
        g.setColour (correlation < 0.0f ? juce::Colour (0xffff5a4d) : colour);
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (zero, x), bar.getY(), std::abs (x - zero), bar.getHeight()), 3.0f);
    }

    g.setColour (juce::Colours::white.withAlpha (0.3f));
    g.fillRect (bar.getCentreX() - 0.5f, bar.getY() - 2.0f, 1.0f, bar.getHeight() + 4.0f);

    g.setColour (signal ? pal.text : pal.textDim);
    g.setFont (klaud::font (11.0f));
    g.drawText (signal ? juce::String (correlation, 2) : juce::String ("--"),
                juce::Rectangle<int> ((int) bar.getRight() + 4, (int) area.getY(), 36, 16), juce::Justification::centredLeft, false);
    g.setColour (pal.textDim);
    g.drawText (signal ? juce::String (juce::roundToInt (width * 100.0f)) + "%" : juce::String(),
                juce::Rectangle<int> ((int) bar.getRight() + 38, (int) area.getY(), 32, 16), juce::Justification::centredLeft, false);
}

void StereoPanel::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto bounds = getLocalBounds().toFloat();

    g.setColour (pal.panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (pal.panelOutline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    drawCaption (g, "Stereo", juce::Rectangle<int> (10, 4, 100, 14), juce::Justification::centredLeft);

    // Goniometer: side across, mid up, so a mono signal is a vertical line.
    const float side = juce::jmin (bounds.getWidth() - 20.0f, bounds.getHeight() - 90.0f);
    const auto box = juce::Rectangle<float> (side, side).withCentre ({ bounds.getCentreX(), 24.0f + side * 0.5f });
    const auto centre = box.getCentre();

    g.setColour (pal.background.withAlpha (0.6f));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.drawLine (box.getX(), centre.y, box.getRight(), centre.y);
    g.drawLine (centre.x, box.getY(), centre.x, box.getBottom());
    g.drawLine (box.getX(), box.getBottom(), box.getRight(), box.getY());
    g.drawLine (box.getX(), box.getY(), box.getRight(), box.getBottom());
    g.setColour (pal.panelOutline);
    g.drawRoundedRectangle (box, 4.0f, 1.0f);

    g.setColour (pal.textDim);
    g.setFont (klaud::font (10.0f, true));
    g.drawText ("L", juce::Rectangle<int> ((int) box.getRight() - 16, (int) box.getY() + 2, 14, 12), juce::Justification::centred, false);
    g.drawText ("R", juce::Rectangle<int> ((int) box.getX() + 2, (int) box.getY() + 2, 14, 12), juce::Justification::centred, false);
    g.drawText ("M", juce::Rectangle<int> ((int) centre.x - 7, (int) box.getY() + 2, 14, 12), juce::Justification::centred, false);

    constexpr int count = 1200;
    std::vector<float> points ((size_t) count * 2);

    // One scale for both so their sizes compare; it follows the louder of the two, slowly.
    float peak = 0.02f;
    std::array<std::vector<float>, 2> data;

    for (int source = 0; source < 2; ++source)
    {
        data[(size_t) source].resize ((size_t) count * 2);
        processor.stereo.readPoints (source == 1, data[(size_t) source].data(), count);

        for (float v : data[(size_t) source])
            peak = juce::jmax (peak, std::abs (v));
    }

    scale += (1.0f / (peak * 1.1f) - scale) * (1.0f / (peak * 1.1f) < scale ? 0.5f : 0.05f);
    const float half = box.getWidth() * 0.5f - 3.0f;

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (box.toNearestInt());

        for (int source : { 1, 0 })   // reference underneath, mix on top
        {
            if (! processor.stereo.hasSignal (source == 1))
                continue;

            g.setColour (settings.getColour (source == 1 ? SpectrumAnalyser::refMid : SpectrumAnalyser::mixMid).withAlpha (0.5f));

            for (int i = 0; i < count; ++i)
            {
                const float x = centre.x + data[(size_t) source][(size_t) i * 2] * scale * half;
                const float y = centre.y - data[(size_t) source][(size_t) i * 2 + 1] * scale * half;
                g.fillRect (x, y, 1.6f, 1.6f);
            }
        }
    }

    const float barTop = box.getBottom() + 8.0f;
    drawBar (g, { 10.0f, barTop, bounds.getWidth() - 20.0f, 18.0f }, "MIX", false);
    drawBar (g, { 10.0f, barTop + 22.0f, bounds.getWidth() - 20.0f, 18.0f }, "REF", true);

    g.setColour (pal.textDim.withAlpha (0.7f));
    g.setFont (klaud::font (9.5f));
    g.drawText ("correlation   width", juce::Rectangle<int> (10, (int) barTop + 40, (int) bounds.getWidth() - 20, 12),
                juce::Justification::centredRight, false);
}

//==============================================================================
// AnalyserBar

AnalyserBar::AnalyserBar (UiSettings& s) : settings (s)
{
    for (int i = 0; i < 5; ++i)
        fftBox.addItem (juce::String (1 << (11 + i)), i + 1);

    const char* smoothNames[] { "Off", "1/24", "1/12", "1/6", "1/3", "1/1" };
    for (int i = 0; i < 6; ++i)
        smoothBox.addItem (smoothNames[i], i + 1);

    const char* tiltNames[] { "Flat", "3 dB", "4.5 dB", "6 dB" };
    for (int i = 0; i < 4; ++i)
        tiltBox.addItem (tiltNames[i], i + 1);

    const char* topNames[] { "0 dB", "+3 dB", "+6 dB", "+9 dB", "+12 dB" };
    for (int i = 0; i < 5; ++i)
        topBox.addItem (topNames[i], i + 1);

    rebuildRange (false);

    fftBox.onChange = [this] { settings.set ("fftOrder", 10 + fftBox.getSelectedId()); };
    smoothBox.onChange = [this] { settings.set ("smooth", smoothingDivisors[smoothBox.getSelectedId() - 1]); };
    tiltBox.onChange = [this] { settings.set ("tilt", tiltValues[tiltBox.getSelectedId() - 1]); };
    topBox.onChange = [this] { settings.set ("top", topValues[topBox.getSelectedId() - 1]); };
    rangeBox.onChange = [this]
    {
        if (rangeShowsDiff) settings.set ("diffSpan", diffSpanValues[rangeBox.getSelectedId() - 1]);
        else                settings.set ("span", spanValues[rangeBox.getSelectedId() - 1]);
    };

    fftBox.setTooltip ("FFT size: bigger = finer bass detail. The speed setting keeps the response time about the same at every size");
    smoothBox.setTooltip ("Smooth the curves over a fraction of an octave");
    tiltBox.setTooltip ("Slope added around 1 kHz so a typical mix reads roughly flat");
    topBox.setTooltip ("Level at the top of the graph: raise it if your spectrum goes above 0 dB");
    rangeBox.setTooltip ("Height of the graph in dB (in the difference view: how far above and below zero it reaches)");

    speedSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    speedSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    speedSlider.setNormalisableRange (juce::NormalisableRange<double> (0.02, 5.0, 0.001, 0.35));
    speedSlider.setDoubleClickReturnValue (true, 0.4);
    speedSlider.onValueChange = [this] { settings.set ("speed", (float) speedSlider.getValue()); };
    speedSlider.setTooltip ("How fast the curves react: left = quick, right = slow and steady. The FFT window sets a floor for big sizes");

    fillButton.onClick = [this] { settings.set ("fill", fillButton.getToggleState()); };
    diffButton.onClick = [this] { settings.set ("diffView", diffButton.getToggleState()); refresh(); };
    freezeButton.onClick = [this] { settings.set ("freeze", freezeButton.getToggleState()); };
    diffButton.setTooltip ("Show mix minus reference on the whole graph instead of the curves (needs matched loudness to mean much)");
    freezeButton.setTooltip ("Hold the curves still");

    for (auto* b : { &fillButton, &diffButton, &freezeButton })
    {
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }

    for (auto* c : { &fftBox, &smoothBox, &tiltBox, &topBox, &rangeBox })
        addAndMakeVisible (c);

    addAndMakeVisible (speedSlider);
    refresh();
}

void AnalyserBar::rebuildRange (bool forDiff)
{
    rangeShowsDiff = forDiff;
    rangeBox.clear (juce::dontSendNotification);

    if (forDiff)
    {
        const auto plusMinus = juce::String (juce::CharPointer_UTF8 ("\xc2\xb1"));
        for (int i = 0; i < 4; ++i)
            rangeBox.addItem (plusMinus + juce::String ((int) diffSpanValues[i]) + " dB", i + 1);
    }
    else
    {
        for (int i = 0; i < 3; ++i)
            rangeBox.addItem (juce::String ((int) spanValues[i]) + " dB", i + 1);
    }
}

void AnalyserBar::refresh()
{
    fftBox.setSelectedId (juce::jlimit (11, 15, settings.fftOrder()) - 10, juce::dontSendNotification);

    int smoothIndex = 3;
    for (int i = 0; i < 6; ++i)
        if (smoothingDivisors[i] == settings.smoothing()) smoothIndex = i;
    smoothBox.setSelectedId (smoothIndex + 1, juce::dontSendNotification);

    int tiltIndex = 2;
    for (int i = 0; i < 4; ++i)
        if (std::abs (tiltValues[i] - settings.tilt()) < 0.01f) tiltIndex = i;
    tiltBox.setSelectedId (tiltIndex + 1, juce::dontSendNotification);

    int topIndex = 0;
    for (int i = 0; i < 5; ++i)
        if (std::abs (topValues[i] - settings.top()) < 0.01f) topIndex = i;
    topBox.setSelectedId (topIndex + 1, juce::dontSendNotification);

    const bool diff = settings.diffView();

    if (diff != rangeShowsDiff)
        rebuildRange (diff);

    int rangeIndex = diff ? 1 : 1;
    if (diff)
    {
        for (int i = 0; i < 4; ++i)
            if (std::abs (diffSpanValues[i] - settings.diffSpan()) < 0.01f) rangeIndex = i;
    }
    else
    {
        for (int i = 0; i < 3; ++i)
            if (std::abs (spanValues[i] - settings.span()) < 0.01f) rangeIndex = i;
    }
    rangeBox.setSelectedId (rangeIndex + 1, juce::dontSendNotification);

    topBox.setEnabled (! diff);

    speedSlider.setValue (settings.speed(), juce::dontSendNotification);
    fillButton.setToggleState (settings.fill(), juce::dontSendNotification);
    diffButton.setToggleState (diff, juce::dontSendNotification);
    freezeButton.setToggleState (settings.freeze(), juce::dontSendNotification);
}

void AnalyserBar::resized()
{
    auto area = getLocalBounds();
    const int h = 24;
    const int y = (getHeight() - h) / 2;

    for (int c = 0; c < SpectrumAnalyser::numChannels; ++c)
    {
        chips[(size_t) c] = { area.removeFromLeft (66).withHeight (h).withY (y), c };
        area.removeFromLeft (3);
    }

    area.removeFromLeft (10);

    auto place = [&] (juce::Component& c, int captionWidth, int width)
    {
        area.removeFromLeft (captionWidth);
        c.setBounds (area.removeFromLeft (width).withHeight (h).withY (y));
        area.removeFromLeft (6);
    };

    place (fftBox, 26, 58);
    place (smoothBox, 46, 56);
    place (tiltBox, 24, 64);
    place (topBox, 24, 64);
    place (rangeBox, 38, 66);
    place (speedSlider, 38, 72);

    fillButton.setBounds (area.removeFromLeft (48).withHeight (h).withY (y));
    diffButton.setBounds (area.removeFromLeft (48).withHeight (h).withY (y));
    freezeButton.setBounds (area.removeFromLeft (66).withHeight (h).withY (y));
}

void AnalyserBar::paint (juce::Graphics& g)
{
    const auto& pal = palette();

    for (const auto& chip : chips)
    {
        const bool on = settings.isVisible (chip.channel);
        const auto colour = settings.getColour (chip.channel);
        const auto r = chip.bounds.toFloat();

        g.setColour (on ? colour.withAlpha (0.16f) : pal.panel);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (on ? colour : pal.panelOutline);
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);

        g.setColour (on ? colour : pal.textDim.withAlpha (0.5f));
        g.fillEllipse (r.getX() + 7.0f, r.getCentreY() - 3.0f, 6.0f, 6.0f);
        g.setColour (on ? pal.text : pal.textDim);
        g.setFont (klaud::font (10.0f, true));
        g.drawText (channelNames[chip.channel], chip.bounds.withTrimmedLeft (16), juce::Justification::centredLeft, false);
    }

    auto captionFor = [&g] (const juce::Component& c, int width, const char* text)
    {
        drawCaption (g, text, juce::Rectangle<int> (c.getX() - width - 2, c.getY(), width, c.getHeight()), juce::Justification::centredRight);
    };

    captionFor (fftBox, 26, "FFT");
    captionFor (smoothBox, 46, "Smooth");
    captionFor (tiltBox, 24, "Tilt");
    captionFor (topBox, 24, "Top");
    captionFor (rangeBox, 38, "Range");
    captionFor (speedSlider, 38, "Speed");
}

void AnalyserBar::mouseUp (const juce::MouseEvent& e)
{
    for (const auto& chip : chips)
    {
        if (chip.bounds.contains (e.getPosition()))
        {
            if (e.mods.isPopupMenu() || e.mods.isCtrlDown())
                showColourPicker (chip.channel);
            else
                settings.setVisible (chip.channel, ! settings.isVisible (chip.channel));

            repaint();
            return;
        }
    }
}

void AnalyserBar::showColourPicker (int channel)
{
    struct Picker : public juce::ColourSelector, private juce::ChangeListener
    {
        Picker (UiSettings& s, int c, AnalyserBar& b)
            : juce::ColourSelector (juce::ColourSelector::showColourspace | juce::ColourSelector::showSliders),
              settings (s), channel (c), bar (&b)
        {
            setCurrentColour (settings.getColour (c), juce::dontSendNotification);
            addChangeListener (this);
            setSize (240, 250);
        }

        ~Picker() override { removeChangeListener (this); }

        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            settings.setColour (channel, getCurrentColour().withAlpha (1.0f));

            if (bar != nullptr)
                bar->repaint();
        }

        UiSettings& settings;
        int channel;
        juce::Component::SafePointer<AnalyserBar> bar;
    };

    const auto bounds = chips[(size_t) channel].bounds;
    auto picker = std::make_unique<Picker> (settings, channel, *this);
    juce::CallOutBox::launchAsynchronously (std::move (picker), localAreaToGlobal (bounds), nullptr);
}

//==============================================================================
// PresetBar

PresetBar::PresetBar (UiSettings& s, std::function<void()> onChanged) : settings (s), changed (std::move (onChanged))
{
    box.setTextWhenNothingSelected ("Default");
    box.setTooltip ("Display presets: curve colours, FFT size, speed, smoothing, tilt, range, top...");
    box.onChange = [this]
    {
        const auto index = box.getSelectedItemIndex();

        if (index >= 0 && settings.loadPreset (box.getItemText (index)))
        {
            deleteButton.setEnabled (UiSettings::getUserPresetNames().contains (settings.getPresetName()));

            if (changed)
                changed();
        }
    };

    saveButton.onClick = [this] { showSaveDialog(); };
    deleteButton.onClick = [this] { showDeleteDialog(); };
    saveButton.setTooltip ("Save the current colours and analyser settings as a preset");
    deleteButton.setTooltip ("Delete the selected preset (your own presets only)");

    for (auto* c : std::initializer_list<juce::Component*> { &box, &saveButton, &deleteButton })
        addAndMakeVisible (c);

    refresh();
}

void PresetBar::resized()
{
    auto area = getLocalBounds();
    deleteButton.setBounds (area.removeFromRight (72));
    area.removeFromRight (6);
    saveButton.setBounds (area.removeFromRight (62));
    area.removeFromRight (6);
    box.setBounds (area);
}

void PresetBar::refresh()
{
    box.clear (juce::dontSendNotification);

    box.addSectionHeading ("Factory");
    int id = 1;
    for (const auto& name : UiSettings::getFactoryPresetNames())
        box.addItem (name, id++);

    const auto user = UiSettings::getUserPresetNames();

    if (! user.isEmpty())
    {
        box.addSeparator();
        box.addSectionHeading ("Yours");
        id = 100;
        for (const auto& name : user)
            box.addItem (name, id++);
    }

    const auto current = settings.getPresetName();

    for (int i = 0; i < box.getNumItems(); ++i)
        if (box.getItemText (i) == current)
            box.setSelectedItemIndex (i, juce::dontSendNotification);

    box.setText (current, juce::dontSendNotification);
    deleteButton.setEnabled (user.contains (current));
}

void PresetBar::showSaveDialog()
{
    dialog = std::make_unique<juce::AlertWindow> ("Save preset", "Name your preset:", juce::MessageBoxIconType::NoIcon, this);
    dialog->setLookAndFeel (&getLookAndFeel());

    const auto current = settings.getPresetName();
    dialog->addTextEditor ("name", UiSettings::getFactoryPresetNames().contains (current) ? juce::String() : current);
    dialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<PresetBar> safeThis (this);

    dialog->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis] (int result)
    {
        if (safeThis == nullptr || safeThis->dialog == nullptr)
            return;

        const auto name = safeThis->dialog->getTextEditorContents ("name");
        safeThis->dialog->setVisible (false);

        if (result == 1 && safeThis->settings.saveUserPreset (name).isNotEmpty())
            safeThis->refresh();
    }), false);
}

void PresetBar::showDeleteDialog()
{
    const auto name = settings.getPresetName();

    if (! UiSettings::getUserPresetNames().contains (name))
        return;

    dialog = std::make_unique<juce::AlertWindow> ("Delete preset", "Delete \"" + name + "\"?", juce::MessageBoxIconType::NoIcon, this);
    dialog->setLookAndFeel (&getLookAndFeel());
    dialog->addButton ("Delete", 1, juce::KeyPress (juce::KeyPress::returnKey));
    dialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<PresetBar> safeThis (this);

    dialog->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, name] (int result)
    {
        if (safeThis == nullptr || safeThis->dialog == nullptr)
            return;

        safeThis->dialog->setVisible (false);

        if (result == 1 && safeThis->settings.deleteUserPreset (name))
        {
            safeThis->settings.loadPreset ("Default");
            safeThis->refresh();

            if (safeThis->changed)
                safeThis->changed();
        }
    }), false);
}

//==============================================================================
// Switch, loudness, monitor, blind test

void ABButton::setLabels (const juce::String& big, const juce::String& small, bool neutral)
{
    if (big != bigText || small != smallText || neutral != neutralColour)
    {
        bigText = big;
        smallText = small;
        neutralColour = neutral;
        repaint();
    }
}

void ABButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto& pal = palette();
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    const auto colour = neutralColour ? pal.accent : settings.getColour (colourChannel);
    const bool on = getToggleState();

    if (on)
    {
        g.setGradientFill (juce::ColourGradient (colour.brighter (highlighted ? 0.2f : 0.05f), 0.0f, bounds.getY(),
                                                 colour.darker (0.35f), 0.0f, bounds.getBottom(), false));
        g.fillRoundedRectangle (bounds, 7.0f);
    }
    else
    {
        g.setColour (pal.panel.brighter (down ? 0.2f : highlighted ? 0.1f : 0.03f));
        g.fillRoundedRectangle (bounds, 7.0f);
        g.setColour (colour.withAlpha (highlighted ? 0.8f : 0.4f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 7.0f, 1.2f);
    }

    g.setColour (on ? juce::Colour (0xff12151b) : colour.withAlpha (0.9f));
    g.setFont (klaud::font (22.0f, true));
    g.drawText (bigText, getLocalBounds().withTrimmedBottom (14), juce::Justification::centred, false);

    g.setFont (klaud::font (10.5f, true));
    g.setColour (on ? juce::Colour (0xff12151b).withAlpha (0.7f) : pal.textDim);
    g.drawText (smallText, getLocalBounds().withTop (getHeight() - 20).withHeight (16), juce::Justification::centred, false);
}

Knob::Knob (APVTS& apvts, const juce::String& paramId, const juce::String& titleText) : title (titleText)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 14);
    slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    slider.setColour (juce::Slider::rotarySliderFillColourId, palette().accent);
    addAndMakeVisible (slider);

    attachment = std::make_unique<APVTS::SliderAttachment> (apvts, paramId, slider);

    if (auto* p = apvts.getParameter (paramId))
        slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
}

void Knob::paint (juce::Graphics& g)
{
    drawCaption (g, title, getLocalBounds().withHeight (14));
}

void Knob::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (14));
}

ControlPanel::ControlPanel (WaveRefProcessor& p, UiSettings& s)
    : processor (p), settings (s), apvts (p.apvts),
      mixButton (s, SpectrumAnalyser::mixMid, "mix"), refButton (s, SpectrumAnalyser::refMid, "ref"),
      trimKnob (p.apvts, ids::trim, "Trim"), freqKnob (p.apvts, ids::filterFreq, "Band"), widthKnob (p.apvts, ids::filterWidth, "Width"),
      offsetKnob (p.apvts, ids::syncOffset, "Offset"),
      beatKnob (p.apvts, ids::beatOffset, "Beats"),
      nudgeKnob (p.apvts, ids::nudge, "Nudge")
{
    mixButton.onClick = [this] { setAb (false); };
    refButton.onClick = [this] { setAb (true); };
    mixButton.setTooltip ("Hear the mix coming from the DAW");
    refButton.setTooltip ("Hear the loaded reference");

    for (auto* b : { &matchButton, &monoButton, &sideButton, &deltaButton, &bandButton, &blindButton, &keyLockButton })
    {
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }

    matchButton.setTooltip ("Bring the reference to the loudness of the mix, using the loop's loudness");
    monoButton.setTooltip ("Fold what you hear to mono (the mid signal)");
    sideButton.setTooltip ("Listen to the side signal only: what is left when the mono part is removed");
    deltaButton.setTooltip ("Listen to mix minus reference (at the matched level): what the mix has that the reference doesn't. "
                            "Most useful with two versions of the same song in sync");
    bandButton.setTooltip ("Listen to one band only, to compare it between mix and reference. Drag on the spectrum to pick it");
    blindButton.setTooltip ("Blind test: A and B become X and Y in random order. Guess which is the reference");

    matchAttach = std::make_unique<APVTS::ButtonAttachment> (apvts, ids::matchOn, matchButton);
    monoAttach = std::make_unique<APVTS::ButtonAttachment> (apvts, ids::mono, monoButton);
    sideAttach = std::make_unique<APVTS::ButtonAttachment> (apvts, ids::side, sideButton);
    deltaAttach = std::make_unique<APVTS::ButtonAttachment> (apvts, ids::delta, deltaButton);
    bandAttach = std::make_unique<APVTS::ButtonAttachment> (apvts, ids::filterOn, bandButton);
    blindAttach = std::make_unique<APVTS::ButtonAttachment> (apvts, ids::blind, blindButton);
    keyLockAttach = std::make_unique<APVTS::ButtonAttachment> (apvts, ids::keyLock, keyLockButton);
    keyLockButton.setTooltip ("Keep the reference's pitch when its tempo is matched to the host's. Off: it speeds up or slows down like a record");

    // Mono and side exclude each other.
    monoButton.onClick = [this] { if (monoButton.getToggleState()) setParameter (apvts, ids::side, 0.0f); };
    sideButton.onClick = [this] { if (sideButton.getToggleState()) setParameter (apvts, ids::mono, 0.0f); };

    matchMode.addItem ("3 s", 1);
    matchMode.addItem ("30 s", 2);
    matchMode.setTooltip ("How much of the mix is measured to set the reference level");
    modeAttach = std::make_unique<APVTS::ComboBoxAttachment> (apvts, ids::matchMode, matchMode);
    addAndMakeVisible (matchMode);

    syncMode.addItem ("Free", 1);
    syncMode.addItem ("Host time", 2);
    syncMode.addItem ("Beats", 3);
    syncMode.setTooltip ("Free: the reference plays on its own (play, cues, loop).\nHost time: it follows the DAW's timeline plus an offset, "
                         "for different mixes of the same song.\nBeats: its beat grid follows the DAW's beats and it is stretched to the project tempo, "
                         "for songs with the same structure at different tempos");
    syncAttach = std::make_unique<APVTS::ComboBoxAttachment> (apvts, ids::refMode, syncMode);
    addAndMakeVisible (syncMode);

    alignButton.setTooltip ("Make the reference's current position line up with the DAW's current position. Host time mode: by time. Beats mode: by whole beats, "
                            "so the bars stay together. From free mode this switches to host time");
    alignButton.onClick = [this]
    {
        if (! processor.alignToHost())
            syncText = "The host doesn't report its time";
    };
    addAndMakeVisible (alignButton);

    for (auto* k : { &trimKnob, &freqKnob, &widthKnob, &offsetKnob, &beatKnob, &nudgeKnob })
        addAndMakeVisible (k);

    for (auto* b : { &mixButton, &refButton })
        addAndMakeVisible (b);

    optionsButton.onClick = [this] { showOptions(); };
    guessX.onClick = [this] { guess (true); };
    guessY.onClick = [this] { guess (false); };
    addAndMakeVisible (optionsButton);

    for (auto* b : { &guessX, &guessY })
        addChildComponent (b);

    startTimerHz (15);
    timerCallback();
}

void ControlPanel::setAb (bool ref)
{
    setParameter (apvts, ids::ab, ref ? 1.0f : 0.0f);
}

void ControlPanel::guess (bool xIsRef)
{
    const bool xWasRef = processor.isXTheReference();
    const bool correct = processor.submitGuess (xIsRef);
    resultText = juce::String (correct ? "Correct" : "Wrong") + ": " + (xWasRef ? "X" : "Y") + " was the reference";
    repaint();
}

void ControlPanel::showOptions()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Restart the reference when it is switched in", true, getParameter (apvts, ids::restartSwitch) > 0.5f);
    menu.addItem (2, "Restart the reference when playback starts", true, getParameter (apvts, ids::restartPlay) > 0.5f);
    menu.addSeparator();
    menu.addItem (3, "Reset curve colours");
    menu.addItem (4, "Reset blind test score");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&optionsButton), [this] (int result)
    {
        if (result == 1)       setParameter (apvts, ids::restartSwitch, getParameter (apvts, ids::restartSwitch) > 0.5f ? 0.0f : 1.0f);
        else if (result == 2)  setParameter (apvts, ids::restartPlay, getParameter (apvts, ids::restartPlay) > 0.5f ? 0.0f : 1.0f);
        else if (result == 3)  { settings.resetColours(); getParentComponent()->repaint(); }
        else if (result == 4)  { processor.resetBlindScore(); resultText = {}; }
    });
}

void ControlPanel::timerCallback()
{
    const bool blind = getParameter (apvts, ids::blind) > 0.5f;
    const bool ref = getParameter (apvts, ids::ab) > 0.5f;

    if (blind != wasBlind)
    {
        wasBlind = blind;
        resultText = {};

        if (blind)
        {
            processor.rerollBlind();
            processor.resetBlindScore();
        }
    }

    mixButton.setToggleState (! ref, juce::dontSendNotification);
    refButton.setToggleState (ref, juce::dontSendNotification);
    mixButton.setLabels (blind ? "X" : "MIX", blind ? "" : "A", blind);
    refButton.setLabels (blind ? "Y" : "REF", blind ? "" : "B", blind);

    guessX.setVisible (blind);
    guessY.setVisible (blind);

    const float mix = processor.getMixLufs(), refLufs = processor.getRefLufs();
    matchText = "Mix " + formatLufs (mix) + "   Ref " + formatLufs (refLufs) + " LUFS";

    if (getParameter (apvts, ids::matchOn) > 0.5f)
        matchText += "\nReference gain " + juce::String (processor.getMatchDb(), 1) + " dB";

    const int mode = juce::jlimit (0, 2, (int) std::lround (getParameter (apvts, ids::refMode)));
    const bool freeMode = mode == 0, beatsMode = mode == 2;

    offsetKnob.setVisible (! beatsMode);
    beatKnob.setVisible (beatsMode);
    nudgeKnob.setVisible (beatsMode);
    offsetKnob.setEnabled (mode == 1);
    offsetKnob.setAlpha (mode == 1 ? 1.0f : 0.4f);
    keyLockButton.setEnabled (beatsMode);
    alignButton.setEnabled (beatsMode ? processor.isHostBeatKnown() : processor.isHostTimeKnown());

    const int slot = juce::jlimit (0, waveref::numSlots - 1, (int) std::lround (getParameter (apvts, ids::slot)));
    const double refBpm = processor.getSlotInfo (slot).bpm;

    if (freeMode)
        syncText = "Reference runs on its own";
    else if (beatsMode && ! processor.isHostBeatKnown())
        syncText = "Host tempo not available";
    else if (beatsMode && refBpm <= 1.0)
        syncText = "Set the reference's BPM first";
    else if (! beatsMode && ! processor.isHostTimeKnown())
        syncText = "Host time not available";
    else if (! processor.isHostPlaying())
        syncText = "Host stopped";
    else if (beatsMode)
        syncText = "Tempo " + juce::String (refBpm, 1) + " > " + juce::String (processor.getHostBpm(), 1) + " BPM";
    else
        syncText = "Following the host";

    repaint();
}

void ControlPanel::resized()
{
    constexpr int top = 22;

    // Switch: x 0 .. 176
    optionsButton.setBounds (146, 4, 28, 18);
    mixButton.setBounds (8, top, 78, 66);
    refButton.setBounds (92, top, 78, 66);

    // Loudness match: x 182 .. 422
    matchButton.setBounds (192, top, 70, 24);
    matchMode.setBounds (266, top, 56, 24);
    trimKnob.setBounds (332, top - 4, 80, 74);

    // Monitor: x 428 .. 708
    monoButton.setBounds (438, top, 60, 24);
    sideButton.setBounds (502, top, 60, 24);
    deltaButton.setBounds (438, top + 30, 60, 24);
    bandButton.setBounds (502, top + 30, 60, 24);
    freqKnob.setBounds (572, top - 4, 66, 74);
    widthKnob.setBounds (640, top - 4, 62, 74);

    // Sync: x 714 .. 1060
    syncMode.setBounds (724, top, 100, 24);
    keyLockButton.setBounds (830, top, 84, 24);
    alignButton.setBounds (724, top + 30, 100, 24);
    offsetKnob.setBounds (950, top - 4, 76, 74);
    beatKnob.setBounds (916, top - 4, 72, 74);
    nudgeKnob.setBounds (988, top - 4, 72, 74);

    // Blind test: x 1066 .. 1252
    blindButton.setBounds (1076, top, 66, 24);
    guessX.setBounds (1076, top + 30, 80, 22);
    guessY.setBounds (1160, top + 30, 80, 22);
}

void ControlPanel::paint (juce::Graphics& g)
{
    const auto& pal = palette();

    auto panel = [&] (int x, int width, const char* title)
    {
        const auto r = juce::Rectangle<float> ((float) x, 0.0f, (float) width, (float) getHeight());
        g.setColour (pal.panel);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (pal.panelOutline);
        g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
        drawCaption (g, title, juce::Rectangle<int> (x + 10, 4, 120, 14), juce::Justification::centredLeft);
    };

    panel (0, 176, "Switch");
    panel (182, 240, "Loudness match");
    panel (428, 280, "Monitor");
    panel (714, 346, "Sync");
    panel (1066, 186, "Blind test");

    g.setColour (pal.text);
    g.setFont (klaud::font (12.0f));
    g.drawFittedText (matchText, juce::Rectangle<int> (192, 56, 136, 34), juce::Justification::topLeft, 2);

    g.setColour (pal.textDim);
    g.setFont (klaud::font (11.5f));
    g.drawFittedText (syncText, juce::Rectangle<int> (724, 78, 190, 16), juce::Justification::centredLeft, 1);

    if (getParameter (apvts, ids::blind) > 0.5f)
    {
        g.setColour (pal.textDim);
        g.setFont (klaud::font (12.0f));
        g.drawText ("Score " + juce::String (processor.getBlindCorrect()) + " / " + juce::String (processor.getBlindTotal()),
                    1150, 24, 94, 18, juce::Justification::centredLeft, false);
        g.setColour (pal.text);
        g.drawFittedText (resultText, juce::Rectangle<int> (1076, 76, 168, 16), juce::Justification::centredLeft, 1);
    }
    else
    {
        g.setColour (pal.textDim);
        g.setFont (klaud::font (11.5f));
        g.drawFittedText ("Pick which of two\nlevel-matched sounds\nis the reference", juce::Rectangle<int> (1148, 20, 100, 60),
                          juce::Justification::topLeft, 3);
    }
}
}
