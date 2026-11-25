#pragma once

#include <JuceHeader.h>

// Remembers the plug-in parameter you touched most recently (in any plug-in window,
// AMETHYST effects and VST3s alike), for the "Last tweaked parameter" menu.
// Written from whatever thread the plug-in reports changes on; read on the message thread.
namespace LastTweaked
{
    struct Entry
    {
        juce::AudioProcessor* processor = nullptr;   // validate against the mixer before use
        int index = -1;
    };

    inline juce::SpinLock& lock() { static juce::SpinLock l; return l; }
    inline Entry& entry() { static Entry e; return e; }

    inline void set (juce::AudioProcessor* p, int index)
    {
        const juce::SpinLock::ScopedLockType sl (lock());
        entry() = { p, index };
    }

    inline Entry get()
    {
        const juce::SpinLock::ScopedLockType sl (lock());
        return entry();
    }

    inline void forget (juce::AudioProcessor* p)
    {
        const juce::SpinLock::ScopedLockType sl (lock());
        if (entry().processor == p)
            entry() = {};
    }

    // Attach to a plug-in while its window is open.
    class Listener : public juce::AudioProcessorListener
    {
    public:
        void audioProcessorParameterChanged (juce::AudioProcessor* p, int index, float) override { set (p, index); }
        void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor* p, int index) override { set (p, index); }
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    };
}
