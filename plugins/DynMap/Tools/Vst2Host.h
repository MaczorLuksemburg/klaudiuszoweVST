// Minimal VST2 host for PluginMeasure (Windows only, like the tool itself). JUCE can host VST2 only with
// Steinberg's VST2 SDK, which is no longer distributed, so this declares just the parts of the VST 2.4
// binary interface it needs: load the DLL, open the effect, list/set parameters, get/set its state
// chunk and run processReplacing.
#pragma once

#include <juce_core/juce_core.h>
#include <cstdint>
#include <string>
#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace vst2
{
    struct Effect;
    using HostCallback = intptr_t (*) (Effect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
    using Dispatcher = intptr_t (*) (Effect*, int32_t opcode, int32_t index, intptr_t value, void* ptr, float opt);
    using SetParameter = void (*) (Effect*, int32_t index, float value);
    using GetParameter = float (*) (Effect*, int32_t index);
    using Process = void (*) (Effect*, float** inputs, float** outputs, int32_t frames);

    // The AEffect structure, field for field.
    struct Effect
    {
        int32_t magic;
        Dispatcher dispatcher;
        Process processDeprecated;
        SetParameter setParameter;
        GetParameter getParameter;
        int32_t numPrograms, numParams, numInputs, numOutputs, flags;
        intptr_t reserved1, reserved2;
        int32_t initialDelay, realQualities, offQualities;
        float ioRatio;
        void* object;
        void* user;
        int32_t uniqueId, version;
        Process processReplacing;
        void* processDoubleReplacing;
        char future[56];
    };

    enum Opcode : int32_t
    {
        effOpen = 0, effClose = 1, effSetProgram = 2, effGetParamLabel = 6, effGetParamDisplay = 7,
        effGetParamName = 8, effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12,
        effEditOpen = 14, effEditClose = 15, effEditIdle = 19, effGetChunk = 23, effSetChunk = 24, effGetEffectName = 45, effStartProcess = 71, effStopProcess = 72
    };

    enum HostOpcode : int32_t
    {
        audioMasterAutomate = 0, audioMasterVersion = 1, audioMasterCurrentId = 2, audioMasterIdle = 3,
        audioMasterGetTime = 7, audioMasterGetSampleRate = 16, audioMasterGetBlockSize = 17,
        audioMasterGetCurrentProcessLevel = 23, audioMasterGetVendorString = 32, audioMasterGetProductString = 33,
        audioMasterGetVendorVersion = 34, audioMasterCanDo = 37
    };

    constexpr int32_t effFlagsProgramChunks = 1 << 5;

    class Plugin
    {
    public:
        ~Plugin()
        {
            if (effect != nullptr)
            {
               #if JUCE_WINDOWS
                if (editorWindow != nullptr)
                {
                    dispatch (effEditClose);
                    DestroyWindow (editorWindow);
                }
               #endif
                dispatch (effMainsChanged, 0, 0);
                dispatch (effClose);
            }
           #if JUCE_WINDOWS
            if (module != nullptr)
                FreeLibrary (module);
           #endif
        }

        bool load (const juce::File& file, juce::String& error)
        {
           #if JUCE_WINDOWS
            module = LoadLibraryW (file.getFullPathName().toWideCharPointer());
            if (module == nullptr)
            {
                error = "LoadLibrary failed";
                return false;
            }

            using Main = Effect* (*) (HostCallback);
            auto main = reinterpret_cast<Main> (GetProcAddress (module, "VSTPluginMain"));
            if (main == nullptr)
                main = reinterpret_cast<Main> (GetProcAddress (module, "main"));
            if (main == nullptr)
            {
                error = "no VSTPluginMain";
                return false;
            }

            effect = main (&hostCallback);
            if (effect == nullptr || effect->magic != 0x56737450) // 'VstP'
            {
                error = "the plugin did not return an effect";
                effect = nullptr;
                return false;
            }

            dispatch (effOpen);
            return true;
           #else
            juce::ignoreUnused (file);
            error = "VST2 hosting is Windows only";
            return false;
           #endif
        }

        juce::String getName()
        {
            char name[256] {};
            dispatch (effGetEffectName, 0, 0, name);
            return name;
        }

        int numParams() const { return effect->numParams; }
        int numInputs() const { return effect->numInputs; }
        int numOutputs() const { return effect->numOutputs; }
        int latency() const { return effect->initialDelay; }
        bool usesChunks() const { return (effect->flags & effFlagsProgramChunks) != 0; }

        juce::String paramName (int i) { return paramString (effGetParamName, i); }
        juce::String paramText (int i) { return (paramString (effGetParamDisplay, i) + " " + paramString (effGetParamLabel, i)).trim(); }
        float getParam (int i) { return effect->getParameter (effect, i); }
        void setParam (int i, float v) { effect->setParameter (effect, i, v); }

        // isProgram: the current program's chunk instead of the whole bank (VST2's two chunk kinds).
        juce::MemoryBlock getChunk (bool isProgram = false)
        {
            void* data = nullptr;
            const auto size = dispatch (effGetChunk, isProgram ? 1 : 0, 0, &data);
            return data != nullptr && size > 0 ? juce::MemoryBlock (data, (size_t) size) : juce::MemoryBlock();
        }

        intptr_t setChunk (const juce::MemoryBlock& chunk, bool isProgram = false)
        {
            return dispatch (effSetChunk, isProgram ? 1 : 0, (intptr_t) chunk.getSize(), const_cast<void*> (chunk.getData()));
        }

        // Opens the plugin's editor in a hidden window: some plugins only fill in parameter text (or finish
        // initialising) once their GUI exists.
        void openHiddenEditor()
        {
           #if JUCE_WINDOWS
            if (editorWindow == nullptr)
            {
                editorWindow = CreateWindowExW (0, L"STATIC", L"PluginMeasure", WS_POPUP, 0, 0, 800, 600, nullptr, nullptr, nullptr, nullptr);
                dispatch (effEditOpen, 0, 0, editorWindow);
                pumpMessages (300);
            }
           #endif
        }

        // Runs this thread's window messages for a while: some plugins apply a loaded state on their
        // message queue rather than inside the call.
        void pumpMessages (int milliseconds)
        {
           #if JUCE_WINDOWS
            const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) milliseconds;
            while (juce::Time::getMillisecondCounter() < end)
            {
                MSG message;
                while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage (&message);
                    DispatchMessageW (&message);
                }
                dispatch (effEditIdle);
                juce::Thread::sleep (5);
            }
           #else
            juce::ignoreUnused (milliseconds);
           #endif
        }

        void prepare (double sampleRate, int blockSize)
        {
            currentSampleRate = sampleRate;
            currentBlockSize = blockSize;
            dispatch (effSetSampleRate, 0, 0, nullptr, (float) sampleRate);
            dispatch (effSetBlockSize, 0, blockSize);
            dispatch (effMainsChanged, 0, 1);
            dispatch (effStartProcess);
        }

        void stop()
        {
            dispatch (effStopProcess);
            dispatch (effMainsChanged, 0, 0);
        }

        void process (float** inputs, float** outputs, int frames) { effect->processReplacing (effect, inputs, outputs, frames); }

    private:
        intptr_t dispatch (int32_t opcode, int32_t index = 0, intptr_t value = 0, void* ptr = nullptr, float opt = 0.0f)
        {
            return effect->dispatcher (effect, opcode, index, value, ptr, opt);
        }

        juce::String paramString (int32_t opcode, int i)
        {
            char text[256] {};
            dispatch (opcode, i, 0, text);
            return juce::String (text).trim();
        }

        static intptr_t hostCallback (Effect*, int32_t opcode, int32_t, intptr_t, void* ptr, float)
        {
            switch (opcode)
            {
                case audioMasterVersion:                return 2400;
                case audioMasterGetSampleRate:          return (intptr_t) currentSampleRate;
                case audioMasterGetBlockSize:           return currentBlockSize;
                case audioMasterGetCurrentProcessLevel: return 4; // offline
                case audioMasterGetVendorString:        return copyString (ptr, "Maki plugins");
                case audioMasterGetProductString:       return copyString (ptr, "PluginMeasure");
                case audioMasterGetVendorVersion:       return 1000;
                case audioMasterCanDo:
                {
                    const juce::String what (ptr != nullptr ? static_cast<const char*> (ptr) : "");
                    return what == "sendVstEvents" || what == "sizeWindow" ? 0 : (what == "offline" ? 1 : 0);
                }
                default:                                return 0;
            }
        }

        static intptr_t copyString (void* ptr, const char* text)
        {
            if (ptr == nullptr)
                return 0;
            juce::String (text).copyToUTF8 (static_cast<char*> (ptr), 64);
            return 1;
        }

        inline static double currentSampleRate = 48000.0;
        inline static int currentBlockSize = 512;

       #if JUCE_WINDOWS
        HMODULE module = nullptr;
        HWND editorWindow = nullptr;
       #endif
        Effect* effect = nullptr;
    };
}
