#pragma once

#include "AudioEngine.h"
#include "Widgets.h"

std::size_t vocaldawProcessMemoryMB();

// Big tempo readout. Drag up/down (Shift = fine), mouse wheel, or double-click to type.
class TempoDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit TempoDisplay (AudioEngine& e) : engine (e)
    {
        setTooltip ("Tempo: drag up/down (Shift for fine), scroll, double-click to type, right-click for tap tempo and presets");
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        Glass::well (g, r, 9.0f);
        if (tapping)
        {
            g.setColour (Theme::accent2.withAlpha (0.18f + 0.25f * tapFlash));
            g.fillRoundedRectangle (r.reduced (1.0f), 8.0f);
        }
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 20.0f, juce::Font::bold));
        g.drawText (juce::String (engine.bpm.load(), 2), r.withTrimmedRight (30).toNearestInt(), juce::Justification::centredRight);
        g.setColour (tapping ? Theme::accent2 : Theme::accent);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText (tapping ? "TAP" : "BPM", r.removeFromRight (30).toNearestInt(), juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            showMenu();
            return;
        }
        if (tapping)
        {
            tap();
            return;
        }
        dragStartBpm = engine.bpm.load();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (tapping || e.mods.isPopupMenu())
            return;
        const double perPixel = e.mods.isShiftDown() ? 0.02 : 0.5;
        setBpm (dragStartBpm - e.getDistanceFromDragStartY() * perPixel);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        setBpm (engine.bpm.load() + (w.deltaY > 0 ? 1.0 : -1.0) * (e.mods.isShiftDown() ? 0.1 : 1.0));
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (! tapping)
            showTypeIn();
    }

    void showTypeIn()
    {
        entry = std::make_unique<juce::AlertWindow> ("Tempo", "Enter a tempo (40 - 300 BPM)", juce::MessageBoxIconType::NoIcon);
        entry->addTextEditor ("bpm", juce::String (engine.bpm.load(), 2));
        entry->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        entry->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<TempoDisplay> safe (this);
        entry->enterModalState (true, juce::ModalCallbackFunction::create ([safe] (int result)
        {
            if (safe == nullptr || safe->entry == nullptr)
                return;
            if (result == 1)
                safe->setBpm (safe->entry->getTextEditorContents ("bpm").getDoubleValue());
            safe->entry.reset();
        }), false);
    }

private:
    void setBpm (double v)
    {
        engine.bpm = juce::jlimit (40.0, 300.0, std::round (v * 100.0) / 100.0);
        repaint();
    }

    void showMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "Tap tempo (click here on each beat)", true, tapping);
        m.addSeparator();
        for (int bpm = 80; bpm <= 160; bpm += 10)
            m.addItem (bpm, juce::String (bpm) + " BPM", true, std::abs (engine.bpm.load() - bpm) < 0.01);
        m.addSeparator();
        m.addItem (2, "Half tempo");
        m.addItem (3, "Double tempo");
        m.addItem (4, "Round to whole BPM");
        m.addItem (5, "Type in value...");

        juce::Component::SafePointer<TempoDisplay> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int r)
        {
            if (safe == nullptr || r == 0)
                return;
            auto& self = *safe;
            const double now = self.engine.bpm.load();
            if (r >= 80)      self.setBpm (r);
            else if (r == 1)  self.setTapping (! self.tapping);
            else if (r == 2)  self.setBpm (now * 0.5);
            else if (r == 3)  self.setBpm (now * 2.0);
            else if (r == 4)  self.setBpm (std::round (now));
            else if (r == 5)  self.showTypeIn();
        });
    }

    void setTapping (bool shouldTap)
    {
        tapping = shouldTap;
        taps.clear();
        setMouseCursor (tapping ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::UpDownResizeCursor);
        if (tapping) startTimerHz (30); else stopTimer();
        repaint();
    }

    void tap()
    {
        const double t = juce::Time::getMillisecondCounterHiRes();
        if (! taps.empty() && t - taps.back() > 2500.0)
            taps.clear();               // a long pause starts a new count
        taps.push_back (t);
        if (taps.size() > 8)
            taps.erase (taps.begin());
        if (taps.size() >= 2)
            setBpm (60000.0 * (double) (taps.size() - 1) / (taps.back() - taps.front()));
        tapFlash = 1.0f;
    }

    struct Ticker : juce::Timer
    {
        std::function<void()> fn;
        void timerCallback() override { fn(); }
    } ticker;

    void startTimerHz (int hz) { ticker.fn = [this] { onTick(); }; ticker.startTimerHz (hz); }
    void stopTimer()           { ticker.stopTimer(); }

    void onTick()
    {
        tapFlash *= 0.85f;
        // leave tap mode after 5 s without a tap
        if (tapping && ! taps.empty() && juce::Time::getMillisecondCounterHiRes() - taps.back() > 5000.0)
            setTapping (false);
        repaint();
    }

    AudioEngine& engine;
    double dragStartBpm = 120.0;
    std::unique_ptr<juce::AlertWindow> entry;
    bool tapping = false;
    std::vector<double> taps;
    float tapFlash = 0.0f;
};

// Song position. Click to switch between bars:beats and minutes:seconds.
class TimeDisplay : public juce::Component
{
public:
    explicit TimeDisplay (AudioEngine& e) : engine (e) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        Glass::well (g, r, 9.0f);

        const double sr = juce::jmax (1.0, engine.getSampleRate());
        const double secs = (double) engine.getTransportPosition() / sr;
        juce::String main, unit;

        if (showBars)
        {
            const double beats = secs * engine.bpm.load() / 60.0;
            const int bar = (int) (beats / 4.0) + 1;
            const int beat = (int) std::fmod (beats, 4.0) + 1;
            const int ticks = (int) (std::fmod (beats, 1.0) * 96.0);
            main = juce::String (bar) + ":" + juce::String (beat) + ":" + juce::String (ticks).paddedLeft ('0', 2);
            unit = "BAR:BEAT:TICK";
        }
        else
        {
            const int mins = (int) (secs / 60.0);
            const int s = (int) std::fmod (secs, 60.0);
            const int ms = (int) (std::fmod (secs, 1.0) * 1000.0);
            main = juce::String (mins) + ":" + juce::String (s).paddedLeft ('0', 2) + "." + juce::String (ms).paddedLeft ('0', 3);
            unit = "MIN:SEC.MS";
        }

        auto textArea = r.reduced (12.0f, 2.0f);
        g.setColour (engine.isRecording() ? Theme::record : Theme::text);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 24.0f, juce::Font::bold));
        g.drawText (main, textArea.withTrimmedRight (54).toNearestInt(), juce::Justification::centredLeft);

        g.setColour (Theme::accent);
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawFittedText (unit, textArea.removeFromRight (54).toNearestInt(), juce::Justification::centredRight, 2);
    }

    void mouseDown (const juce::MouseEvent&) override { showBars = ! showBars; repaint(); }

private:
    AudioEngine& engine;
    bool showBars = true;
};

// CPU load graph + memory readout.
class SystemMeter : public juce::Component
{
public:
    explicit SystemMeter (AudioEngine& e) : engine (e) { history.resize (40, 0.0f); }

    void tick()
    {
        if (++frame % 6 != 0)  // ~5 updates a second
            return;
        history.erase (history.begin());
        history.push_back ((float) juce::jlimit (0.0, 1.0, engine.getCpuUsage()));
        memoryMB = vocaldawProcessMemoryMB();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        Glass::well (g, r, 9.0f);
        auto inner = r.reduced (8.0f, 6.0f);

        auto text = inner.removeFromRight (62.0f);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("CPU " + juce::String (juce::roundToInt (history.back() * 100.0f)) + "%",
                    text.removeFromTop (text.getHeight() * 0.5f).toNearestInt(), juce::Justification::centredRight);
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String ((int) memoryMB) + " MB", text.toNearestInt(), juce::Justification::centredRight);

        inner.removeFromRight (6.0f);
        const float bw = inner.getWidth() / (float) history.size();
        for (size_t i = 0; i < history.size(); ++i)
        {
            const float h = juce::jmax (1.0f, inner.getHeight() * history[i]);
            auto bar = juce::Rectangle<float> (inner.getX() + bw * (float) i, inner.getBottom() - h, juce::jmax (1.0f, bw - 1.0f), h);
            g.setColour ((history[i] > 0.8f ? Theme::record : Theme::accent).withAlpha (0.4f + 0.6f * (float) i / (float) history.size()));
            g.fillRect (bar);
        }
    }

private:
    AudioEngine& engine;
    std::vector<float> history;
    std::size_t memoryMB = 0;
    int frame = 0;
};
