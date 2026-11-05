#include "MainComponent.h"

#include <thread>

namespace lavalign
{
namespace
{
    klaud::Palette makePalette()
    {
        klaud::Palette p;
        p.accent = juce::Colour (0xff59c9a5);
        return p;
    }

    juce::String formatTime (double seconds, bool tenths = true)
    {
        seconds = std::max (0.0, seconds);
        const int m = (int) (seconds / 60);
        const double s = seconds - m * 60;
        return tenths ? juce::String::formatted ("%d:%04.1f", m, s) : juce::String::formatted ("%d:%02d", m, (int) s);
    }

    juce::String percent (float quality) { return juce::String (juce::jlimit (0, 100, juce::roundToInt (quality * 100.0f))) + "%"; }

    // Level of speech in a signal: RMS of its loudest 20 % of 50 ms blocks.
    double speechLevel (const std::vector<float>& x, double fs)
    {
        const size_t block = (size_t) std::max (64.0, 0.05 * fs);
        std::vector<double> e;
        for (size_t i = 0; i + block <= x.size(); i += block)
        {
            double s = 0;
            for (size_t k = 0; k < block; ++k) s += (double) x[i + k] * x[i + k];
            e.push_back (s / (double) block);
        }
        if (e.empty()) return 0.0;
        std::sort (e.begin(), e.end());
        double s = 0; size_t n = 0;
        for (size_t i = (size_t) (0.8 * (double) e.size()); i < e.size(); ++i) { s += e[i]; ++n; }
        return std::sqrt (s / (double) std::max<size_t> (1, n));
    }

    const juce::Colour boomColour { 0xffc9d1db };

    juce::String arrow() { return "  " + juce::String::charToString ((juce::juce_wchar) 0x2192) + "  "; }
    juce::String plusMinus() { return juce::String::charToString ((juce::juce_wchar) 0xb1); }
}

juce::Colour MainComponent::lavColour (int index)
{
    static const juce::Colour colours[] { juce::Colour (0xfff0b35a), juce::Colour (0xff5ab4f0), juce::Colour (0xffe57ba6),
                                          juce::Colour (0xffa98bf0), juce::Colour (0xff8fd16a), juce::Colour (0xff57d0d6) };
    return colours[(size_t) index % 6];
}

//==============================================================================
// Runs one job at a time on a background thread; posting a new job cancels the running one.
class MainComponent::Worker : private juce::Thread
{
public:
    using Job = std::function<void (const std::function<bool()>& keepGoing)>;

    Worker() : juce::Thread ("LavAlign worker") { startThread(); }
    ~Worker() override { cancel(); stopThread (30000); }

    void post (Job job)
    {
        const juce::ScopedLock sl (lock);
        cancelled = true;
        queue.clear();
        queue.push_back (std::move (job));
        notify();
    }

    void cancel()
    {
        const juce::ScopedLock sl (lock);
        cancelled = true;
        queue.clear();
    }

    bool busy() const { return running.load(); }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            Job job;
            {
                const juce::ScopedLock sl (lock);
                if (! queue.empty()) { job = std::move (queue.front()); queue.erase (queue.begin()); cancelled = false; }
            }
            if (! job) { wait (100); continue; }
            running = true;
            job ([this] { return ! cancelled.load() && ! threadShouldExit(); });
            running = false;
        }
    }

    juce::CriticalSection lock;
    std::vector<Job> queue;
    std::atomic<bool> cancelled { false }, running { false };
};

//==============================================================================
// Rows of the selected take's channels: role segments and what was found.
class ChannelList : public juce::Component
{
public:
    explicit ChannelList (MainComponent& o) : owner (o) {}

    void rebuild()
    {
        rows.clear();
        const auto* take = owner.selectedTake >= 0 && owner.selectedTake < owner.session.numTakes() ? &owner.session.take (owner.selectedTake) : nullptr;
        if (take != nullptr)
        {
            for (size_t i = 0; i < take->channels.size(); ++i)
            {
                auto row = std::make_unique<Row>();
                row->index = (int) i;
                row->role.setOptions ({ "Boom", "Lav", "Off" }, roleIndex (take->channels[i].role));
                row->role.setTooltip ("Boom: the reference, never changed.\nLav: aligned to the boom (any mic can be, e.g. a second boom or a plant).\n"
                                      "Off: left out.\nRoles follow the track name in every take.");
                const int idx = (int) i;
                row->role.onChange = [this, idx] (int sel)
                {
                    owner.session.setRole (owner.selectedTake, idx, sel == 0 ? Role::boom : sel == 1 ? Role::lav : Role::off);
                    owner.rolesChanged();
                };
                addAndMakeVisible (row->role);
                rows.push_back (std::move (row));
            }
        }
        resized();
        repaint();
    }

    void refreshRoles()
    {
        if (owner.selectedTake < 0 || owner.selectedTake >= owner.session.numTakes()) return;
        const auto& take = owner.session.take (owner.selectedTake);
        for (auto& r : rows)
            if (r->index < (int) take.channels.size())
                r->role.setSelected (roleIndex (take.channels[(size_t) r->index].role));
        repaint();
    }

    int preferredHeight() const { return headerH + (int) rows.size() * rowH + 6; }

    void resized() override
    {
        for (size_t i = 0; i < rows.size(); ++i)
            rows[i]->role.setBounds (nameW + 12, headerH + (int) i * rowH + 4, 150, rowH - 8);
    }

    void paint (juce::Graphics& g) override
    {
        auto& lnfPal = owner.lnf;
        juce::ignoreUnused (lnfPal);
        g.setColour (findColour (juce::ComboBox::backgroundColourId).withAlpha (0.35f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
        g.setFont (klaud::font (11.0f, true));
        g.setColour (juce::Colour (0xff7d8794));
        g.drawText ("CHANNEL", 12, 0, nameW, headerH, juce::Justification::centredLeft);
        g.drawText ("ROLE", nameW + 12, 0, 150, headerH, juce::Justification::centredLeft);
        g.drawText ("DELAY TO BOOM", resultX(), 0, 140, headerH, juce::Justification::centredLeft);
        g.drawText ("PHASE MATCH (SUM WITH BOOM)", resultX() + 150, 0, 260, headerH, juce::Justification::centredLeft);

        if (owner.selectedTake < 0 || owner.selectedTake >= owner.session.numTakes()) return;
        const auto& take = owner.session.take (owner.selectedTake);
        const auto lavs = take.lavChannels();
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& ch = take.channels[(size_t) rows[i]->index];
            const int y = headerH + (int) i * rowH;
            if (i % 2 == 0)
            {
                g.setColour (juce::Colours::white.withAlpha (0.025f));
                g.fillRect (4, y, getWidth() - 8, rowH);
            }
            const int lavIdx = (int) (std::find (lavs.begin(), lavs.end(), rows[i]->index) - lavs.begin());
            const auto colour = ch.role == Role::boom ? boomColour : ch.role == Role::lav ? MainComponent::lavColour (lavIdx) : juce::Colour (0xff4a525d);
            g.setColour (colour);
            g.fillRoundedRectangle (12.0f, (float) y + rowH * 0.5f - 5.0f, 10.0f, 10.0f, 2.0f);

            g.setColour (ch.role == Role::off ? juce::Colour (0xff7d8794) : juce::Colour (0xffd6dbe2));
            g.setFont (klaud::font (13.0f, ch.role != Role::off));
            const auto& info = take.files[(size_t) ch.fileIndex].info;
            const auto sub = info.numChannels > 1 ? info.file.getFileName() + "  ch " + juce::String (ch.channel + 1)
                                                  : (take.files.size() > 1 ? juce::String ("starts at ") + formatTime ((double) take.files[(size_t) ch.fileIndex].start / take.sampleRate) : juce::String());
            g.drawText (ch.name, 30, y + 2, nameW - 30, rowH / 2 + 2, juce::Justification::bottomLeft, true);
            g.setFont (klaud::font (10.5f));
            g.setColour (juce::Colour (0xff7d8794));
            g.drawText (sub, 30, y + rowH / 2 + 2, nameW - 30, rowH / 2 - 4, juce::Justification::topLeft, true);

            // results
            const LavOutcome* oc = nullptr;
            for (auto& o : owner.outcomes) if (o.channelIndex == rows[i]->index) oc = &o;
            if (oc == nullptr)
            {
                auto it = owner.summaries.find (owner.summaryKey (take));
                if (it != owner.summaries.end())
                    for (auto& o : it->second.outcomes) if (o.channelIndex == rows[i]->index) oc = &o;
            }
            g.setFont (klaud::font (12.5f));
            if (ch.role == Role::boom)
            {
                g.setColour (juce::Colour (0xff7d8794));
                g.drawText ("reference (unchanged)", resultX(), y, 260, rowH, juce::Justification::centredLeft);
            }
            else if (ch.role == Role::lav && oc != nullptr)
            {
                const auto& c = oc->result.curve;
                if (! c.found)
                {
                    g.setColour (juce::Colour (0xfff0b35a));
                    g.drawText ("no common sound with the boom: left unchanged", resultX(), y, 400, rowH, juce::Justification::centredLeft);
                    continue;
                }
                juce::String d = juce::String (c.medianDelayMs(), 2) + " ms";
                if (c.maxDelayMs() - c.minDelayMs() > 0.05f)
                    d << "  (" << juce::String (c.minDelayMs(), 2) << " .. " << juce::String (c.maxDelayMs(), 2) << ")";
                g.setColour (juce::Colour (0xffd6dbe2));
                g.drawText (d, resultX(), y, 150, rowH, juce::Justification::centredLeft);
                int x = resultX() + 150;
                g.setColour (juce::Colour (0xff7d8794));
                g.drawText (percent (oc->result.before.mean), x, y, 40, rowH, juce::Justification::centredRight);
                g.drawText (arrow().trim(), x + 42, y, 18, rowH, juce::Justification::centred);
                g.setColour (owner.lnf.findColour (juce::Slider::trackColourId));
                g.setFont (klaud::font (12.5f, true));
                g.drawText (percent (oc->result.after.mean), x + 60, y, 44, rowH, juce::Justification::centredLeft);
                if (c.polarity < 0)
                {
                    g.setFont (klaud::font (11.0f, true));
                    g.setColour (juce::Colour (0xfff0b35a));
                    g.drawText ("POLARITY FLIPPED", x + 112, y, 130, rowH, juce::Justification::centredLeft);
                }
            }
        }
    }

private:
    static int roleIndex (Role r) { return r == Role::boom ? 0 : r == Role::lav ? 1 : 2; }
    int resultX() const { return nameW + 180; }

    struct Row { int index = 0; klaud::ChoiceButtons role; };
    MainComponent& owner;
    std::vector<std::unique_ptr<Row>> rows;
    static constexpr int headerH = 24, rowH = 34, nameW = 250;
};

//==============================================================================
// Delay over time for each lav (line: what is applied, dots: raw measurements, brighter = more certain),
// over the boom's waveform. Click to move the playhead.
class DelayGraph : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit DelayGraph (MainComponent& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff12151a));
        g.fillRoundedRectangle (area, 5.0f);
        g.setColour (juce::Colour (0xff2a3039));
        g.drawRoundedRectangle (area.reduced (0.5f), 5.0f, 1.0f);

        const auto* take = owner.selectedTake >= 0 && owner.selectedTake < owner.session.numTakes() ? &owner.session.take (owner.selectedTake) : nullptr;
        if (take == nullptr)
        {
            g.setColour (juce::Colour (0xff7d8794));
            g.setFont (klaud::font (15.0f));
            g.drawFittedText (owner.session.numTakes() == 0
                                  ? "Drop recorder files or folders here\n\npoly WAVs straight from the recorder, or boom and lav files (mono AAF media, stems)\nthat share timecode"
                                  : "Select a take",
                              getLocalBounds().reduced (30), juce::Justification::centred, 6);
            return;
        }
        plot = area.reduced (46, 14).withTrimmedBottom (14).withTrimmedLeft (0);
        const double length = (double) take->length / take->sampleRate;

        // boom waveform
        if (! owner.boomEnvelope.empty())
        {
            g.setColour (boomColour.withAlpha (0.13f));
            const int n = (int) owner.boomEnvelope.size();
            juce::Path p;
            const float mid = plot.getCentreY(), h = plot.getHeight() * 0.5f;
            p.startNewSubPath (plot.getX(), mid);
            for (int i = 0; i < n; ++i) p.lineTo (plot.getX() + plot.getWidth() * (float) i / (float) (n - 1), mid - h * owner.boomEnvelope[(size_t) i]);
            for (int i = n - 1; i >= 0; --i) p.lineTo (plot.getX() + plot.getWidth() * (float) i / (float) (n - 1), mid + h * owner.boomEnvelope[(size_t) i]);
            p.closeSubPath();
            g.fillPath (p);
        }

        // delay range
        float lo = 1e9f, hi = -1e9f;
        const auto& outs = owner.outcomes;
        for (auto& o : outs)
        {
            if (! o.result.curve.found) continue;
            lo = std::min (lo, o.result.curve.minDelayMs());
            hi = std::max (hi, o.result.curve.maxDelayMs());
        }
        if (lo > hi) { lo = -1; hi = 3; }
        const float span = std::max (2.0f, hi - lo);
        lo = (lo + hi) * 0.5f - span * 0.75f; hi = lo + span * 1.5f;
        yLo = lo; yHi = hi;

        // grid
        g.setFont (klaud::font (10.5f));
        const float step = span > 8 ? 2.0f : span > 4 ? 1.0f : span > 2 ? 0.5f : 0.25f;
        for (float v = std::ceil (lo / step) * step; v <= hi; v += step)
        {
            const float y = yFor (v);
            g.setColour (juce::Colours::white.withAlpha (std::abs (v) < 1e-4f ? 0.16f : 0.05f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (juce::Colour (0xff7d8794));
            g.drawText (juce::String (v, step < 0.5f ? 2 : 1) + " ms", 2, (int) y - 7, 42, 14, juce::Justification::centredRight);
        }
        const double tStep = length > 600 ? 60 : length > 120 ? 15 : length > 30 ? 5 : length > 10 ? 2 : 0.5;
        for (double t = 0; t <= length; t += tStep)
        {
            const float x = xFor (t, length);
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (juce::Colour (0xff7d8794));
            g.drawText (formatTime (t, false), (int) x - 30, (int) plot.getBottom() + 2, 60, 12, juce::Justification::centred);
        }

        // curves
        const auto lavs = take->lavChannels();
        for (auto& o : outs)
        {
            const auto& c = o.result.curve;
            if (c.delay.empty() || ! c.found) continue;
            const int lavIdx = (int) (std::find (lavs.begin(), lavs.end(), o.channelIndex) - lavs.begin());
            const auto colour = MainComponent::lavColour (lavIdx);
            const double fileStart = (double) take->files[(size_t) take->channels[(size_t) o.channelIndex].fileIndex].start / take->sampleRate;
            const double toMs = 1000.0 / c.sampleRate;
            // measurements, thinned to about one per pixel
            const int stride = std::max (1, (int) ((double) c.delay.size() / plot.getWidth()));
            for (size_t k = 0; k < c.delay.size(); k += (size_t) stride)
            {
                float w = 0; size_t best = k;
                for (size_t j = k; j < std::min (c.delay.size(), k + (size_t) stride); ++j) if (c.weight[j] > w) { w = c.weight[j]; best = j; }
                if (w < 0.03f) continue;
                const double t = fileStart + (c.firstFrameCentre + (double) best * c.hop) / c.sampleRate;
                const float y = yFor ((float) (c.measured[best] * toMs));
                if (y < plot.getY() || y > plot.getBottom()) continue;
                g.setColour (colour.withAlpha (juce::jlimit (0.08f, 0.6f, w)));
                g.fillEllipse (xFor (t, length) - 1.5f, y - 1.5f, 3.0f, 3.0f);
            }
            juce::Path p;
            bool started = false;
            size_t nextStep = 0;
            for (size_t k = 0; k < c.delay.size(); k += (size_t) std::max (1, stride / 2))
            {
                while (nextStep < c.steps.size() && (size_t) c.steps[nextStep] <= k) { started = false; ++nextStep; }
                const double t = fileStart + (c.firstFrameCentre + (double) k * c.hop) / c.sampleRate;
                const juce::Point<float> pt (xFor (t, length), juce::jlimit (plot.getY(), plot.getBottom(), yFor ((float) (c.delay[k] * toMs))));
                if (! started) { p.startNewSubPath (pt); started = true; } else p.lineTo (pt);
            }
            g.setColour (colour);
            g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // legend
        int lx = (int) plot.getX() + 8;
        g.setFont (klaud::font (11.5f, true));
        for (size_t i = 0; i < lavs.size(); ++i)
        {
            const auto name = take->channels[(size_t) lavs[i]].name;
            const int w = juce::GlyphArrangement::getStringWidthInt (klaud::font (11.5f, true), name) + 26;
            g.setColour (juce::Colour (0xff12151a).withAlpha (0.8f));
            g.fillRoundedRectangle ((float) lx - 4, plot.getY() + 4, (float) w, 18.0f, 3.0f);
            g.setColour (MainComponent::lavColour ((int) i));
            g.fillRect ((float) lx, plot.getY() + 12, 12.0f, 2.0f);
            g.drawText (name, lx + 16, (int) plot.getY() + 4, w, 18, juce::Justification::centredLeft);
            lx += w + 6;
        }
        if (owner.analysing)
        {
            g.setColour (juce::Colour (0xff7d8794));
            g.setFont (klaud::font (12.0f));
            g.drawText ("analysing...", plot.toNearestInt().removeFromTop (24).removeFromRight (120), juce::Justification::centredRight);
        }

        // playhead
        const double pos = (double) owner.preview.getPosition() / take->sampleRate;
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawVerticalLine ((int) xFor (pos, length), plot.getY(), plot.getBottom());
    }

    void mouseDown (const juce::MouseEvent& e) override { seek (e); }
    void mouseDrag (const juce::MouseEvent& e) override { seek (e); }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto* take = owner.selectedTake >= 0 && owner.selectedTake < owner.session.numTakes() ? &owner.session.take (owner.selectedTake) : nullptr;
        if (take == nullptr || plot.isEmpty()) return;
        const double t = (e.position.x - plot.getX()) / plot.getWidth() * (double) take->length / take->sampleRate;
        const float d = yLo + (plot.getBottom() - e.position.y) / plot.getHeight() * (yHi - yLo);
        setTooltip (formatTime (t) + "   " + juce::String (d, 2) + " ms");
    }

private:
    float xFor (double t, double length) const { return plot.getX() + (float) (t / std::max (1e-9, length)) * plot.getWidth(); }
    float yFor (float ms) const { return plot.getBottom() - (ms - yLo) / (yHi - yLo) * plot.getHeight(); }

    void seek (const juce::MouseEvent& e)
    {
        const auto* take = owner.selectedTake >= 0 && owner.selectedTake < owner.session.numTakes() ? &owner.session.take (owner.selectedTake) : nullptr;
        if (take == nullptr || plot.isEmpty()) return;
        const double frac = juce::jlimit (0.0, 1.0, (double) ((e.position.x - plot.getX()) / plot.getWidth()));
        owner.preview.setPosition ((int64_t) (frac * (double) take->length));
        repaint();
    }

    MainComponent& owner;
    juce::Rectangle<float> plot;
    float yLo = 0, yHi = 1;
};

//==============================================================================
MainComponent::MainComponent() : lnf (makePalette())
{
    setLookAndFeel (&lnf);
    lnf.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff59c9a5));
    lnf.setColour (juce::TextButton::textColourOnId, juce::Colour (0xff10261f));
    lnf.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    lnf.setColour (juce::ProgressBar::foregroundColourId, juce::Colour (0xff59c9a5));
    lnf.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff1a1e24));

    juce::PropertiesFile::Options opt;
    opt.applicationName = "LavAlign";
    opt.filenameSuffix = ".settings";
    opt.folderName = "Maki plugins/LavAlign";
    opt.osxLibrarySubFolder = "Application Support";
    properties.setStorageParameters (opt);

    worker = std::make_unique<Worker>();
    channelList = std::make_unique<ChannelList> (*this);
    graph = std::make_unique<DelayGraph> (*this);

    addAndMakeVisible (addButton);
    addAndMakeVisible (clearButton);
    addAndMakeVisible (takeList);
    addAndMakeVisible (*channelList);
    addAndMakeVisible (*graph);
    takeList.setRowHeight (40);
    takeList.setMultipleSelectionEnabled (false);
    takeList.setColour (juce::ListBox::outlineColourId, juce::Colour (0xff2a3039));
    takeList.setOutlineThickness (1);

    addButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Recorder files or folders", juce::File(), "*.wav;*.bwf;*.rf64;*.aif;*.aiff;*.flac");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc) { addFiles (fc.getResults()); });
    };
    clearButton.onClick = [this]
    {
        if (exporting) return;
        preview.stop();
        preview.setData (nullptr);
        session.clear();
        outcomes.clear(); boomTimeline.clear(); boomEnvelope.clear(); outcomesKey.clear(); summaries.clear();
        selectedTake = -1;
        takeList.updateContent();
        channelList->rebuild();
        updateButtons();
        setStatus ({});
        repaint();
    };

    // preview
    addAndMakeVisible (playButton);
    addAndMakeVisible (abButtons);
    addAndMakeVisible (listenButtons);
    addAndMakeVisible (timeLabel);
    abButtons.setOptions ({ "Original", "Aligned" }, 1);
    abButtons.setTooltip ("Hear the lavs as recorded or aligned (switches smoothly). Shortcut: A");
    abButtons.onChange = [this] (int i) { preview.setAligned (i == 1); };
    listenButtons.setOptions ({ "Boom + lavs", "Lavs", "Boom" }, 0);
    listenButtons.setTooltip ("Boom + lavs: what a mix of both sounds like (levels matched). Comb filtering shows up as a hollow, phasey sound.");
    listenButtons.onChange = [this] (int i) { preview.setListen ((Preview::Listen) i); };
    playButton.setClickingTogglesState (false);
    playButton.onClick = [this]
    {
        if (preview.isPlaying()) preview.stop(); else preview.play();
        updateButtons();
    };
    timeLabel.setJustificationType (juce::Justification::centredRight);
    timeLabel.setFont (klaud::font (12.5f));
    timeLabel.setColour (juce::Label::textColourId, juce::Colour (0xff7d8794));

    // settings
    auto setupLabel = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (klaud::font (11.0f, true));
        l.setColour (juce::Label::textColourId, juce::Colour (0xff7d8794));
        l.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (l);
    };
    setupLabel (phaseLabel, "PHASE CORRECTION");
    setupLabel (rangeLabel, "SEARCH");
    setupLabel (trackLabel, "FOLLOW");
    setupLabel (outputLabel, "OUTPUT");
    setupLabel (namesLabel, "NAMES");

    phaseSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    phaseSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    phaseSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    phaseSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    phaseSlider.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffd6dbe2));
    phaseSlider.setRange (0, 100, 1);
    phaseSlider.setTextValueSuffix (" %");
    phaseSlider.setValue (100, juce::dontSendNotification);
    phaseSlider.setTooltip ("After time alignment, turns the lav's remaining phase difference to the boom towards zero "
                            "(different mics, chest vs. air, recorder filters). Changes no levels or tone on its own; 0 = time alignment only.");
    phaseSlider.onDragEnd = [this] { settingsChanged(); };
    phaseSlider.onValueChange = [this] { if (! phaseSlider.isMouseButtonDown()) settingsChanged(); };
    addAndMakeVisible (phaseSlider);

    rangeBox.addItemList ({ plusMinus() + "10 ms (same recorder)", plusMinus() + "50 ms (wireless, 1 frame)", plusMinus() + "250 ms (separate recorders)", plusMinus() + "1 s" }, 1);
    rangeBox.setSelectedItemIndex (1, juce::dontSendNotification);
    rangeBox.setTooltip ("How far apart boom and lav may be in time. Wireless lavs arrive a few ms late; files from separate recorders "
                         "synced by timecode can be off by a frame or more.");
    rangeBox.onChange = [this] { settingsChanged(); };
    addAndMakeVisible (rangeBox);

    trackButtons.setOptions ({ "Steady", "Normal", "Fast" }, 1);
    trackButtons.setTooltip ("How quickly the delay follows movement of the actor and the boom. "
                             "Steady averages more (static scenes), Fast follows quick moves (walk and talk).");
    trackButtons.onChange = [this] (int) { settingsChanged(); };
    addAndMakeVisible (trackButtons);

    outputButtons.setOptions ({ "Same poly file", "Split lavs" }, 0);
    outputButtons.setTooltip ("Same poly file: a copy of each recorder file with the lav channels aligned, everything else (boom, mix, metadata) untouched.\n"
                              "Split lavs: each aligned lav as its own mono file (timecode kept). Mono source files always stay mono.");
    outputButtons.onChange = [this] (int) { saveSettings(); };
    addAndMakeVisible (outputButtons);
    namesButtons.setOptions ({ "Same", "+ _aligned" }, 0);
    namesButtons.setTooltip ("Same: files keep their names (in another folder), so a DAW session or AAF can be relinked to them.");
    namesButtons.onChange = [this] (int) { saveSettings(); };
    addAndMakeVisible (namesButtons);

    folderButton.onClick = [this]
    {
        juce::PopupMenu m;
        m.setLookAndFeel (&getLookAndFeel());
        m.addItem (1, "\"Aligned\" folder next to each source file", true, exportFolder == juce::File());
        m.addItem (2, exportFolder == juce::File() ? juce::String ("Choose a folder...") : "Choose a folder... (" + exportFolder.getFullPathName() + ")",
                   true, exportFolder != juce::File());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&folderButton), [this] (int r)
        {
            if (r == 1) { exportFolder = juce::File(); updateButtons(); saveSettings(); }
            if (r == 2)
            {
                chooser = std::make_unique<juce::FileChooser> ("Export folder", exportFolder);
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                      [this] (const juce::FileChooser& fc)
                                      {
                                          if (fc.getResult() != juce::File()) { exportFolder = fc.getResult(); updateButtons(); saveSettings(); }
                                      });
            }
        });
    };
    addAndMakeVisible (folderButton);

    exportButton.onClick = [this]
    {
        if (exporting) { worker->cancel(); return; }
        startExport();
    };
    exportButton.setToggleState (true, juce::dontSendNotification);  // drawn in the accent colour: the main action
    addAndMakeVisible (exportButton);
    addAndMakeVisible (progressBar);
    progressBar.setPercentageDisplay (false);
    statusLabel.setFont (klaud::font (12.5f));
    statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff7d8794));
    addAndMakeVisible (statusLabel);

    loadSettings();
    preview.setListen (Preview::Listen::sum);
    setWantsKeyboardFocus (true);
    updateButtons();
    setSize (1180, 760);
    startTimerHz (25);
}

MainComponent::~MainComponent()
{
    stopTimer();
    worker.reset();
    setLookAndFeel (nullptr);
}

//==============================================================================
void MainComponent::loadSettings()
{
    auto* p = properties.getUserSettings();
    if (p == nullptr) return;
    phaseSlider.setValue (p->getDoubleValue ("phase", 100.0), juce::dontSendNotification);
    rangeBox.setSelectedItemIndex (p->getIntValue ("range", 1), juce::dontSendNotification);
    trackButtons.setSelected (p->getIntValue ("follow", 1));
    outputButtons.setSelected (p->getIntValue ("output", 0));
    namesButtons.setSelected (p->getIntValue ("names", 0));
    exportFolder = juce::File (p->getValue ("folder", {}));
    if (! exportFolder.isDirectory()) exportFolder = juce::File();
}

void MainComponent::saveSettings()
{
    auto* p = properties.getUserSettings();
    if (p == nullptr) return;
    p->setValue ("phase", phaseSlider.getValue());
    p->setValue ("range", rangeBox.getSelectedItemIndex());
    p->setValue ("follow", trackButtons.getSelected());
    p->setValue ("output", outputButtons.getSelected());
    p->setValue ("names", namesButtons.getSelected());
    p->setValue ("folder", exportFolder.getFullPathName());
    p->saveIfNeeded();
}

Settings MainComponent::currentSettings() const
{
    Settings s;
    s.phaseCorrection = (float) (phaseSlider.getValue() / 100.0);
    static const double ranges[] { 10.0, 50.0, 250.0, 1000.0 };
    s.maxOffsetMs = ranges[(size_t) juce::jlimit (0, 3, rangeBox.getSelectedItemIndex())];
    static const double smooth[] { 160.0, 80.0, 40.0 };
    s.smoothingMs = smooth[(size_t) juce::jlimit (0, 2, trackButtons.getSelected())];
    return s;
}

ExportOptions MainComponent::currentExportOptions() const
{
    ExportOptions o;
    o.folder = exportFolder;
    o.splitPoly = outputButtons.getSelected() == 1;
    o.suffix = namesButtons.getSelected() == 1 ? "_aligned" : "";
    return o;
}

juce::String MainComponent::summaryKey (const Take& take) const
{
    const auto s = currentSettings();
    juce::String k = take.signature();
    for (auto& ch : take.channels) k << (int) ch.role;
    k << "|" << s.phaseCorrection << "|" << s.maxOffsetMs << "|" << s.smoothingMs;
    return k;
}

//==============================================================================
void MainComponent::addFiles (const juce::Array<juce::File>& files)
{
    if (exporting || files.isEmpty()) return;
    const auto previous = selectedTake >= 0 && selectedTake < session.numTakes() ? session.take (selectedTake).signature() : juce::String();
    const auto messages = session.addFiles (files);
    takeList.updateContent();
    int select = 0;
    for (int i = 0; i < session.numTakes(); ++i)
        if (session.take (i).signature() == previous) select = i;
    if (session.numTakes() > 0) selectTake (previous.isEmpty() ? 0 : select);
    if (! messages.isEmpty())
        setStatus (juce::String (messages.size()) + " file(s) skipped: " + messages.joinIntoString ("; "), true);
    else
        setStatus (juce::String (session.numTakes()) + " take(s). Check the roles, listen, then Align & export all.");
    updateButtons();
}

void MainComponent::filesDropped (const juce::StringArray& paths, int, int)
{
    dragOver = false;
    juce::Array<juce::File> files;
    for (auto& p : paths) files.add (juce::File (p));
    addFiles (files);
    repaint();
}

void MainComponent::selectTake (int index)
{
    if (index == selectedTake && index >= 0) return;
    selectedTake = index;
    takeList.selectRow (index, false, true);
    preview.stop();
    preview.setData (nullptr);
    preview.setPosition (0);
    outcomes.clear(); boomTimeline.clear(); boomEnvelope.clear(); outcomesKey.clear();
    channelList->rebuild();
    resized();
    startAnalysis();
    updateButtons();
    repaint();
}

void MainComponent::rolesChanged()
{
    channelList->refreshRoles();
    takeList.repaint();
    scheduleAnalysis();
    updateButtons();
}

void MainComponent::settingsChanged()
{
    saveSettings();
    scheduleAnalysis();
}

void MainComponent::scheduleAnalysis()
{
    analysisPending = 8;  // ~0.3 s: let a slider settle
}

void MainComponent::startAnalysis()
{
    analysisPending = 0;
    if (exporting || selectedTake < 0 || selectedTake >= session.numTakes()) return;
    const auto& take = session.take (selectedTake);
    const auto key = summaryKey (take);
    if (key == outcomesKey) return;

    const int gen = ++generation;
    const auto problem = take.problem();
    if (problem.isNotEmpty())
    {
        outcomes.clear(); outcomesKey.clear();
        analysing = false;
        setStatus (take.name + ": " + problem, true);
        // the boom waveform can still be shown
        buildPreview();
        repaint();
        return;
    }
    analysing = true;
    progress = 0.0;
    setStatus ("Analysing " + take.name + "...");
    auto takeCopy = std::make_shared<Take> (take);
    const auto settings = currentSettings();
    juce::Component::SafePointer<MainComponent> safe (this);
    worker->post ([safe, takeCopy, settings, gen, this] (const std::function<bool()>& keepGoing)
    {
        std::vector<LavOutcome> outs;
        std::vector<float> boom;
        const auto err = processTake (*takeCopy, settings, outs, [this, &keepGoing, gen] (float p)
        {
            if (gen == generation.load()) progress = p;
            return keepGoing();
        }, true, true, &boom);
        if (! keepGoing()) return;
        juce::MessageManager::callAsync ([safe, gen, err, outs = std::move (outs), boom = std::move (boom)]() mutable
        {
            if (safe != nullptr) safe->analysisFinished (gen, err, std::move (outs), std::move (boom));
        });
    });
    repaint();
}

void MainComponent::analyseSelectedNow()
{
    if (selectedTake < 0 || selectedTake >= session.numTakes()) return;
    const int gen = ++generation;
    std::vector<LavOutcome> outs;
    std::vector<float> boom;
    const auto err = processTake (session.take (selectedTake), currentSettings(), outs, {}, true, true, &boom);
    analysing = true;
    analysisFinished (gen, err, std::move (outs), std::move (boom));
}

void MainComponent::analysisFinished (int gen, juce::String error, std::vector<LavOutcome> outs, std::vector<float> boom)
{
    if (gen != generation.load()) return;
    analysing = false;
    progress = 0.0;
    if (selectedTake < 0 || selectedTake >= session.numTakes()) return;
    const auto& take = session.take (selectedTake);
    if (error.isNotEmpty())
    {
        setStatus (take.name + ": " + error, true);
        repaint();
        return;
    }
    outcomes = std::move (outs);
    boomTimeline = std::move (boom);
    outcomesKey = summaryKey (take);
    Summary sum { outcomesKey, {}, {} };
    for (auto& o : outcomes)
    {
        LavOutcome light;
        light.channelIndex = o.channelIndex;
        light.result.curve = o.result.curve;
        light.result.before = o.result.before;
        light.result.after = o.result.after;
        sum.outcomes.push_back (std::move (light));
    }
    summaries[outcomesKey] = std::move (sum);
    buildPreview();
    setStatus (take.name + ": " + summaryFor (take) + "   Space plays, A switches original / aligned, click the graph to move.");
    channelList->repaint();
    takeList.repaint();
    repaint();
}

void MainComponent::buildPreview()
{
    if (selectedTake < 0 || selectedTake >= session.numTakes()) return;
    const auto& take = session.take (selectedTake);
    auto data = std::make_shared<PreviewData>();
    data->sampleRate = take.sampleRate;
    if (boomTimeline.empty())
    {
        // no analysis (yet): show and play the boom alone if there is one
        const int b = take.boomChannel();
        if (b >= 0)
        {
            std::vector<std::vector<float>> d;
            const auto& ch = take.channels[(size_t) b];
            if (readChannels (take.files[(size_t) ch.fileIndex].info, { ch.channel }, d).isEmpty())
            {
                boomTimeline.assign ((size_t) take.length, 0.0f);
                std::copy (d[0].begin(), d[0].end(), boomTimeline.begin() + take.files[(size_t) ch.fileIndex].start);
            }
        }
    }
    data->boom = boomTimeline;
    const double boomLevel = speechLevel (boomTimeline, take.sampleRate);
    for (auto& o : outcomes)
    {
        const auto start = (size_t) take.files[(size_t) take.channels[(size_t) o.channelIndex].fileIndex].start;
        const double lavLevel = speechLevel (o.original, take.sampleRate);
        const float g = lavLevel > 1e-9 && boomLevel > 1e-9 ? (float) std::min (1000.0, boomLevel / lavLevel) : 1.0f;
        std::vector<float> orig ((size_t) take.length, 0.0f), al ((size_t) take.length, 0.0f);
        for (size_t i = 0; i < o.original.size() && start + i < orig.size(); ++i) orig[start + i] = o.original[i] * g;
        for (size_t i = 0; i < o.result.aligned.size() && start + i < al.size(); ++i) al[start + i] = o.result.aligned[i] * g;
        data->lavOriginal.push_back (std::move (orig));
        data->lavAligned.push_back (std::move (al));
    }
    // waveform overview
    boomEnvelope.assign (2000, 0.0f);
    float peak = 1e-9f;
    if (! boomTimeline.empty())
    {
        const double per = (double) boomTimeline.size() / 2000.0;
        for (int i = 0; i < 2000; ++i)
        {
            const auto a = (size_t) (i * per), b = std::min (boomTimeline.size(), (size_t) ((i + 1) * per) + 1);
            float m = 0; for (size_t k = a; k < b; ++k) m = std::max (m, std::abs (boomTimeline[k]));
            boomEnvelope[(size_t) i] = m; peak = std::max (peak, m);
        }
        for (auto& v : boomEnvelope) v /= peak;
    }
    preview.setData (data);
}

juce::String MainComponent::summaryFor (const Take& take) const
{
    auto it = summaries.find (summaryKey (take));
    if (it == summaries.end()) return {};
    juce::String s;
    for (auto& o : it->second.outcomes)
    {
        const auto& c = o.result.curve;
        if (s.isNotEmpty()) s << ",  ";
        s << take.channels[(size_t) o.channelIndex].name << " ";
        if (! c.found) { s << "not found"; continue; }
        s << juce::String (c.medianDelayMs(), 2) << " ms" << (c.polarity < 0 ? " (flipped)" : "")
          << " " << percent (o.result.before.mean) << arrow() << percent (o.result.after.mean);
    }
    return s;
}

//==============================================================================
void MainComponent::startExport()
{
    if (session.numTakes() == 0) return;
    preview.stop();
    std::vector<std::shared_ptr<Take>> takes;
    int skipped = 0;
    for (int i = 0; i < session.numTakes(); ++i)
    {
        if (const auto p = session.take (i).problem(); p.isNotEmpty()) { if (! p.startsWith ("Nothing")) ++skipped; continue; }
        takes.push_back (std::make_shared<Take> (session.take (i)));
    }
    if (takes.empty()) { setStatus ("Nothing to export: no take has a boom and a lav", true); return; }
    exporting = true;
    exportIndex = 0; exportFiles = 0;
    exportErrors.clear();
    progress = 0.0;
    ++generation;  // a running analysis is dropped
    analysing = false;
    updateButtons();

    const auto settings = currentSettings();
    const auto options = currentExportOptions();
    juce::Component::SafePointer<MainComponent> safe (this);
    worker->post ([safe, takes, settings, options, skipped, this] (const std::function<bool()>& keepGoing)
    {
        const int n = (int) takes.size();
        int files = 0;
        juce::StringArray errors;
        if (skipped > 0) errors.add (juce::String (skipped) + " take(s) without a boom skipped");
        for (int i = 0; i < n && keepGoing(); ++i)
        {
            const auto& take = *takes[(size_t) i];
            std::vector<LavOutcome> outs;
            auto err = processTake (take, settings, outs, [&] (float p)
            {
                progress = (i + 0.9 * p) / n;
                return keepGoing();
            }, true, true);
            if (! keepGoing()) break;
            if (err.isEmpty())
            {
                juce::String e;
                const auto written = exportTake (take, outs, options, e, [&] (float p) { progress = (i + 0.9 + 0.1 * p) / n; return keepGoing(); });
                files += written.size();
                if (e.isNotEmpty()) errors.add (take.name + ": " + e);
            }
            else
                errors.add (take.name + ": " + err);
            // keep the curves for the list
            std::vector<LavOutcome> light;
            for (auto& o : outs)
            {
                LavOutcome l; l.channelIndex = o.channelIndex; l.result.curve = o.result.curve; l.result.before = o.result.before; l.result.after = o.result.after;
                light.push_back (std::move (l));
            }
            auto takeCopy = takes[(size_t) i];
            juce::MessageManager::callAsync ([safe, takeCopy, light = std::move (light), i, n]() mutable
            {
                if (safe == nullptr) return;
                safe->summaries[safe->summaryKey (*takeCopy)] = { safe->summaryKey (*takeCopy), std::move (light), {} };
                safe->setStatus ("Exported " + juce::String (i + 1) + " of " + juce::String (n) + ": " + takeCopy->name);
                safe->takeList.repaint();
            });
        }
        const bool cancelled = ! keepGoing();
        juce::MessageManager::callAsync ([safe, files, errors, cancelled]
        {
            if (safe == nullptr) return;
            safe->exporting = false;
            safe->progress = 0.0;
            if (cancelled) safe->setStatus ("Export cancelled after " + juce::String (files) + " file(s)", true);
            else if (! errors.isEmpty()) safe->setStatus (juce::String (files) + " file(s) written. Problems: " + errors.joinIntoString ("; "), true);
            else safe->setStatus (juce::String (files) + " aligned file(s) written" + (safe->exportFolder == juce::File() ? " to the \"Aligned\" folders next to the sources." : " to " + safe->exportFolder.getFullPathName()));
            safe->updateButtons();
            safe->startAnalysis();
        });
    });
    // the worker cancels the export when cancel() is called; a cancelled job still posts the result above
}

void MainComponent::updateButtons()
{
    playButton.setButtonText (preview.isPlaying() ? "Stop" : "Play");
    playButton.setEnabled (selectedTake >= 0 && ! exporting);
    exportButton.setButtonText (exporting ? "Cancel" : "Align & export all");
    exportButton.setEnabled (session.numTakes() > 0 || exporting);
    addButton.setEnabled (! exporting);
    clearButton.setEnabled (! exporting && session.numTakes() > 0);
    folderButton.setButtonText (exportFolder == juce::File() ? juce::String ("To: \"Aligned\" next to sources") : "To: " + exportFolder.getFileName());
    folderButton.setTooltip (exportFolder == juce::File() ? juce::String ("Each file's results go to an \"Aligned\" folder beside it") : exportFolder.getFullPathName());
    progressBar.setVisible (exporting || analysing);
}

void MainComponent::setStatus (const juce::String& text, bool warning)
{
    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, warning ? juce::Colour (0xfff0b35a) : juce::Colour (0xff9aa4b1));
    statusLabel.setTooltip (text);
}

void MainComponent::timerCallback()
{
    if (analysisPending > 0 && --analysisPending == 0)
        startAnalysis();
    progressBar.setVisible (exporting || analysing);
    if (selectedTake >= 0 && selectedTake < session.numTakes())
    {
        const auto& take = session.take (selectedTake);
        timeLabel.setText (formatTime ((double) preview.getPosition() / take.sampleRate) + " / " + formatTime ((double) take.length / take.sampleRate),
                           juce::dontSendNotification);
    }
    const bool playing = preview.isPlaying();
    if (playButton.getButtonText() != (playing ? "Stop" : "Play")) updateButtons();
    if (playing || analysing) graph->repaint();
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        if (selectedTake >= 0 && ! exporting) playButton.triggerClick();
        return true;
    }
    if (key.getTextCharacter() == 'a' || key.getTextCharacter() == 'A')
    {
        const int next = abButtons.getSelected() == 1 ? 0 : 1;
        abButtons.setSelected (next);
        preview.setAligned (next == 1);
        return true;
    }
    return false;
}

//==============================================================================
int MainComponent::getNumRows() { return session.numTakes(); }

void MainComponent::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= session.numTakes()) return;
    const auto& take = session.take (row);
    if (selected)
    {
        g.setColour (juce::Colour (0xff59c9a5).withAlpha (0.16f));
        g.fillRoundedRectangle (2.0f, 1.0f, (float) width - 4.0f, (float) height - 2.0f, 4.0f);
    }
    g.setColour (juce::Colour (0xffd6dbe2));
    g.setFont (klaud::font (13.0f, true));
    g.drawText (take.name, 10, 3, width - 70, height / 2, juce::Justification::bottomLeft, true);
    g.setFont (klaud::font (11.0f));
    g.setColour (juce::Colour (0xff7d8794));
    g.drawText (formatTime ((double) take.length / take.sampleRate, false), width - 60, 3, 52, height / 2, juce::Justification::bottomRight);

    juce::String line;
    juce::Colour colour (0xff7d8794);
    const auto problem = take.problem();
    if (problem.isNotEmpty()) { line = problem; colour = problem.startsWith ("Nothing") ? juce::Colour (0xff5d6672) : juce::Colour (0xfff0b35a); }
    else
    {
        auto it = summaries.find (summaryKey (take));
        if (it == summaries.end())
        {
            int lavs = (int) take.lavChannels().size();
            line = juce::String (lavs) + (lavs == 1 ? " lav" : " lavs") + " to align";
            if (const auto& f = take.files.front().info; f.timeReference) line << "  -  TC " << f.timecodeString();
        }
        else
        {
            float worst = 1.0f; float before = 1.0f; bool notFound = false;
            for (auto& o : it->second.outcomes)
            {
                worst = std::min (worst, o.result.after.mean);
                before = std::min (before, o.result.before.mean);
                notFound = notFound || ! o.result.curve.found;
            }
            line = juce::String ("phase match ") + percent (before) + arrow() + percent (worst);
            colour = notFound ? juce::Colour (0xfff0b35a) : juce::Colour (0xff59c9a5);
            if (notFound) line << "  (a lav was not found)";
        }
    }
    g.setColour (colour);
    g.drawText (line, 10, height / 2 + 1, width - 16, height / 2 - 4, juce::Justification::topLeft, true);
}

void MainComponent::selectedRowsChanged (int lastRowSelected)
{
    if (lastRowSelected >= 0) selectTake (lastRowSelected);
}

void MainComponent::deleteKeyPressed (int row)
{
    if (exporting || row < 0) return;
    preview.stop();
    preview.setData (nullptr);
    session.removeTake (row);
    selectedTake = -1;
    takeList.updateContent();
    if (session.numTakes() > 0) selectTake (std::min (row, session.numTakes() - 1));
    else { channelList->rebuild(); outcomes.clear(); boomTimeline.clear(); boomEnvelope.clear(); outcomesKey.clear(); }
    updateButtons();
    repaint();
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.setGradientFill (juce::ColourGradient (klaud::colours::backgroundTop, 0, 0, klaud::colours::backgroundBottom, 0, (float) getHeight(), false));
    g.fillAll();
    g.setColour (juce::Colour (0xffd6dbe2));
    g.setFont (klaud::font (19.0f, true));
    g.drawText ("LavAlign", 16, 0, 120, 44, juce::Justification::centredLeft);
    g.setFont (klaud::font (11.0f, true));
    g.setColour (juce::Colour (0xff7d8794));
    g.drawText ("MAKI PLUGINS  " + juce::String (JUCE_APPLICATION_VERSION_STRING), 110, 2, 200, 44, juce::Justification::centredLeft);
    g.setFont (klaud::font (12.0f));
    g.drawText ("Phase-aligns lavalier mics to the boom", 300, 0, 400, 44, juce::Justification::centredLeft);
    g.setColour (juce::Colour (0xff2a3039));
    g.drawHorizontalLine (44, 0.0f, (float) getWidth());
    g.drawHorizontalLine (getHeight() - 92, 0.0f, (float) getWidth());

    g.setFont (klaud::font (11.0f, true));
    g.setColour (juce::Colour (0xff7d8794));
    g.drawText ("TAKES", 16, 52, 100, 20, juce::Justification::centredLeft);
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    if (dragOver)
    {
        g.setColour (juce::Colour (0xff59c9a5).withAlpha (0.12f));
        g.fillAll();
        g.setColour (juce::Colour (0xff59c9a5));
        g.drawRect (getLocalBounds(), 2);
    }
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    r.removeFromTop (44);
    auto bottom = r.removeFromBottom (92).reduced (16, 8);

    // settings row
    auto row1 = bottom.removeFromTop (32);
    phaseLabel.setBounds (row1.removeFromLeft (118));
    row1.removeFromLeft (6);
    phaseSlider.setBounds (row1.removeFromLeft (220).reduced (0, 4));
    row1.removeFromLeft (14);
    rangeLabel.setBounds (row1.removeFromLeft (52));
    row1.removeFromLeft (6);
    rangeBox.setBounds (row1.removeFromLeft (220).reduced (0, 4));
    row1.removeFromLeft (14);
    trackLabel.setBounds (row1.removeFromLeft (56));
    row1.removeFromLeft (6);
    trackButtons.setBounds (row1.removeFromLeft (200).reduced (0, 4));

    bottom.removeFromTop (6);
    auto row2 = bottom.removeFromTop (34);
    exportButton.setBounds (row2.removeFromRight (180));
    row2.removeFromRight (10);
    outputLabel.setBounds (row2.removeFromLeft (58));
    row2.removeFromLeft (6);
    outputButtons.setBounds (row2.removeFromLeft (200).reduced (0, 4));
    row2.removeFromLeft (14);
    namesLabel.setBounds (row2.removeFromLeft (50));
    row2.removeFromLeft (6);
    namesButtons.setBounds (row2.removeFromLeft (150).reduced (0, 4));
    row2.removeFromLeft (14);
    folderButton.setBounds (row2.removeFromLeft (std::min (260, row2.getWidth())).reduced (0, 4));

    auto body = r.reduced (16, 8);
    auto left = body.removeFromLeft (290);
    left.removeFromTop (22);
    auto buttons = left.removeFromBottom (30);
    clearButton.setBounds (buttons.removeFromRight (70));
    buttons.removeFromRight (8);
    addButton.setBounds (buttons);
    left.removeFromBottom (8);
    takeList.setBounds (left);
    body.removeFromLeft (14);

    auto status = body.removeFromBottom (24);
    progressBar.setBounds (status.removeFromRight (180).reduced (0, 5));
    statusLabel.setBounds (status);
    body.removeFromBottom (4);

    auto previewRow = body.removeFromBottom (34);
    playButton.setBounds (previewRow.removeFromLeft (80).reduced (0, 3));
    previewRow.removeFromLeft (10);
    abButtons.setBounds (previewRow.removeFromLeft (170).reduced (0, 5));
    previewRow.removeFromLeft (10);
    listenButtons.setBounds (previewRow.removeFromLeft (230).reduced (0, 5));
    timeLabel.setBounds (previewRow);
    body.removeFromBottom (6);

    const int listH = std::min (channelList->preferredHeight(), body.getHeight() / 2);
    channelList->setBounds (body.removeFromTop (std::max (listH, 30)));
    body.removeFromTop (8);
    graph->setBounds (body);
}

//==============================================================================
int renderSnapshots (const juce::File& folder)
{
    folder.createDirectory();
    // a synthetic poly take: boom + two lavs (one moving, one flipped)
    const double fs = 48000;
    const int n = (int) (fs * 20);
    juce::Random rng (2);
    std::vector<float> src ((size_t) n, 0.0f);
    double ph = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs;
        const double env = std::max (0.0, std::sin (2 * 3.14159 * t * 1.3)) * (std::fmod (t, 4.0) < 3.0 ? 1.0 : 0.0);
        ph += (120 + 30 * std::sin (t)) / fs;
        src[(size_t) i] = (float) (env * (std::fmod (ph, 1.0) < 0.05 ? 0.8 : 0.0) + rng.nextFloat() * 0.02 * env);
    }
    auto shifted = [&] (double ms, float sign)
    {
        DelayCurve c; c.sampleRate = fs; c.hop = 64; c.found = true; c.polarity = sign;
        for (int k = 0; k < n / 64 + 2; ++k) { const double t = k * 64 / fs; c.delay.push_back ((float) ((ms + 0.8 * std::sin (t)) * fs / 1000)); c.weight.push_back (1); c.measured.push_back (0); }
        std::vector<float> y ((size_t) n);
        renderAligned (src.data(), y.data(), n, c);
        return y;
    };
    auto boom = shifted (2.0, 1.0f);
    for (auto& v : boom) v = v * 0.5f + rng.nextFloat() * 0.002f;
    auto lavB = shifted (0.3, -1.0f);
    const juce::String ixml = "<BWFXML><SCENE>24</SCENE><TAKE>3</TAKE><TRACK_LIST>"
                              "<TRACK><INTERLEAVE_INDEX>1</INTERLEAVE_INDEX><NAME>Boom</NAME></TRACK>"
                              "<TRACK><INTERLEAVE_INDEX>2</INTERLEAVE_INDEX><NAME>Lav Anna</NAME></TRACK>"
                              "<TRACK><INTERLEAVE_INDEX>3</INTERLEAVE_INDEX><NAME>Lav Tom</NAME></TRACK></TRACK_LIST></BWFXML>";
    const auto file = folder.getChildFile ("snapshot_take.wav");
    writeSimpleWav (file, { boom.data(), src.data(), lavB.data() }, n, fs, 24, false, (int64_t) (fs * 3600 * 10), ixml);

    MainComponent mc;
    mc.addFiles ({ file });
    mc.selectTake (0);
    mc.analyseSelectedNow();
    for (float scale : { 1.0f, 1.25f })
    {
        auto img = mc.createComponentSnapshot (mc.getLocalBounds(), true, scale);
        const auto out = folder.getChildFile ("LavAlign_" + juce::String (scale, 2) + "x.png");
        out.deleteFile();
        juce::FileOutputStream os (out);
        juce::PNGImageFormat().writeImageToStream (img, os);
        std::cout << "wrote " << out.getFullPathName() << std::endl;
    }
    file.deleteFile();
    return 0;
}
}
