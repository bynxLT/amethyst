// Runs a third-party plug-in's processBlock so that a crash inside it (access violation, divide by zero...)
// doesn't take AMETHYST down. Windows structured exception handling; elsewhere it simply calls through.
// Kept in its own file with no C++ objects in the guarded functions (required for __try).
#include <JuceHeader.h>

namespace
{
    void callProcess (juce::AudioPluginInstance* p, juce::AudioBuffer<float>* b, juce::MidiBuffer* m)
    {
        p->processBlock (*b, *m);
    }
}

bool amethystGuardedProcess (juce::AudioPluginInstance* plugin, juce::AudioBuffer<float>* buffer, juce::MidiBuffer* midi)
{
   #if JUCE_WINDOWS && defined (_MSC_VER)
    __try
    {
        callProcess (plugin, buffer, midi);
        return true;
    }
    __except (1 /* EXCEPTION_EXECUTE_HANDLER */)
    {
        return false;
    }
   #else
    callProcess (plugin, buffer, midi);
    return true;
   #endif
}
