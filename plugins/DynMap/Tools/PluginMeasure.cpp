// Loads a VST3 or VST2 (.dll) plugin and runs a WAV file through it, to measure other dynamics plugins
// with the DynMap measurement kit (see Tests/Measure.h):
//   PluginMeasure --plugin <file.vst3|file.dll> --list
//   PluginMeasure --plugin <file> [--state-in <file>] [--set "<name>=<text>"]... [--state-out <file>]
//                 [--program-chunk] [--render <in.wav> <out.wav>]
// Parameter values are given as the plugin's own text (e.g. "50%", "-6 dB"); a number in brackets,
// "<name>=[0.25]", sets the normalised value directly. --state-in/--state-out load and save the plugin's
// whole state (VST3 getStateInformation, VST2 chunk), which also holds anything that isn't a parameter,
// such as drawn curves.
#include <juce_audio_processors/juce_audio_processors.h>
#include "../Tests/Measure.h"
#include "Vst2Host.h"

namespace
{
    // What the tool needs from a hosted plugin, whichever format it is.
    struct Hosted
    {
        virtual ~Hosted() = default;
        virtual juce::String name() = 0;
        virtual int latency() = 0;
        virtual int numParams() = 0;
        virtual juce::String paramName (int) = 0;
        virtual juce::String paramText (int) = 0;
        virtual float getParam (int) = 0;
        virtual void setParam (int, float) = 0;
        virtual float valueForText (int index, const juce::String& text) = 0;
        virtual juce::MemoryBlock getState() = 0;
        virtual void setState (const juce::MemoryBlock&) = 0;
        virtual void prepare (double sr, int block) = 0;
        virtual void process (juce::AudioBuffer<float>& stereo) = 0;
        virtual void release() = 0;
    };

    struct Vst3Hosted : Hosted
    {
        explicit Vst3Hosted (std::unique_ptr<juce::AudioPluginInstance> p) : plugin (std::move (p))
        {
            plugin->enableAllBuses();
        }

        juce::String name() override { return plugin->getName(); }
        int latency() override { return plugin->getLatencySamples(); }
        int numParams() override { return plugin->getParameters().size(); }
        juce::String paramName (int i) override { return plugin->getParameters()[i]->getName (100); }
        juce::String paramText (int i) override { return plugin->getParameters()[i]->getCurrentValueAsText(); }
        float getParam (int i) override { return plugin->getParameters()[i]->getValue(); }
        void setParam (int i, float v) override { plugin->getParameters()[i]->setValueNotifyingHost (v); }
        float valueForText (int i, const juce::String& text) override { return plugin->getParameters()[i]->getValueForText (text); }

        juce::MemoryBlock getState() override
        {
            juce::MemoryBlock block;
            plugin->getStateInformation (block);
            return block;
        }

        void setState (const juce::MemoryBlock& block) override { plugin->setStateInformation (block.getData(), (int) block.getSize()); }

        void prepare (double sr, int block) override
        {
            plugin->setPlayConfigDetails (2, 2, sr, block);
            plugin->prepareToPlay (sr, block);
            io.setSize (juce::jmax (2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()), block);
        }

        void process (juce::AudioBuffer<float>& stereo) override
        {
            const int n = stereo.getNumSamples();
            io.clear();
            for (int ch = 0; ch < 2; ++ch)
                io.copyFrom (ch, 0, stereo, ch, 0, n);

            juce::AudioBuffer<float> view (io.getArrayOfWritePointers(), io.getNumChannels(), n);
            plugin->processBlock (view, midi);

            for (int ch = 0; ch < 2; ++ch)
                stereo.copyFrom (ch, 0, io, ch, 0, n);
        }

        void release() override { plugin->releaseResources(); }

        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::AudioBuffer<float> io;
        juce::MidiBuffer midi;
    };

    struct Vst2Hosted : Hosted
    {
        juce::String name() override { return plugin.getName(); }
        int latency() override { return plugin.latency(); }
        int numParams() override { return plugin.numParams(); }
        juce::String paramName (int i) override { return plugin.paramName (i); }
        juce::String paramText (int i) override { return plugin.paramText (i); }
        float getParam (int i) override { return plugin.getParam (i); }
        void setParam (int i, float v) override { plugin.setParam (i, v); }

        // VST2 has no reliable text-to-value call: scan the range for the value whose display is the
        // closest number (or the same text, for choices). Restores nothing: the caller sets the result.
        float valueForText (int i, const juce::String& text) override
        {
            const float target = text.getFloatValue();
            const bool numeric = text.containsAnyOf ("0123456789");
            float best = 0.0f, bestError = 1.0e30f;

            for (int step = 0; step <= 2000; ++step)
            {
                const float v = (float) step / 2000.0f;
                plugin.setParam (i, v);
                const auto shown = plugin.paramText (i);
                const float error = numeric ? std::abs (shown.getFloatValue() - target)
                                            : (shown.equalsIgnoreCase (text) ? 0.0f : 1.0f);
                if (error < bestError)
                {
                    bestError = error;
                    best = v;
                }
            }
            return best;
        }

        juce::MemoryBlock getState() override { return plugin.getChunk (programChunk); }
        void setState (const juce::MemoryBlock& block) override
        {
            const auto result = plugin.setChunk (block, programChunk);
            plugin.pumpMessages (300);
            std::cout << "  set " << block.getSize() << " bytes of state (plugin returned " << (int64_t) result << ")" << std::endl;
        }

        bool programChunk = false;

        void prepare (double sr, int block) override
        {
            plugin.prepare (sr, block);
            inputs.setSize (juce::jmax (2, plugin.numInputs()), block);
            outputs.setSize (juce::jmax (2, plugin.numOutputs()), block);
        }

        void process (juce::AudioBuffer<float>& stereo) override
        {
            const int n = stereo.getNumSamples();
            inputs.clear();
            outputs.clear();
            for (int ch = 0; ch < 2; ++ch)
                inputs.copyFrom (ch, 0, stereo, ch, 0, n);

            plugin.process (const_cast<float**> (inputs.getArrayOfWritePointers()), const_cast<float**> (outputs.getArrayOfWritePointers()), n);

            for (int ch = 0; ch < 2; ++ch)
                stereo.copyFrom (ch, 0, outputs, ch, 0, n);
        }

        void release() override { plugin.stop(); }

        vst2::Plugin plugin;
        juce::AudioBuffer<float> inputs, outputs;
    };

    std::unique_ptr<Hosted> load (const juce::File& file, bool programChunk, juce::String& error)
    {
        if (file.hasFileExtension ("dll"))
        {
            auto hosted = std::make_unique<Vst2Hosted>();
            hosted->programChunk = programChunk;
            if (! hosted->plugin.load (file, error))
                return nullptr;
            hosted->plugin.openHiddenEditor();
            return hosted;
        }

        juce::VST3PluginFormat format;
        juce::OwnedArray<juce::PluginDescription> types;
        format.findAllTypesForFile (types, file.getFullPathName());
        if (types.isEmpty())
        {
            error = "no VST3 plugin found";
            return nullptr;
        }

        auto plugin = format.createInstanceFromDescription (*types[0], 48000.0, 512, error);
        return plugin != nullptr ? std::make_unique<Vst3Hosted> (std::move (plugin)) : nullptr;
    }

    void list (Hosted& plugin)
    {
        std::cout << plugin.name() << ", latency " << plugin.latency() << " samples" << std::endl;
        for (int i = 0; i < plugin.numParams(); ++i)
            std::cout << "  " << juce::String (i).paddedLeft (' ', 3) << "  " << plugin.paramName (i).paddedRight (' ', 28)
                      << plugin.paramText (i) << "  [" << juce::String (plugin.getParam (i), 4) << "]" << std::endl;
    }

    bool set (Hosted& plugin, const juce::String& assignment)
    {
        const auto name = assignment.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto text = assignment.fromFirstOccurrenceOf ("=", false, false).trim();

        // "#<n>" picks a parameter by index, for plugins with duplicate names.
        int index = name.startsWith ("#") ? name.substring (1).getIntValue() : -1;
        for (int i = 0; index < 0 && i < plugin.numParams(); ++i)
            if (plugin.paramName (i).equalsIgnoreCase (name))
                index = i;

        if (index < 0 || index >= plugin.numParams())
        {
            std::cout << "No parameter called \"" << name << "\"" << std::endl;
            return false;
        }

        const float value = text.startsWith ("[") ? text.removeCharacters ("[]").getFloatValue() : plugin.valueForText (index, text);
        plugin.setParam (index, juce::jlimit (0.0f, 1.0f, value));
        std::cout << "  " << plugin.paramName (index) << " = " << plugin.paramText (index) << std::endl;
        return true;
    }

    int render (Hosted& plugin, const juce::File& in, const juce::File& out)
    {
        juce::AudioBuffer<float> audio;
        double sr = 48000.0;
        if (! measure::readAudio (in, audio, sr))
        {
            std::cout << "Could not read " << in.getFullPathName() << std::endl;
            return 1;
        }

        constexpr int block = 512;
        plugin.prepare (sr, block);

        for (int start = 0; start < audio.getNumSamples(); start += block)
        {
            const int n = juce::jmin (block, audio.getNumSamples() - start);
            juce::AudioBuffer<float> view (audio.getArrayOfWritePointers(), 2, start, n);
            plugin.process (view);
        }

        plugin.release();
        const bool ok = measure::writeWav (audio, sr, out);
        std::cout << (ok ? "Wrote " : "Could not write ") << out.getFullPathName() << " (latency " << plugin.latency() << ")" << std::endl;
        return ok ? 0 : 1;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    auto argAfter = [&] (const char* flag, int offset = 1) {
        const int i = args.indexOf (flag);
        return i >= 0 && i + offset < args.size() ? args[i + offset] : juce::String();
    };

    if (argAfter ("--plugin").isEmpty())
    {
        std::cout << "Usage: PluginMeasure --plugin <file.vst3|file.dll> [--list] [--state-in <file>] [--set \"<name>=<text>\"]..."
                     " [--state-out <file>] [--render <in.wav> <out.wav>]" << std::endl;
        return 1;
    }

    juce::String error;
    auto plugin = load (juce::File (argAfter ("--plugin")), args.contains ("--program-chunk"), error);
    if (plugin == nullptr)
    {
        std::cout << "Could not load the plugin: " << error << std::endl;
        return 1;
    }

    if (const auto stateIn = argAfter ("--state-in"); stateIn.isNotEmpty())
    {
        juce::MemoryBlock state;
        if (! juce::File (stateIn).loadFileAsData (state))
        {
            std::cout << "Could not read " << stateIn << std::endl;
            return 1;
        }
        plugin->setState (state);

    }

    for (int i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--set" && ! set (*plugin, args[i + 1]))
            return 1;

    if (args.contains ("--list"))
        list (*plugin);

    if (const auto stateOut = argAfter ("--state-out"); stateOut.isNotEmpty())
    {
        const auto state = plugin->getState();
        juce::File (stateOut).replaceWithData (state.getData(), state.getSize());
        std::cout << "Saved " << state.getSize() << " bytes of state to " << stateOut << std::endl;
    }

    if (const auto in = argAfter ("--render"), out = argAfter ("--render", 2); in.isNotEmpty() && out.isNotEmpty())
        return render (*plugin, juce::File (in), juce::File (out));

    return 0;
}
