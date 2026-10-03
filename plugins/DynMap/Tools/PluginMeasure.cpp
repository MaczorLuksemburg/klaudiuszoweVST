// Loads a VST3 plugin and runs a WAV file through it, to measure other dynamics plugins with the
// DynMap measurement kit (see Tests/Measure.h):
//   PluginMeasure --plugin <file.vst3> --list
//   PluginMeasure --plugin <file.vst3> [--set "<name>=<text>"]... --render <in.wav> <out.wav>
// Parameter values are given as the plugin's own text (e.g. "50%", "-6 dB"); a number in brackets,
// "<name>=[0.25]", sets the normalised value directly.
#include <juce_audio_processors/juce_audio_processors.h>
#include "../Tests/Measure.h"

namespace
{
    juce::AudioProcessorParameter* findParam (juce::AudioPluginInstance& plugin, const juce::String& name)
    {
        for (auto* p : plugin.getParameters())
            if (p->getName (100).equalsIgnoreCase (name))
                return p;
        return nullptr;
    }

    void list (juce::AudioPluginInstance& plugin)
    {
        std::cout << plugin.getName() << ", latency " << plugin.getLatencySamples() << " samples" << std::endl;
        for (auto* p : plugin.getParameters())
            std::cout << "  " << juce::String (p->getParameterIndex()).paddedLeft (' ', 3) << "  "
                      << p->getName (100).paddedRight (' ', 28) << p->getCurrentValueAsText()
                      << "  [" << juce::String (p->getValue(), 4) << "]" << std::endl;
    }

    bool set (juce::AudioPluginInstance& plugin, const juce::String& assignment)
    {
        const auto name = assignment.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto text = assignment.fromFirstOccurrenceOf ("=", false, false).trim();
        auto* p = findParam (plugin, name);

        if (p == nullptr)
        {
            std::cout << "No parameter called \"" << name << "\"" << std::endl;
            return false;
        }

        const float value = text.startsWith ("[") ? text.removeCharacters ("[]").getFloatValue() : p->getValueForText (text);
        p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, value));
        std::cout << "  " << name << " = " << p->getCurrentValueAsText() << std::endl;
        return true;
    }

    int render (juce::AudioPluginInstance& plugin, const juce::File& in, const juce::File& out)
    {
        juce::AudioBuffer<float> audio;
        double sr = 48000.0;
        if (! measure::readAudio (in, audio, sr))
        {
            std::cout << "Could not read " << in.getFullPathName() << std::endl;
            return 1;
        }

        constexpr int block = 512;
        plugin.setPlayConfigDetails (2, 2, sr, block);
        plugin.prepareToPlay (sr, block);

        juce::AudioBuffer<float> io (juce::jmax (2, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels()), block);
        juce::MidiBuffer midi;

        for (int start = 0; start < audio.getNumSamples(); start += block)
        {
            const int n = juce::jmin (block, audio.getNumSamples() - start);
            io.clear();
            for (int ch = 0; ch < 2; ++ch)
                io.copyFrom (ch, 0, audio, ch, start, n);

            juce::AudioBuffer<float> view (io.getArrayOfWritePointers(), io.getNumChannels(), n);
            plugin.processBlock (view, midi);

            for (int ch = 0; ch < 2; ++ch)
                audio.copyFrom (ch, start, io, ch, 0, n);
        }

        plugin.releaseResources();
        const bool ok = measure::writeWav (audio, sr, out);
        std::cout << (ok ? "Wrote " : "Could not write ") << out.getFullPathName() << " (latency " << plugin.getLatencySamples() << ")" << std::endl;
        return ok ? 0 : 1;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (argv[i]);

    const int pluginIndex = args.indexOf ("--plugin");
    if (pluginIndex < 0 || pluginIndex + 1 >= args.size())
    {
        std::cout << "Usage: PluginMeasure --plugin <file.vst3> [--list] [--set \"<name>=<text>\"]... [--render <in.wav> <out.wav>]" << std::endl;
        return 1;
    }

    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> types;
    format.findAllTypesForFile (types, args[pluginIndex + 1]);

    if (types.isEmpty())
    {
        std::cout << "No VST3 plugin found in " << args[pluginIndex + 1] << std::endl;
        return 1;
    }

    juce::String error;
    auto plugin = format.createInstanceFromDescription (*types[0], 48000.0, 512, error);
    if (plugin == nullptr)
    {
        std::cout << "Could not load the plugin: " << error << std::endl;
        return 1;
    }

    plugin->enableAllBuses();

    for (int i = 0; i + 1 < args.size(); ++i)
        if (args[i] == "--set" && ! set (*plugin, args[i + 1]))
            return 1;

    if (args.contains ("--list"))
        list (*plugin);

    const int renderIndex = args.indexOf ("--render");
    if (renderIndex >= 0 && renderIndex + 2 < args.size())
        return render (*plugin, juce::File (args[renderIndex + 1]), juce::File (args[renderIndex + 2]));

    return 0;
}
