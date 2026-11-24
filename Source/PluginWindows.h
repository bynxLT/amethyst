#pragma once

#include "Track.h"

// A floating window showing one plug-in's editor (or a generic slider UI if it has none).
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance& p, std::function<void (PluginWindow*)> onClose)
        : juce::DocumentWindow (p.getName(), juce::Colour (0xff120a1f),
                                juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          plugin (p),
          closeCallback (std::move (onClose))
    {
        setUsingNativeTitleBar (true);

        juce::AudioProcessorEditor* editor = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (p);

        setContentOwned (editor, true);
        setResizable (editor->isResizable(), false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    ~PluginWindow() override
    {
        clearContentComponent(); // deletes the editor before the window
    }

    void closeButtonPressed() override { closeCallback (this); }

    juce::AudioPluginInstance& plugin;

private:
    std::function<void (PluginWindow*)> closeCallback;
    JUCE_DECLARE_NON_COPYABLE (PluginWindow)
};

class PluginWindowManager
{
public:
    void show (juce::AudioPluginInstance& p)
    {
        for (auto* w : windows)
        {
            if (&w->plugin == &p)
            {
                w->setMinimised (false);
                w->toFront (true);
                return;
            }
        }

        windows.add (new PluginWindow (p, [this] (PluginWindow* w) { windows.removeObject (w); }));
    }

    void close (juce::AudioPluginInstance& p)
    {
        for (int i = windows.size(); --i >= 0;)
            if (&windows[i]->plugin == &p)
                windows.remove (i);
    }

    void closeAllFor (const Track& t)
    {
        for (auto& slot : t.inserts)
            if (slot->plugin != nullptr)
                close (*slot->plugin);
    }

    void closeAll() { windows.clear(); }

private:
    juce::OwnedArray<PluginWindow> windows;
};
