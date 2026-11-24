#pragma once

#include "Track.h"

namespace PluginWindowLayout
{
    // Usable screen area for a window, leaving room for the native title bar and borders.
    inline juce::Rectangle<int> usableArea (juce::Rectangle<int> near)
    {
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* d = near.isEmpty() ? displays.getPrimaryDisplay() : displays.getDisplayForRect (near);
        if (d == nullptr)
            d = displays.getPrimaryDisplay();
        const auto area = d != nullptr ? d->userArea : juce::Rectangle<int> (0, 0, 1280, 720);
        return area.reduced (8).withTrimmedTop (32);
    }
}

// Holds a plug-in editor. If the editor is bigger than the screen, it sits in a scrollable
// view instead of spilling off-screen; it follows the editor when the plug-in resizes itself.
class PluginEditorHolder : public juce::Component, private juce::ComponentListener
{
public:
    explicit PluginEditorHolder (juce::AudioProcessorEditor* e) : editor (e)
    {
        viewport.setViewedComponent (editor.get(), false);
        viewport.setScrollBarsShown (true, true); // only appear when needed
        addAndMakeVisible (viewport);
        editor->addComponentListener (this);
        fitToEditor();
    }

    ~PluginEditorHolder() override
    {
        editor->removeComponentListener (this);
        viewport.setViewedComponent (nullptr, false);
        editor = nullptr; // delete the editor while the plug-in is still alive
    }

    bool editorIsResizable() const { return editor->isResizable(); }

    void fitToEditor()
    {
        const juce::ScopedValueSetter<bool> svs (updating, true);
        const auto area = PluginWindowLayout::usableArea (getScreenBounds());
        setSize (juce::jmin (editor->getWidth(), area.getWidth()), juce::jmin (editor->getHeight(), area.getHeight()));
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());

        // Resizable editors follow the window; fixed-size ones scroll if the window is smaller.
        if (! updating && editor->isResizable())
        {
            const juce::ScopedValueSetter<bool> svs (updating, true);
            editor->setSize (getWidth(), getHeight());
        }
    }

private:
    void componentMovedOrResized (juce::Component&, bool, bool wasResized) override
    {
        if (wasResized && ! updating)
            fitToEditor();
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::Viewport viewport;
    bool updating = false;
};

// A floating window showing one plug-in's editor (or a generic slider UI if it has none).
// It always keeps itself on screen, including when the plug-in resizes its editor later.
class PluginWindow : public juce::DocumentWindow, private juce::AsyncUpdater
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

        auto* holder = new PluginEditorHolder (editor);
        setContentOwned (holder, true);

        // Always allow resizing so oversized editors can be scrolled/shrunk; cap at the screen.
        setResizable (true, false);
        const auto area = PluginWindowLayout::usableArea ({});
        setResizeLimits (120, 80, area.getWidth(), area.getHeight());

        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        keepOnScreen();
    }

    ~PluginWindow() override
    {
        cancelPendingUpdate();
        clearContentComponent(); // deletes the editor before the window
    }

    void closeButtonPressed() override { closeCallback (this); }

    void resized() override
    {
        juce::DocumentWindow::resized();
        triggerAsyncUpdate(); // re-check position after the plug-in changes size
    }

    void keepOnScreen()
    {
        auto bounds = getBounds();
        if (auto* peer = getPeer())
            bounds = peer->getFrameSize().addedTo (bounds);

        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* d = displays.getDisplayForRect (bounds);
        if (d == nullptr)
            d = displays.getPrimaryDisplay();
        if (d == nullptr)
            return;

        const auto area = d->userArea;
        // Prefer keeping the top-left (title bar) visible if the window is taller/wider than the screen.
        const int x = juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - bounds.getWidth()), bounds.getX());
        const int y = juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - bounds.getHeight()), bounds.getY());

        if (x != bounds.getX() || y != bounds.getY())
            setTopLeftPosition (getX() + (x - bounds.getX()), getY() + (y - bounds.getY()));
    }

    juce::AudioPluginInstance& plugin;

private:
    void handleAsyncUpdate() override { keepOnScreen(); }

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
                w->keepOnScreen();
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
