#include "TrackComponent.h"
#include "Plugins/AmethystPlugin.h"
#include "PluginWindows.h"

namespace
{
    // Musical grid: every other bar shaded, bar lines, beat lines and (when zoomed in) the
    // magnet's snap subdivisions. Labels = ruler mode (bar numbers + beat ticks).
    void drawBarGrid (juce::Graphics& g, const TimelineView& view, juce::Rectangle<int> lane, double bpm, bool labels)
    {
        const double beatSec = 60.0 / juce::jmax (20.0, bpm);
        const double barSec = beatSec * 4.0;
        const double pps = view.pixelsPerSecond;
        const float top = (float) lane.getY(), bottom = (float) lane.getBottom();
        const double endSec = view.startSeconds + lane.getWidth() / pps;

        // alternate bar shading
        if (! labels)
        {
            for (int bar = (int) std::floor (view.startSeconds / barSec); bar * barSec < endSec; ++bar)
            {
                if ((bar & 1) == 0)
                    continue;
                const float x0 = juce::jmax ((float) lane.getX(), view.timeToX (bar * barSec, lane));
                const float x1 = juce::jmin ((float) lane.getRight(), view.timeToX ((bar + 1) * barSec, lane));
                if (x1 > x0)
                {
                    g.setColour (juce::Colours::black.withAlpha (0.14f));
                    g.fillRect (juce::Rectangle<float> (x0, top, x1 - x0, bottom - top));
                }
            }
        }

        // snap subdivisions (only where they're far enough apart to be useful)
        const double snapSec = view.snapBeats() * beatSec;
        if (! labels && snapSec > 0.0 && snapSec < beatSec && snapSec * pps >= 7.0)
        {
            g.setColour (juce::Colours::white.withAlpha (0.035f));
            for (auto i = (juce::int64) std::floor (view.startSeconds / snapSec); i * snapSec < endSec; ++i)
                g.drawVerticalLine ((int) view.timeToX (i * snapSec, lane), top, bottom);
        }

        // 16th-step ticks along the bottom of the ruler
        const double stepSec = beatSec * 0.25;
        if (labels && stepSec * pps >= 7.0)
        {
            g.setColour (Theme::text.withAlpha (0.18f));
            for (auto i = (juce::int64) std::floor (view.startSeconds / stepSec); i * stepSec < endSec; ++i)
                if (i % 4 != 0)
                    g.drawVerticalLine ((int) view.timeToX (i * stepSec, lane), bottom - 3.0f, bottom);
        }

        // beat lines
        if (beatSec * pps >= 9.0)
        {
            for (auto i = (juce::int64) std::floor (view.startSeconds / beatSec); i * beatSec < endSec; ++i)
            {
                if (i % 4 == 0)
                    continue;
                const float x = view.timeToX (i * beatSec, lane);
                if (labels)
                {
                    g.setColour (Theme::text.withAlpha (0.3f));
                    g.drawVerticalLine ((int) x, bottom - 6.0f, bottom);
                }
                else
                {
                    g.setColour (juce::Colours::white.withAlpha (0.06f));
                    g.drawVerticalLine ((int) x, top, bottom);
                }
            }
        }

        // bar lines (+ numbers on the ruler, thinned out when zoomed far out)
        int step = 1;
        while (barSec * step * pps < (labels ? 34.0 : 6.0))
            step *= 2;
        int bar = (int) std::floor (view.startSeconds / barSec);
        bar -= bar % step;
        for (;; bar += step)
        {
            const float x = view.timeToX (bar * barSec, lane);
            if (x > (float) lane.getRight())
                break;
            if (x < (float) lane.getX())
                continue;

            g.setColour (labels ? Theme::text.withAlpha (0.45f) : juce::Colours::white.withAlpha (0.12f));
            g.drawVerticalLine ((int) x, labels ? bottom - 12.0f : top, bottom);

            if (labels)
            {
                g.setColour (Theme::text.withAlpha (0.85f));
                g.drawText (juce::String (bar + 1), (int) x + 3, lane.getY(), 40, lane.getHeight() - 6,
                            juce::Justification::centredLeft);
            }
        }
    }
}

//==============================================================================
TrackComponent::TrackComponent (AppContext& c, Track& t) : ctx (c), track (t)
{
    nameLabel.setText (track.name, juce::dontSendNotification);
    nameLabel.setEditable (false, true);
    nameLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    nameLabel.setMinimumHorizontalScale (0.8f);
    nameLabel.setTooltip ("Double-click to rename");
    nameLabel.onTextChange = [this]
    {
        track.name = nameLabel.getText().trim().isEmpty() ? "Track" : nameLabel.getText().trim();
    };
    addAndMakeVisible (nameLabel);

    onLed.setTooltip ("Track on / off (mute)");
    onLed.onClick = [this] { track.mute = ! onLed.getToggleState(); };
    armLed.setTooltip ("Arm for recording");
    armLed.onClick = [this] { track.armed = armLed.getToggleState(); };
    addAndMakeVisible (onLed);
    addAndMakeVisible (armLed);

    menuBtn.setTooltip ("Input, monitoring, solo, plug-ins, presets, delete");
    menuBtn.onClick = [this] { showTrackMenu(); };
    addAndMakeVisible (menuBtn);

    inputBox.onChange = [this] { UiHelpers::applyInputSelection (inputBox, track); };
    addChildComponent (inputBox); // never shown

    if (track.isAutomation())
    {
        armLed.setVisible (false);
        onLed.setTooltip ("Automation on / off");
        menuBtn.setTooltip ("Automation clip options");
        nameLabel.setTooltip ("Automation clip - click the lane to add points, drag to move, right-click a point to delete");
    }

    menuBtn.setVisible (false);
    disableFocusOnChildren (*this);
    refreshInputs();
    tick();
}

void TrackComponent::refreshInputs()
{
    UiHelpers::fillInputBox (inputBox, ctx.engine, track);
}

void TrackComponent::syncThumbnails()
{
    // drop thumbnails for clips that no longer exist
    for (auto it = thumbs.begin(); it != thumbs.end();)
    {
        const bool alive = std::any_of (track.clips.begin(), track.clips.end(),
                                        [&] (const auto& c) { return c.get() == it->first; });
        it = alive ? std::next (it) : thumbs.erase (it);
    }

    // (re)build from the rendered buffer, so clip effects show in the waveform
    for (auto& c : track.clips)
    {
        auto& th = thumbs[c.get()];
        const auto* buf = c->buffer.get();
        if (th.thumbnail != nullptr && th.buffer == buf)
            continue;

        th.buffer = buf;
        if (th.thumbnail == nullptr)
            th.thumbnail = std::make_unique<juce::AudioThumbnail> (512, ctx.engine.audioFormats, ctx.thumbCache);

        if (buf != nullptr && buf->getNumSamples() > 0)
        {
            th.thumbnail->reset (buf->getNumChannels(), ctx.engine.getSampleRate(), buf->getNumSamples());
            th.thumbnail->addBlock (0, *buf, 0, buf->getNumSamples());
        }
        else
        {
            th.thumbnail->clear();
        }
    }
}

void TrackComponent::tick()
{
    syncThumbnails();
    // keep rows calm: the "..." button shows while the mouse is over the row
    const bool h = isMouseOver (true);
    if (h != hovered) { hovered = h; menuBtn.setVisible (h); }

    levelL = juce::jmax (track.meterL.exchange (0.0f), levelL * 0.85f);
    levelR = juce::jmax (track.meterR.exchange (0.0f), levelR * 0.85f);

    // keep the header in sync with the mixer / presets
    if (! nameLabel.isBeingEdited() && nameLabel.getText() != track.name)
        nameLabel.setText (track.name, juce::dontSendNotification);
    onLed.setToggleState (! track.mute.load(), juce::dontSendNotification);
    armLed.setToggleState (track.armed.load(), juce::dontSendNotification);
    nameLabel.setColour (juce::Label::textColourId, track.mute.load() ? Theme::textDim.withAlpha (0.45f) : Theme::text);

    repaint();
}

void TrackComponent::showTrackMenu()
{
    if (track.isAutomation())
    {
        showAutomationMenu();
        return;
    }

    juce::PopupMenu m;
    m.addSectionHeader (track.name);
    m.addItem (1, "Arm for recording", true, track.armed.load());
    m.addItem (2, "Input monitoring (hear yourself)", true, track.monitor.load());
    m.addItem (3, "Solo", true, track.solo.load());
    m.addItem (4, "Mute", true, track.mute.load());

    juce::PopupMenu inputs;
    for (int i = 0; i < inputBox.getNumItems(); ++i)
        inputs.addItem (1000 + inputBox.getItemId (i), inputBox.getItemText (i), true, inputBox.getSelectedId() == inputBox.getItemId (i));
    if (inputBox.getNumItems() == 0)
        inputs.addItem (999, "No inputs enabled - open Audio settings", false, false);
    m.addSubMenu ("Recording input", inputs);

    juce::PopupMenu gainMenu;
    const float currentGain = track.inputGainDb.load();
    for (int db : { -6, 0, 6, 12, 18, 24 })
        gainMenu.addItem (4000 + db + 100, (db > 0 ? "+" : "") + juce::String (db) + " dB", true, std::abs (currentGain - (float) db) < 0.05f);
    m.addSubMenu ("Input gain (mic boost)" + (std::abs (currentGain) > 0.05f ? "  " + juce::String (currentGain, 1) + " dB" : juce::String()), gainMenu);

    juce::PopupMenu through;
    through.addItem (2000, "Its own mixer channel", true, track.playThrough == nullptr);
    through.addSeparator();
    {
        int i = 0;
        for (auto* other : ctx.engine.getTracks())
        {
            if (other != &track && other->playThrough == nullptr && ! other->isAutomation())
                through.addItem (2001 + i, other->name, true, track.playThrough == other);
            ++i;
        }
    }
    m.addSubMenu ("Play through channel", through);

    m.addSeparator();
    const int nFx = track.countPlugins();
    m.addItem (5, "Plug-ins in mixer..." + (nFx > 0 ? "  (" + juce::String (nFx) + ")" : juce::String()));
    m.addItem (6, "Channel presets...");
    juce::PopupMenu takes;
    takes.addItem (8, "Layer - hear every take", true, track.layerTakes.load());
    takes.addItem (9, "Newest take on top", true, ! track.layerTakes.load());
    takes.addSeparator();
    takes.addItem (10, "Move new overlapping takes to the next free track", true, track.takesToFreeTrack);
    takes.addItem (11, "Keep new takes on this track", true, ! track.takesToFreeTrack);
    m.addSubMenu ("Takes", takes);
    {
        juce::PopupMenu automate;
        automate.addItem (3000, "Volume");
        automate.addItem (3001, "Pan");
        for (int slot = 0; slot < (int) track.inserts.size(); ++slot)
        {
            auto* plugin = track.inserts[(size_t) slot]->plugin.get();
            if (plugin == nullptr)
                continue;
            juce::PopupMenu params;
            const auto& list = plugin->getParameters();
            for (int p = 0; p < juce::jmin (list.size(), 1999); ++p)
                if (list[p]->isAutomatable())
                    params.addItem (10000 + slot * 2000 + p, list[p]->getName (40));
            automate.addSubMenu ("Slot " + juce::String (slot + 1) + ": " + plugin->getName(), params, params.getNumItems() > 0);
        }
        m.addSubMenu ("Create automation clip", automate);
    }
    // Channel strip extras and AMETHYST 2026 tools (items carry their own actions; ids 500+ are ignored below)
    {
        auto* tp = &track;
        auto refresh = [safe = juce::Component::SafePointer<TrackComponent> (this)] { if (safe != nullptr) { safe->tick(); safe->repaint(); } };
        juce::PopupMenu colours;
        auto addColour = [&] (int id, const juce::String& name, juce::Colour col)
        {
            juce::PopupMenu::Item it (name);
            it.setID (id).setColour (col.isTransparent() ? Theme::text : col).setTicked (tp->colour == col).setAction ([tp, col, refresh] { tp->colour = col; refresh(); });
            colours.addItem (it);
        };
        addColour (500, "Default", juce::Colour());
        static const std::pair<const char*, juce::uint32> kTrackColours[] = {
            { "Violet", 0xffa070ff }, { "Pink", 0xffff6fc8 }, { "Red", 0xffff5a6e }, { "Orange", 0xffffa04a }, { "Gold", 0xffffd34d },
            { "Green", 0xff5fe08a }, { "Teal", 0xff3fd6c8 }, { "Blue", 0xff4fa8ff }, { "Indigo", 0xff6c6cff }, { "Grey", 0xffa8a4b8 } };
        for (int i = 0; i < 10; ++i)
            addColour (501 + i, kTrackColours[i].first, juce::Colour (kTrackColours[i].second));
        m.addSubMenu ("Track colour", colours);

        juce::PopupMenu lowCut;
        for (int i = 0; i < 7; ++i)
        {
            static const float kCuts[] = { 0.0f, 40.0f, 60.0f, 80.0f, 100.0f, 120.0f, 150.0f };
            const float hz = kCuts[i];
            juce::PopupMenu::Item it (hz <= 0.0f ? juce::String ("Off") : juce::String ((int) hz) + " Hz");
            it.setID (520 + i).setTicked (std::abs (tp->lowCutHz.load() - hz) < 0.5f).setAction ([tp, hz, refresh] { tp->lowCutHz = hz; refresh(); });
            lowCut.addItem (it);
        }
        m.addSubMenu ("Low cut (rumble filter)" + (tp->lowCutHz.load() > 1.0f ? "  " + juce::String ((int) tp->lowCutHz.load()) + " Hz" : juce::String()), lowCut);
        {
            juce::PopupMenu::Item it ("Invert polarity");
            it.setID (530).setTicked (tp->phaseInvert.load()).setAction ([tp, refresh] { tp->phaseInvert = ! tp->phaseInvert.load(); refresh(); });
            m.addItem (it);
        }

        if (ctx.trackAction)
        {
            const int tier = ctx.tier ? ctx.tier() : 0;
            auto fn = ctx.trackAction;
            auto add = [&] (int id, const juce::String& text, int action, int needTier)
            {
                juce::PopupMenu::Item it (text + (tier >= needTier ? juce::String() : needTier >= 2 ? juce::String ("   (Ultra Pro)") : juce::String ("   (Pro)")));
                it.setID (id).setAction ([fn, tp, action] { fn (*tp, action); });
                m.addItem (it);
            };
            m.addSectionHeader ("Tools");
            add (540, "Smart Vocal Chain (analyse and build effects)", 2, 1);
            add (541, "Bounce in place (render effects to audio)", 1, 1);
            add (542, "Capture last performance  (Shift+R)", 4, 1);
            add (543, "Set input level (sing for 5 seconds)", 3, 0);
        }
    }

    m.addSeparator();
    m.addItem (7, "Delete track");

    juce::Component::SafePointer<TrackComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuBtn), [safe] (int r)
    {
        if (safe == nullptr || r == 0)
            return;
        auto& t = safe->track;
        if (r >= 4000 && r < 4300)
        {
            t.inputGainDb = (float) (r - 4100);
            safe->tick();
            return;
        }
        if (r >= 3000 && r < 3002 && safe->ctx.createAutomation)
        {
            safe->ctx.createAutomation (t, r == 3000 ? Automation::Kind::Volume : Automation::Kind::Pan, -1, -1);
            return;
        }
        if (r >= 10000 && safe->ctx.createAutomation)
        {
            safe->ctx.createAutomation (t, Automation::Kind::Param, (r - 10000) / 2000, (r - 10000) % 2000);
            return;
        }
        if (r >= 2000)
        {
            safe->ctx.engine.setPlayThrough (t, r == 2000 ? nullptr : safe->ctx.engine.getTracks()[r - 2001]);
            if (safe->ctx.showInMixer == nullptr) return;
            safe->tick();
            return;
        }
        if (r >= 1000)
        {
            safe->inputBox.setSelectedId (r - 1000, juce::sendNotificationSync);
            return;
        }
        switch (r)
        {
            case 1: t.armed = ! t.armed.load(); break;
            case 2: t.monitor = ! t.monitor.load(); break;
            case 3: t.solo = ! t.solo.load(); break;
            case 4: t.mute = ! t.mute.load(); break;
            case 5: if (safe->ctx.showInMixer) safe->ctx.showInMixer (&t); break;
            case 6: UiHelpers::showPresetMenu (safe->ctx, t, safe->menuBtn, {}); break;
            case 7: if (safe->ctx.removeTrack) safe->ctx.removeTrack (&t); break;
            case 8: t.layerTakes = true; break;
            case 9: t.layerTakes = false; break;
            case 10: t.takesToFreeTrack = true; break;
            case 11: t.takesToFreeTrack = false; break;
            default: break;
        }
        safe->tick();
    });
}

//==============================================================================
juce::Rectangle<float> TrackComponent::clipBounds (const AudioClip& clip) const
{
    const auto lane = laneBounds();
    const double sr = juce::jmax (1.0, ctx.engine.getSampleRate());
    const double startSec = (double) clip.startSample.load() / sr;
    const double lenSec = (double) clip.getLength() / sr;
    const float x0 = ctx.view.timeToX (startSec, lane);
    const float x1 = ctx.view.timeToX (startSec + lenSec, lane);
    return { x0, (float) lane.getY() + 4.0f, x1 - x0, (float) lane.getHeight() - 8.0f };
}

std::shared_ptr<AudioClip> TrackComponent::clipAt (juce::Point<int> p) const
{
    for (auto it = track.clips.rbegin(); it != track.clips.rend(); ++it)
        if (clipBounds (**it).contains (p.toFloat()))
            return *it;
    return nullptr;
}

void TrackComponent::paint (juce::Graphics& g)
{
    if (track.isAutomation())
    {
        paintAutomation (g);
        return;
    }

    // Compact list row
    auto header = getLocalBounds().withWidth (kHeaderWidth).toFloat().reduced (3.0f, 2.0f);
    Glass::panel (g, header, 8.0f, track.armed ? 1.4f : 1.0f);
    const bool coloured = ! track.colour.isTransparent();
    g.setGradientFill (juce::ColourGradient (track.armed ? Theme::record : coloured ? track.colour : Theme::accent, header.getX(), header.getY(),
                                             track.armed ? Theme::record.darker() : coloured ? track.colour.darker (0.4f) : Theme::accent2, header.getX(), header.getBottom(), false));
    g.fillRoundedRectangle (header.withWidth (coloured ? 5.0f : 3.0f).reduced (0.0f, 8.0f), 2.0f);
    if (track.clipped.load())
    {
        // red clip dot until clicked in the mixer
        g.setColour (Theme::record);
        g.fillEllipse (header.getRight() - 24.0f, header.getY() + 5.0f, 6.0f, 6.0f);
    }

    // status chips under the name: input, monitoring, solo, plug-ins
    {
        juce::StringArray chips;
        const juce::String arrow (juce::CharPointer_UTF8 ("\xe2\x86\x92 "));
        if (track.playThrough != nullptr)
            chips.add (arrow + track.playThrough->name.substring (0, 12));
        if (inputBox.getNumItems() > 0)
            chips.add (inputBox.getSelectedId() >= 1001 ? "ST " + juce::String (track.inputChannel.load() + 1)
                                                        : "IN " + juce::String (track.inputChannel.load() + 1));
        if (std::abs (track.inputGainDb.load()) > 0.05f)
            chips.add ((track.inputGainDb.load() > 0 ? "+" : "") + juce::String (track.inputGainDb.load(), 0) + "dB");
        if (track.lowCutHz.load() > 1.0f) chips.add ("LC" + juce::String ((int) track.lowCutHz.load()));
        if (track.monitor.load()) chips.add ("MON");
        if (track.solo.load())    chips.add ("SOLO");
        if (const int n = track.countPlugins()) chips.add ("FX " + juce::String (n));

        auto chipArea = juce::Rectangle<float> (menuBtn.getRight() + 6.0f, (float) menuBtn.getY(), header.getRight() - menuBtn.getRight() - 24.0f, (float) menuBtn.getHeight());
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        if (! hovered)
            chipArea = chipArea.withX ((float) menuBtn.getX());   // use the "..." space while it's hidden
        g.setOpacity (hovered ? 1.0f : 0.6f);
        for (auto& c : chips)
        {
            const float w = 10.0f + 6.0f * (float) c.length();
            if (w > chipArea.getWidth())
                break;
            auto chip = chipArea.removeFromLeft (w).reduced (0.0f, 2.0f);
            chipArea.removeFromLeft (3.0f);
            const auto col = c == "MON" ? Theme::monitor : c == "SOLO" ? Theme::solo : c.startsWith (arrow) ? Theme::accent2 : Theme::accent;
            g.setColour (col.withAlpha (0.22f));
            g.fillRoundedRectangle (chip, 4.0f);
            g.setColour (col.brighter (0.3f));
            g.drawText (c, chip.toNearestInt(), juce::Justification::centred);
        }
        g.setOpacity (1.0f);
    }

    // slim level meter on the right edge of the row
    auto meterArea = getLocalBounds().withWidth (kHeaderWidth).removeFromRight (14).reduced (3, 8).toFloat();
    drawMeter (g, meterArea.removeFromLeft (3.5f), levelL);
    meterArea.removeFromLeft (1.0f);
    drawMeter (g, meterArea.removeFromLeft (3.5f), levelR);

    // Lane
    const auto lane = laneBounds();
    Glass::well (g, lane.toFloat().reduced (2.0f, 3.0f), 10.0f);

    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (lane.reduced (2, 3));
    drawBarGrid (g, ctx.view, lane, ctx.engine.bpm.load(), false);

    const double sr = juce::jmax (1.0, ctx.engine.getSampleRate());
    const auto dragging = draggingClip.lock();

    for (auto& clipPtr : track.clips)
    {
        const auto& clip = *clipPtr;
        const auto full = clipBounds (clip);
        const float vx0 = juce::jmax (full.getX(), (float) lane.getX());
        const float vx1 = juce::jmin (full.getRight(), (float) lane.getRight());
        if (vx1 <= vx0)
            continue;

        const auto visible = full.withLeft (vx0).withRight (vx1);
        const bool isDragged = dragging.get() == clipPtr.get();
        const bool on = clip.enabled.load();
        const float alpha = on ? 1.0f : 0.35f;

        // liquid glass clip body (in the clip's own colour, if it has one)
        const auto base = ! clip.colour.isTransparent() ? clip.colour : ! track.colour.isTransparent() ? track.colour : Theme::clip;
        g.setGradientFill (juce::ColourGradient (base.withAlpha ((isDragged ? 0.75f : 0.55f) * alpha), visible.getX(), visible.getY(),
                                                 base.darker (0.9f).withAlpha (0.65f * alpha), visible.getX(), visible.getBottom(), false));
        g.fillRoundedRectangle (visible, 6.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.22f * alpha), visible.getX(), visible.getY(),
                                                 juce::Colours::white.withAlpha (0.0f), visible.getX(), visible.getY() + 18.0f, false));
        g.fillRoundedRectangle (visible, 6.0f);
        g.setColour (isDragged ? Theme::accent2 : juce::Colours::white.withAlpha (0.28f * alpha));
        g.drawRoundedRectangle (visible.reduced (0.5f), 6.0f, isDragged ? 2.0f : 1.0f);

        auto it = thumbs.find (clipPtr.get());
        if (it != thumbs.end() && it->second.thumbnail != nullptr)
        {
            const double offsetSec = (double) clip.getOffset() / sr;   // trimmed-off start of the audio
            const double t0 = offsetSec + (vx0 - full.getX()) / ctx.view.pixelsPerSecond;
            const double t1 = offsetSec + (vx1 - full.getX()) / ctx.view.pixelsPerSecond;
            const auto waveArea = visible.reduced (0.0f, 8.0f).withTrimmedTop (6.0f);

            // flat centre line per channel, so silent stretches show a line instead of a gap
            const int numCh = juce::jmax (1, it->second.thumbnail->getNumChannels());
            g.setColour (Theme::wave.withAlpha (0.55f * alpha));
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float bandH = waveArea.getHeight() / (float) numCh;
                g.fillRect (juce::Rectangle<float> (waveArea.getX(), waveArea.getY() + bandH * ((float) ch + 0.5f) - 0.5f, waveArea.getWidth(), 1.0f));
            }

            g.setColour (Theme::wave.withAlpha (0.92f * alpha));
            const float zoom = juce::jlimit (0.05f, 4.0f, juce::Decibels::decibelsToGain (clip.handleGainDb.load(), -60.0f));
            it->second.thumbnail->drawChannels (g, waveArea.toNearestInt(), t0, t1, zoom);
        }

        // trim handles
        const bool trimMode = ctx.view.tool == EditTool::Trim;
        for (int side : { -1, 1 })
        {
            const bool hot = (hoverEdgeClip == clipPtr.get() && hoverEdgeSide == side)
                             || (isDragged && ((side < 0 && dragMode == Drag::TrimStart) || (side > 0 && dragMode == Drag::TrimEnd)));
            if (! hot && ! trimMode)
                continue;
            const float ex = side < 0 ? full.getX() : full.getRight();
            if (ex < (float) lane.getX() || ex > (float) lane.getRight())
                continue;
            auto handle = juce::Rectangle<float> (side < 0 ? ex : ex - 5.0f, full.getY() + 6.0f, 5.0f, full.getHeight() - 12.0f);
            g.setColour ((hot ? Theme::accent2 : Theme::wave).withAlpha (hot ? 0.95f : 0.45f));
            g.fillRoundedRectangle (handle, 2.0f);
        }

        // volume handle (always faintly there; bright while dragging or when the clip has a gain)
        {
            const auto h = gainHandle (clip);
            const bool hot = isDragged && dragMode == Drag::Gain;
            const float gdb = clip.handleGainDb.load();
            if (h.getRight() > (float) lane.getX() && h.getX() < (float) lane.getRight())
            {
                g.setColour ((hot || std::abs (gdb) > 0.01f ? Theme::accent2 : Theme::wave).withAlpha (hot ? 1.0f : 0.5f * alpha));
                g.fillRoundedRectangle (h.withSizeKeepingCentre (h.getWidth(), 4.0f), 2.0f);
                if (hot || std::abs (gdb) > 0.01f)
                {
                    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
                    g.setColour (Theme::text.withAlpha (alpha));
                    g.drawText ((gdb > 0 ? "+" : "") + juce::String (gdb, 1) + " dB", h.translated (20.0f, 0.0f).withWidth (60.0f).expanded (0.0f, 3.0f),
                                juce::Justification::centredLeft);
                }
            }
        }

        g.setColour (Theme::text.withAlpha (0.9f * alpha));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        juce::String label = clip.getDisplayName();
        if (! clip.settings.isNeutral())
            label << "  *fx";
        if (ctx.engine.isPreviewing() && ctx.engine.isPlaying())
            label << "  (preview)";
        g.drawText (label, visible.reduced (7.0f, 3.0f).toNearestInt(), juce::Justification::topLeft, true);
    }

    // comp swipe in progress
    if (dragMode == Drag::Comp)
        if (auto c = draggingClip.lock())
        {
            const auto b = clipBounds (*c);
            const float x0 = ctx.view.timeToX ((double) juce::jmin (compAnchor, compNow) / sr, lane);
            const float x1 = ctx.view.timeToX ((double) juce::jmax (compAnchor, compNow) / sr, lane);
            auto band = juce::Rectangle<float> (x0, b.getY(), juce::jmax (2.0f, x1 - x0), b.getHeight()).getIntersection (b);
            g.setColour (Theme::monitor.withAlpha (0.35f));
            g.fillRoundedRectangle (band, 4.0f);
            g.setColour (Theme::monitor);
            g.drawRoundedRectangle (band, 4.0f, 1.5f);
        }

    // crossfades: an X across each partial overlap
    for (auto& c : track.clips)
    {
        juce::int64 xIn = 0, xOut = 0;
        ctx.engine.crossfadeLengths (track, *c, xIn, xOut);
        if (xIn <= 0) continue;
        const auto b = clipBounds (*c);
        const float x0 = b.getX(), x1 = ctx.view.timeToX ((double) (c->startSample.load() + xIn) / sr, lane);
        if (x1 < (float) lane.getX() || x0 > (float) lane.getRight()) continue;
        g.setColour (Theme::text.withAlpha (0.7f));
        g.drawLine (x0, b.getBottom() - 2.0f, x1, b.getY() + 2.0f, 1.4f);
        g.drawLine (x0, b.getY() + 2.0f, x1, b.getBottom() - 2.0f, 1.4f);
        g.setColour (Theme::accent2.withAlpha (0.12f));
        g.fillRect (juce::Rectangle<float> (x0, b.getY(), x1 - x0, b.getHeight()));
    }

    // Take being recorded right now
    if (ctx.engine.isRecording() && track.armed)
    {
        const float x0 = juce::jmax ((float) lane.getX(), ctx.view.timeToX ((double) ctx.engine.getRecordStart() / sr, lane));
        const float x1 = ctx.view.timeToX ((double) ctx.engine.getTransportPosition() / sr, lane);
        if (x1 > x0)
        {
            g.setColour (Theme::record.withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (x0, (float) lane.getY() + 4.0f, x1 - x0, (float) lane.getHeight() - 8.0f));
        }
    }

    if (track.clips.empty() && ! ctx.engine.isRecording())
    {
        g.setColour (Theme::text.withAlpha (0.18f));
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Record here, or drop audio files (WAV, MP3, FLAC, AIFF, OGG)", lane.reduced (16, 0),
                    juce::Justification::centredLeft);
    }

    // selection: highlighted range on this track, outlines on selected clips
    if (ctx.selection.rangeTrack == &track && ctx.selection.hasRange())
    {
        const float x0 = ctx.view.timeToX ((double) ctx.selection.rangeStart / sr, lane);
        const float x1 = ctx.view.timeToX ((double) ctx.selection.rangeEnd / sr, lane);
        auto band = juce::Rectangle<float> (x0, (float) lane.getY() + 2.0f, juce::jmax (2.0f, x1 - x0), (float) lane.getHeight() - 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.16f));
        g.fillRect (band);
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.drawRect (band, 1.2f);
    }
    for (auto& c : track.clips)
        if (ctx.selection.isSelected (c.get()))
        {
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawRoundedRectangle (clipBounds (*c).reduced (0.5f), 6.0f, 2.0f);
        }

    // cut-tool preview
    if (ctx.view.tool == EditTool::Cut && hoverX >= (float) lane.getX())
    {
        g.setColour (Theme::accent2.withAlpha (0.3f));
        g.fillRect (juce::Rectangle<float> (hoverX - 2.0f, (float) lane.getY() + 3.0f, 5.0f, (float) lane.getHeight() - 6.0f));
        g.setColour (Theme::accent2);
        g.drawVerticalLine ((int) hoverX, (float) lane.getY() + 3.0f, (float) lane.getBottom() - 3.0f);
    }

    const float px = ctx.view.timeToX ((double) ctx.engine.getTransportPosition() / sr, lane);
    g.setColour (Theme::accent2.withAlpha (0.25f));
    g.fillRect (juce::Rectangle<float> (px - 2.0f, (float) lane.getY(), 5.0f, (float) lane.getHeight()));
    g.setColour (Theme::playhead);
    g.drawVerticalLine ((int) px, (float) lane.getY(), (float) lane.getBottom());
}

void TrackComponent::resized()
{
    auto r = getLocalBounds().withWidth (kHeaderWidth).reduced (8, 6);
    r.removeFromRight (12); // meter

    auto row1 = r.removeFromTop (22);
    onLed.setBounds (row1.removeFromRight (20));
    row1.removeFromRight (2);
    armLed.setBounds (row1.removeFromRight (20));
    nameLabel.setBounds (row1.withTrimmedLeft (-4));

    r.removeFromTop (2);
    menuBtn.setBounds (r.removeFromTop (20).withWidth (30));
    inputBox.setBounds (0, 0, 10, 10);
}

//==============================================================================
juce::Rectangle<float> TrackComponent::gainHandle (const AudioClip& clip) const
{
    const auto b = clipBounds (clip);
    return { b.getCentreX() - 15.0f, b.getY() + 1.0f, 30.0f, 9.0f };
}

int TrackComponent::edgeAt (const AudioClip& clip, juce::Point<int> p) const
{
    const auto b = clipBounds (clip);
    if (! b.expanded (2.0f, 0.0f).contains (p.toFloat()))
        return 0;
    const float zone = juce::jmin (8.0f, b.getWidth() * 0.25f);
    if ((float) p.x <= b.getX() + zone)     return -1;
    if ((float) p.x >= b.getRight() - zone) return 1;
    return 0;
}

juce::int64 TrackComponent::sampleAtX (int x, bool bypassSnap) const
{
    // magnet snapping to the grid; holding Shift turns it off for this action
    const double t = ctx.view.snap (timeAtX (x), ctx.engine.bpm.load(), bypassSnap);
    return (juce::int64) std::llround (juce::jmax (0.0, t) * ctx.engine.getSampleRate());
}

void TrackComponent::splitAt (const std::shared_ptr<AudioClip>& clip, juce::int64 at)
{
    if (ctx.engine.splitClip (track, clip, at) != nullptr)
        repaint();
}

void TrackComponent::mouseDown (const juce::MouseEvent& e)
{
    if (track.isAutomation()) { automationMouseDown (e); return; }
    dragMode = Drag::None;
    if (! isInLane (e.getPosition()))
        return;

    auto clip = clipAt (e.getPosition());
    ctx.selection.lastTrack = &track;

    if (e.mods.isPopupMenu())
    {
        // quick click = menu (on release); hold or drag = delete what's under the mouse
        rightDown = true;
        rightDeleting = false;
        rightDownPos = e.getPosition();
        const int token = ++rightToken;
        juce::Component::SafePointer<TrackComponent> safe (this);
        juce::Timer::callAfterDelay (320, [safe, token]
        {
            if (safe != nullptr && safe->rightDown && ! safe->rightDeleting && safe->rightToken == token)
                safe->beginRightDelete();
        });
        return;
    }

    if (e.mods.isCtrlDown() || e.mods.isCommandDown())
    {
        // Ctrl-drag highlights a time range on this track; Ctrl-click a clip adds / removes it from the selection
        dragMode = Drag::Range;
        ctrlClickClip = clip;
        rangeAnchor = sampleAtX (e.x, e.mods.isShiftDown());
        return;
    }

    if (clip == nullptr)
    {
        ctx.selection.clear();
        repaintTimeline();
        ctx.engine.setTransportPosition (sampleAtX (e.x, e.mods.isShiftDown()));
        return;
    }

    // the volume handle at the top of the clip
    if (ctx.view.tool != EditTool::Cut && gainHandle (*clip).expanded (3.0f).contains (e.position))
    {
        if (ctx.checkpoint) ctx.checkpoint ("Clip volume");
        dragMode = Drag::Gain;
        draggingClip = clip;
        gainStartDb = clip->handleGainDb.load();
        gainStartY = e.y;
        return;
    }

    if (! ctx.selection.isSelected (clip.get()))
        ctx.selection.selectOnly (&track, clip);
    else
        ctx.selection.rangeTrack = nullptr;
    repaintTimeline();

    switch (ctx.view.tool)
    {
        case EditTool::Comp:
            // swipe across a take to keep that part (a click keeps the whole take)
            dragMode = Drag::Comp;
            draggingClip = clip;
            compAnchor = compNow = sampleAtX (e.x, e.mods.isShiftDown());
            repaint();
            return;

        case EditTool::Cut:
            if (ctx.checkpoint) ctx.checkpoint ("Cut clip");
            splitAt (clip, sampleAtX (e.x, e.mods.isShiftDown()));
            if (ctx.discardUnchangedCheckpoint) ctx.discardUnchangedCheckpoint(); // too close to an edge = no-op
            return;

        case EditTool::Trim:
        {
            // whichever half you grab is the edge you move
            const auto b = clipBounds (*clip);
            dragMode = (float) e.x < b.getCentreX() ? Drag::TrimStart : Drag::TrimEnd;
            break;
        }

        case EditTool::Move:
        default:
        {
            const int edge = edgeAt (*clip, e.getPosition()); // edges trim even with the Move tool
            dragMode = edge < 0 ? Drag::TrimStart : (edge > 0 ? Drag::TrimEnd : Drag::Move);
            break;
        }
    }

    draggingClip = clip;
    const double sr = juce::jmax (1.0, ctx.engine.getSampleRate());
    dragGrabOffsetSec = timeAtX (e.x) - (double) clip->startSample.load() / sr;
    grabbedStart = clip->startSample.load();
    groupStarts.clear();
    if (dragMode == Drag::Move)
        for (auto& [t, w] : ctx.selection.clips)
            if (auto other = w.lock(); other != nullptr && other != clip)
                groupStarts.push_back ({ other, other->startSample.load() });
    if (ctx.checkpoint)
        ctx.checkpoint (dragMode == Drag::Move ? "Move clip" : "Trim clip");
}

void TrackComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (track.isAutomation()) { automationMouseDrag (e); return; }
    if (rightDown)
    {
        if (! rightDeleting && e.getDistanceFromDragStart() > 3)
            beginRightDelete();
        if (rightDeleting)
            deleteUnder (e.getPosition());
        return;
    }
    if (dragMode == Drag::Comp)
    {
        compNow = sampleAtX (e.x, e.mods.isShiftDown());
        repaint();
        return;
    }
    if (dragMode == Drag::Gain)
    {
        if (auto c = draggingClip.lock())
        {
            // 0.2 dB per pixel (Shift = fine), snaps to 0 dB near the middle
            const float perPx = e.mods.isShiftDown() ? 0.05f : 0.2f;
            float db = juce::jlimit (-36.0f, 12.0f, gainStartDb + (float) (gainStartY - e.y) * perPx);
            if (std::abs (db) < 0.3f) db = 0.0f;
            c->handleGainDb = db;
            repaint();
        }
        return;
    }
    if (dragMode == Drag::Range)
    {
        if (e.getDistanceFromDragStart() < 4)
            return;
        const auto s = sampleAtX (e.x, e.mods.isShiftDown());
        ctx.selection.clips.clear();
        ctx.selection.rangeTrack = &track;
        ctx.selection.rangeStart = juce::jmin (s, rangeAnchor);
        ctx.selection.rangeEnd = juce::jmax (s, rangeAnchor);
        repaintTimeline();
        return;
    }
    if (draggingClip.expired())
        return;

    lastDragX = e.x;
    lastBypassSnap = e.mods.isShiftDown();
    applyDrag (e.x, lastBypassSnap);
    if (! isTimerRunning())
        startTimerHz (40);
}

void TrackComponent::applyDrag (int x, bool bypassSnap)
{
    auto clip = draggingClip.lock();
    if (clip == nullptr)
        return;

    switch (dragMode)
    {
        case Drag::TrimStart: ctx.engine.trimClipStart (*clip, sampleAtX (x, bypassSnap)); break;
        case Drag::TrimEnd:   ctx.engine.trimClipEnd (*clip, sampleAtX (x, bypassSnap));   break;
        case Drag::Move:
        {
            const double sr = juce::jmax (1.0, ctx.engine.getSampleRate());
            const double newStart = ctx.view.snap (timeAtX (x) - dragGrabOffsetSec, ctx.engine.bpm.load(), bypassSnap);
            clip->startSample = (juce::int64) std::llround (juce::jmax (0.0, newStart) * sr);
            // the rest of the selection moves by the same amount
            if (! groupStarts.empty())
            {
                auto delta = clip->startSample.load() - grabbedStart;
                for (auto& [w, s0] : groupStarts)
                    delta = juce::jmax (delta, -s0);   // nothing goes before the start
                clip->startSample = grabbedStart + delta;
                for (auto& [w, s0] : groupStarts)
                    if (auto other = w.lock())
                        other->startSample = s0 + delta;
                repaintTimeline();
            }
            break;
        }
        case Drag::None: break;
    }
    repaint();
}

void TrackComponent::timerCallback()
{
    if (draggingClip.expired() || dragMode == Drag::None)
    {
        stopTimer();
        return;
    }
    // holding a clip against the left/right edge keeps scrolling, and the clip comes along
    if (ctx.view.autoScroll ((float) lastDragX, laneBounds()))
    {
        applyDrag (lastDragX, lastBypassSnap);
        if (auto* p = getParentComponent())
            p->repaint();
    }
}

void TrackComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (isInLane (e.getPosition()) && handleTimelineWheel (ctx.view, e, w, laneBounds()))
    {
        if (auto* p = getParentComponent())
            p->repaint();
        return;
    }
    juce::Component::mouseWheelMove (e, w); // plain wheel scrolls the track list up/down
}

void TrackComponent::mouseUp (const juce::MouseEvent& e)
{
    if (rightDown)
    {
        const bool wasDeleting = rightDeleting;
        rightDown = false;
        rightDeleting = false;
        ++rightToken;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        if (! wasDeleting)
        {
            if (auto clip = clipAt (rightDownPos)) showClipMenu (clip, sampleAtX (rightDownPos.x, e.mods.isShiftDown()));
            else                                   showLaneMenu();
        }
        return;
    }
    if (dragMode == Drag::Comp)
    {
        dragMode = Drag::None;
        if (auto c = draggingClip.lock())
        {
            juce::int64 a = juce::jmin (compAnchor, compNow), b = juce::jmax (compAnchor, compNow);
            if (e.getDistanceFromDragStart() < 4) { a = c->startSample.load(); b = c->getEnd(); }   // click: the whole take
            if (ctx.checkpoint) ctx.checkpoint ("Comp take");
            ctx.engine.compTake (track, c, a, b);
            if (ctx.discardUnchangedCheckpoint) ctx.discardUnchangedCheckpoint();
        }
        draggingClip.reset();
        repaintTimeline();
        return;
    }
    if (dragMode == Drag::Range)
    {
        dragMode = Drag::None;
        if (e.getDistanceFromDragStart() < 4 && ctrlClickClip != nullptr)
            ctx.selection.toggle (&track, ctrlClickClip);
        ctrlClickClip = nullptr;
        repaintTimeline();
        return;
    }
    autoDragIndex = -1;
    stopTimer();
    if (dragMode != Drag::None && ctx.discardUnchangedCheckpoint)
        ctx.discardUnchangedCheckpoint(); // a click without a drag isn't an undo step
    draggingClip.reset();
    dragMode = Drag::None;
    updateCursor (e.getPosition());
}

void TrackComponent::updateCursor (juce::Point<int> p)
{
    const AudioClip* edgeClip = nullptr;
    int edgeSide = 0;
    auto clip = isInLane (p) ? clipAt (p) : nullptr;
    auto cursorType = juce::MouseCursor::NormalCursor;
    float newHover = -1.0f;

    if (clip != nullptr)
    {
        switch (ctx.view.tool)
        {
            case EditTool::Cut:
                cursorType = juce::MouseCursor::CrosshairCursor;
                newHover = (float) p.x;
                break;
            case EditTool::Comp:
                cursorType = juce::MouseCursor::IBeamCursor;
                break;
            case EditTool::Trim:
                cursorType = juce::MouseCursor::LeftRightResizeCursor;
                edgeClip = clip.get();
                edgeSide = (float) p.x < clipBounds (*clip).getCentreX() ? -1 : 1;
                break;
            case EditTool::Move:
            default:
                if (gainHandle (*clip).expanded (3.0f).contains (p.toFloat()))
                {
                    cursorType = juce::MouseCursor::UpDownResizeCursor;
                    break;
                }
                edgeSide = edgeAt (*clip, p);
                edgeClip = edgeSide != 0 ? clip.get() : nullptr;
                cursorType = edgeSide != 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor;
                break;
        }
    }

    setMouseCursor (cursorType);
    if (newHover != hoverX || edgeClip != hoverEdgeClip || edgeSide != hoverEdgeSide)
    {
        hoverX = newHover;
        hoverEdgeClip = edgeClip;
        hoverEdgeSide = edgeSide;
        repaint();
    }
}

void TrackComponent::mouseMove (const juce::MouseEvent& e)
{
    if (track.isAutomation()) { automationMouseMove (e); return; }
    updateCursor (e.getPosition());
}

void TrackComponent::mouseExit (const juce::MouseEvent&)
{
    hoverX = -1.0f;
    hoverEdgeClip = nullptr;
    hoverEdgeSide = 0;
    repaint();
}

void TrackComponent::repaintTimeline()
{
    if (auto* p = getParentComponent()) p->repaint();
    else repaint();
}

void TrackComponent::beginRightDelete()
{
    rightDeleting = true;
    if (ctx.checkpoint) ctx.checkpoint ("Delete clips");
    setMouseCursor (juce::MouseCursor::CrosshairCursor);
    deleteUnder (getMouseXYRelative());
}

void TrackComponent::deleteUnder (juce::Point<int> pos)
{
    // works across tracks: find the row under the mouse
    TrackComponent* target = this;
    auto local = pos;
    if (auto* parent = getParentComponent())
    {
        const auto inParent = pos + getPosition();
        for (auto* child : parent->getChildren())
            if (auto* tc = dynamic_cast<TrackComponent*> (child); tc != nullptr && tc->getBounds().contains (inParent))
            {
                target = tc;
                local = inParent - tc->getPosition();
                break;
            }
    }
    if (target->track.isAutomation() || ! target->isInLane (local))
        return;
    if (auto clip = target->clipAt (local))
    {
        ctx.engine.removeClip (target->track, clip.get());
        target->repaint();
    }
}

void TrackComponent::showLaneMenu()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Overlapping takes on " + track.name);
    m.addItem (1, "Layer - hear every take", true, track.layerTakes.load());
    m.addItem (2, "Newest take on top", true, ! track.layerTakes.load());

    juce::Component::SafePointer<TrackComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe] (int r)
    {
        if (safe != nullptr && r > 0)
            safe->track.layerTakes = (r == 1);
    });
}

void TrackComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (track.isAutomation()) { automationDoubleClick (e); return; }
    if (auto clip = clipAt (e.getPosition()))
        if (ctx.openClipEditor)
            ctx.openClipEditor (&track, clip);
}

namespace
{
    const std::array<std::pair<const char*, juce::uint32>, 10> kClipColours { {
        { "Amethyst", 0xff8f5cff }, { "Orchid", 0xffd65cff }, { "Rose", 0xffff70d9 }, { "Ruby", 0xffff5c7b },
        { "Amber", 0xffff9d5c }, { "Gold", 0xffffd65c }, { "Lime", 0xff9dff5c }, { "Mint", 0xff5cffc8 },
        { "Sky", 0xff5cd6ff }, { "Sapphire", 0xff5c7dff } } };

    enum ClipMenuId
    {
        idPreview = 1, idMute, idRename, idProperties, idDuplicate, idShowFile, idDelete, idShowChannel, idMoveToPlayhead,
        idSplitHere, idSplitPlayhead, idTrimStartPh, idTrimEndPh, idRestore, idSplitSilence, idRemoveSilence,
        idReverse, idNormalize, idDetectTempo, idFitTempo, idDetectKey, idResetFades, idLayer, idNewest,
        idColourDefault = 100, idColourRandom = 101, idColour0 = 110,
        idChop0 = 200, idPitch0 = 300, idFadeIn0 = 400, idFadeOut0 = 500, idSendTo0 = 1000
    };

    const std::array<std::pair<const char*, double>, 4> kChops { { { "Every bar", 4.0 }, { "Every beat", 1.0 }, { "Every 1/2 beat", 0.5 }, { "At the snap grid", -1.0 } } };
    const std::array<int, 9> kPitches { { -12, -7, -5, -2, 0, 2, 5, 7, 12 } };
    const std::array<std::pair<const char*, float>, 6> kFades { { { "None", 0.0f }, { "10 ms", 10.0f }, { "50 ms", 50.0f }, { "200 ms", 200.0f },
                                                                  { "500 ms", 500.0f }, { "1 s", 1000.0f } } };
}

void TrackComponent::showClipMenu (std::shared_ptr<AudioClip> clip, juce::int64 clickSample)
{
    const auto playhead = ctx.engine.getTransportPosition();
    const bool playheadInside = playhead > clip->startSample.load() && playhead < clip->getEnd();
    const bool trimmed = clip->getOffset() > 0 || clip->trimLength.load() >= 0;
    const auto& s = clip->settings;

    juce::PopupMenu m;
    m.addSectionHeader ("Clip  -  " + clip->getDisplayName());
    m.addItem (idPreview, "Preview (play only this clip)");
    m.addItem (idMute, "Muted", true, ! clip->enabled.load());
    m.addItem (idRename, "Rename...");
    {
        juce::PopupMenu colours;
        colours.addItem (idColourDefault, "Default", true, clip->colour.isTransparent());
        colours.addItem (idColourRandom, "Random colour");
        colours.addSeparator();
        for (int i = 0; i < (int) kClipColours.size(); ++i)
            colours.addColouredItem (idColour0 + i, kClipColours[(size_t) i].first, juce::Colour (kClipColours[(size_t) i].second),
                                     true, clip->colour == juce::Colour (kClipColours[(size_t) i].second));
        m.addSubMenu ("Colour", colours);
    }
    m.addItem (idProperties, "Clip properties...  (double-click)");
    m.addItem (idDuplicate, "Duplicate");
    {
        juce::PopupMenu sendTo;
        int i = 0;
        for (auto* t : ctx.engine.getTracks())
        {
            if (t != &track)
                sendTo.addItem (idSendTo0 + i, t->name);
            ++i;
        }
        m.addSubMenu ("Send to track", sendTo);
    }
    m.addItem (idShowChannel, "Show this track's mixer channel");
    m.addItem (idShowFile, "Show file in Explorer");
    m.addItem (idDelete, "Delete");

    m.addSectionHeader ("Edit");
    m.addItem (idSplitHere, "Split here");
    m.addItem (idSplitPlayhead, "Split at playhead", playheadInside);
    m.addItem (idTrimStartPh, "Trim start to playhead", playheadInside);
    m.addItem (idTrimEndPh, "Trim end to playhead", playheadInside);
    m.addItem (idRestore, "Restore full length", trimmed);
    m.addItem (idMoveToPlayhead, "Move to playhead");
    {
        juce::PopupMenu chop;
        for (int i = 0; i < (int) kChops.size(); ++i)
            chop.addItem (idChop0 + i, kChops[(size_t) i].first);
        m.addSubMenu ("Chop", chop);
    }
    m.addItem (idSplitSilence, "Split at silences");
    m.addItem (idRemoveSilence, "Remove silence (keep only the singing)");

    m.addSectionHeader ("Sound");
    m.addItem (idReverse, "Reverse", true, s.reverse);
    m.addItem (idNormalize, "Normalize", true, s.normalize);
    {
        juce::PopupMenu pitch;
        for (int i = 0; i < (int) kPitches.size(); ++i)
            pitch.addItem (idPitch0 + i, kPitches[(size_t) i] == 0 ? juce::String ("Original")
                                                                    : (kPitches[(size_t) i] > 0 ? "+" : "") + juce::String (kPitches[(size_t) i]) + " semitones",
                           true, (int) s.pitchSemis == kPitches[(size_t) i]);
        m.addSubMenu ("Pitch (keeps length)", pitch);
    }
    m.addItem (idDetectTempo, "Detect tempo...");
    m.addItem (idFitTempo, "Fit to project tempo");
    m.addItem (idDetectKey, "Detect key...");
    if (ctx.clipAction)
    {
        const int tier = ctx.tier ? ctx.tier() : 0;
        juce::PopupMenu::Item rr (juce::String ("Reverse reverb swell (lead-in)") + (tier >= 1 ? "" : "   (Pro)"));
        rr.setID (900000).setAction ([fn = ctx.clipAction, tp = &track, clip] { fn (*tp, clip, 1); });
        m.addItem (rr);
    }

    m.addSectionHeader ("Fades");
    {
        juce::PopupMenu fadeIn, fadeOut;
        for (int i = 0; i < (int) kFades.size(); ++i)
        {
            fadeIn.addItem (idFadeIn0 + i, kFades[(size_t) i].first, true, std::abs (s.fadeInMs - kFades[(size_t) i].second) < 0.5f);
            fadeOut.addItem (idFadeOut0 + i, kFades[(size_t) i].first, true, std::abs (s.fadeOutMs - kFades[(size_t) i].second) < 0.5f);
        }
        m.addSubMenu ("Fade in", fadeIn);
        m.addSubMenu ("Fade out", fadeOut);
    }
    m.addItem (idResetFades, "Reset fades", s.fadeInMs > 0.0f || s.fadeOutMs > 0.0f);

    m.addSeparator();
    {
        juce::PopupMenu takes;
        takes.addItem (idLayer, "Layer - hear every take", true, track.layerTakes.load());
        takes.addItem (idNewest, "Newest take on top", true, ! track.layerTakes.load());
        m.addSubMenu ("Overlapping takes", takes);
    }

    std::weak_ptr<AudioClip> weak = clip;
    juce::Component::SafePointer<TrackComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, weak, clickSample, playhead] (int r)
    {
        auto c = weak.lock();
        if (safe == nullptr || c == nullptr || r == 0)
            return;
        safe->handleClipMenu (c, r, clickSample, playhead);
        safe->repaint();
    });
}

void TrackComponent::handleClipMenu (const std::shared_ptr<AudioClip>& c, int r, juce::int64 clickSample, juce::int64 playhead)
{
    auto& engine = ctx.engine;
    auto checkpoint = [this] (const char* what) { if (ctx.checkpoint) ctx.checkpoint (what); };
    auto rerender = [&] { engine.requestRender (c); };
    const double sr = juce::jmax (1.0, engine.getSampleRate());

    if (r >= idSendTo0)
    {
        if (auto* target = engine.getTracks()[r - idSendTo0])
        {
            checkpoint ("Send clip to track");
            if (auto moved = engine.removeClip (track, c.get()))
                engine.addClip (*target, std::move (moved));
        }
        return;
    }
    if (r >= idFadeOut0) { checkpoint ("Fade out"); c->settings.fadeOutMs = kFades[(size_t) (r - idFadeOut0)].second; rerender(); return; }
    if (r >= idFadeIn0)  { checkpoint ("Fade in");  c->settings.fadeInMs = kFades[(size_t) (r - idFadeIn0)].second;  rerender(); return; }
    if (r >= idPitch0)
    {
        checkpoint ("Change pitch");
        c->settings.pitchSemis = (float) kPitches[(size_t) (r - idPitch0)];
        if (c->settings.pitchSemis != 0.0f)
            c->settings.timeMode = 1; // keep the length
        rerender();
        return;
    }
    if (r >= idChop0)
    {
        const double beats = kChops[(size_t) (r - idChop0)].second > 0 ? kChops[(size_t) (r - idChop0)].second : ctx.view.snapBeats();
        if (beats <= 0.0)
            return;
        checkpoint ("Chop clip");
        const auto unit = (juce::int64) std::llround (beats * 60.0 / juce::jmax (20.0, engine.bpm.load()) * sr);
        const auto end0 = c->getEnd();
        auto piece = c;
        for (auto at = ((c->startSample.load() / unit) + 1) * unit; at < end0; at += unit)
            if (auto next = engine.splitClip (track, piece, at)) // skips cuts too close to an edge
                piece = next;
        return;
    }
    if (r >= idColour0)
    {
        checkpoint ("Clip colour");
        c->colour = juce::Colour (kClipColours[(size_t) (r - idColour0)].second);
        return;
    }

    switch (r)
    {
        case idPreview:        engine.previewClipSolo (*c); break;
        case idMute:           checkpoint ("Mute clip"); c->enabled = ! c->enabled.load(); break;
        case idRename:         showRenameBox (c); break;
        case idColourDefault:  checkpoint ("Clip colour"); c->colour = {}; break;
        case idColourRandom:
            checkpoint ("Clip colour");
            c->colour = juce::Colour::fromHSV (juce::Random::getSystemRandom().nextFloat(), 0.62f, 1.0f, 1.0f);
            break;
        case idProperties:     if (ctx.openClipEditor) ctx.openClipEditor (&track, c); break;
        case idDuplicate:
        {
            checkpoint ("Duplicate clip");
            auto copy = std::make_shared<AudioClip>();
            copy->source = c->source; copy->buffer = c->buffer; copy->file = c->file; copy->lossless = c->lossless; copy->handleGainDb = c->handleGainDb.load();
            copy->settings = c->settings; copy->name = c->name; copy->colour = c->colour;
            copy->enabled = c->enabled.load();
            copy->trimOffset = c->trimOffset.load(); copy->trimLength = c->trimLength.load();
            copy->startSample = (juce::int64) std::llround (ctx.view.snap ((double) c->getEnd() / sr, engine.bpm.load(), false) * sr);
            engine.addClip (track, std::move (copy));
            break;
        }
        case idShowChannel:    if (ctx.showInMixer) ctx.showInMixer (track.playThrough != nullptr ? track.playThrough : &track); break;
        case idShowFile:       c->file.revealToUser(); break;
        case idDelete:         checkpoint ("Delete clip"); engine.removeClip (track, c.get()); break;
        case idMoveToPlayhead: checkpoint ("Move clip to playhead"); c->startSample = playhead; break;
        case idSplitHere:      checkpoint ("Split clip"); splitAt (c, clickSample); break;
        case idSplitPlayhead:  checkpoint ("Split clip"); splitAt (c, playhead); break;
        case idTrimStartPh:    checkpoint ("Trim clip"); engine.trimClipStart (*c, playhead); break;
        case idTrimEndPh:      checkpoint ("Trim clip"); engine.trimClipEnd (*c, playhead); break;
        case idRestore:
        {
            checkpoint ("Restore clip length");
            const auto originalStart = c->startSample.load() - c->getOffset();
            engine.trimClipStart (*c, originalStart);
            engine.trimClipEnd (*c, originalStart + c->getBufferLength());
            break;
        }
        case idSplitSilence:   checkpoint ("Split at silences"); splitAtSilences (c, false); break;
        case idRemoveSilence:  checkpoint ("Remove silence"); splitAtSilences (c, true); break;
        case idReverse:        checkpoint ("Reverse clip"); c->settings.reverse = ! c->settings.reverse; rerender(); break;
        case idNormalize:      checkpoint ("Normalize clip"); c->settings.normalize = ! c->settings.normalize; rerender(); break;
        case idResetFades:     checkpoint ("Reset fades"); c->settings.fadeInMs = c->settings.fadeOutMs = 0.0f; rerender(); break;
        case idDetectTempo:
        case idFitTempo:
        {
            const double bpm = c->buffer != nullptr ? ClipFx::detectTempo (*c->buffer, (int) c->getOffset(), (int) c->getLength(), sr) : 0.0;
            if (bpm <= 0.0)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Detect tempo",
                                                        "Couldn't find a steady beat in this clip (it needs a few seconds of rhythmic audio).");
                break;
            }
            const double project = engine.bpm.load();
            if (r == idFitTempo)
            {
                checkpoint ("Fit clip to tempo");
                c->settings.timeMode = 1;
                c->settings.speed = (float) juce::jlimit (0.5, 2.0, project / bpm);
                rerender();
                break;
            }
            UiHelpers::offerTempo (engine, bpm, "Detected tempo", "\"" + c->getDisplayName() + "\"");
            break;
        }
        case idDetectKey:
        {
            const auto key = c->buffer != nullptr ? ClipFx::detectKey (*c->buffer, (int) c->getOffset(), (int) c->getLength(), sr) : ClipFx::KeyResult();
            if (key.tonic < 0)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Detect key",
                                                        "Couldn't hear a clear key in this clip. Try a longer part with chords or melody.");
                break;
            }
            // Offer to set AMETHYST TUNE on the channel this clip plays through.
            Track& channel = track.playThrough != nullptr ? *track.playThrough : track;
            AmethystPlugin* tune = nullptr;
            for (auto& slot : channel.inserts)
                if (auto* am = dynamic_cast<AmethystPlugin*> (slot->plugin.get()); am != nullptr && am->pluginId == "amethyst.tune")
                    tune = am;
            const auto text = "This clip sounds like " + key.name() + juce::String (key.confidence < 0.25f ? " (not very sure)." : ".");
            if (tune == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Detected key", text);
                break;
            }
            const int tonic = key.tonic;
            const bool minor = key.minor;
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Detected key",
                                                text + "\n\nSet AMETHYST TUNE on \"" + channel.name + "\" to " + key.name() + "?",
                                                "Set key", "Close", nullptr,
                                                juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<TrackComponent> (this), tonic, minor] (int ok)
            {
                if (ok != 1 || safe == nullptr)
                    return;
                // look the plug-in up again: it may have been removed while the box was open
                Track& ch = safe->track.playThrough != nullptr ? *safe->track.playThrough : safe->track;
                for (auto& slot : ch.inserts)
                    if (auto* am = dynamic_cast<AmethystPlugin*> (slot->plugin.get()); am != nullptr && am->pluginId == "amethyst.tune")
                    {
                        am->setParam ("key", (float) tonic);
                        am->setParam ("scale", minor ? 2.0f : 1.0f);
                    }
            }));
            break;
        }
        case idLayer:          track.layerTakes = true; break;
        case idNewest:         track.layerTakes = false; break;
        default: break;
    }
}

void TrackComponent::showRenameBox (const std::shared_ptr<AudioClip>& c)
{
    renameBox = std::make_unique<juce::AlertWindow> ("Rename clip", "", juce::MessageBoxIconType::NoIcon);
    renameBox->addTextEditor ("name", c->getDisplayName());
    renameBox->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    renameBox->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    std::weak_ptr<AudioClip> weak = c;
    juce::Component::SafePointer<TrackComponent> safe (this);
    renameBox->enterModalState (true, juce::ModalCallbackFunction::create ([safe, weak] (int r)
    {
        if (safe == nullptr || safe->renameBox == nullptr)
            return;
        if (auto clip = weak.lock(); clip != nullptr && r == 1)
        {
            if (safe->ctx.checkpoint) safe->ctx.checkpoint ("Rename clip");
            clip->name = safe->renameBox->getTextEditorContents ("name").trim();
        }
        safe->renameBox.reset();
        safe->repaint();
    }), false);
}

void TrackComponent::splitAtSilences (const std::shared_ptr<AudioClip>& clip, bool removeSilentParts)
{
    if (clip->buffer == nullptr)
        return;

    const double sr = juce::jmax (1.0, ctx.engine.getSampleRate());
    const auto start = clip->startSample.load();
    const auto spans = ClipFx::findSilences (*clip->buffer, (int) clip->getOffset(), (int) clip->getLength(), sr);
    const auto pad = (juce::int64) (sr * 0.03); // keep 30 ms of air around the singing

    auto current = clip; // always the left-most remaining piece (we work right to left)
    for (auto it = spans.rbegin(); it != spans.rend() && current != nullptr; ++it)
    {
        const auto a = start + it->first, b = start + it->second;
        const auto curStart = current->startSample.load(), curEnd = current->getEnd();
        const bool atStart = a <= curStart + pad;
        const bool atEnd = b >= curEnd - pad;

        if (atStart && atEnd) // the whole clip is silence
        {
            if (removeSilentParts)
                ctx.engine.removeClip (track, current.get());
            break;
        }
        if (atEnd)
        {
            if (auto silent = ctx.engine.splitClip (track, current, a + pad); silent != nullptr && removeSilentParts)
                ctx.engine.removeClip (track, silent.get());
            continue;
        }
        if (atStart)
        {
            ctx.engine.splitClip (track, current, b - pad);
            if (removeSilentParts)
                ctx.engine.removeClip (track, current.get());
            break;
        }
        ctx.engine.splitClip (track, current, b - pad);
        if (auto silent = ctx.engine.splitClip (track, current, a + pad); silent != nullptr && removeSilentParts)
            ctx.engine.removeClip (track, silent.get());
    }
}

//==============================================================================
void RulerComponent::paint (juce::Graphics& g)
{
    auto lane = getLocalBounds().withTrimmedLeft (kHeaderWidth);
    Glass::panel (g, lane.toFloat().reduced (2.0f, 2.0f), 8.0f, 0.8f);

    lane = lane.reduced (2, 2);

    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (lane);
    drawBarGrid (g, ctx.view, lane, ctx.engine.bpm.load(), true);

    const double sr = juce::jmax (1.0, ctx.engine.getSampleRate());
    if (ctx.engine.hasPunchRegion())
    {
        const float x0 = ctx.view.timeToX ((double) ctx.engine.punchIn.load() / sr, lane);
        const float x1 = ctx.view.timeToX ((double) ctx.engine.punchOut.load() / sr, lane);
        const bool on = ctx.engine.punchEnabled.load();
        const auto band = juce::Rectangle<float> (x0, (float) lane.getY(), juce::jmax (2.0f, x1 - x0), (float) lane.getHeight());
        g.setColour (Theme::record.withAlpha (on ? 0.28f : 0.10f));
        g.fillRect (band);
        g.setColour (Theme::record.withAlpha (on ? 0.9f : 0.35f));
        g.drawVerticalLine ((int) x0, band.getY(), band.getBottom());
        g.drawVerticalLine ((int) x1, band.getY(), band.getBottom());
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText ("PUNCH", band.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft);
    }
    // Loop region: a band along the top of the ruler
    if (ctx.engine.hasLoopRegion())
    {
        const float x0 = ctx.view.timeToX ((double) ctx.engine.loopStart.load() / sr, lane);
        const float x1 = ctx.view.timeToX ((double) ctx.engine.loopEnd.load() / sr, lane);
        const bool on = ctx.engine.loopEnabled.load();
        const auto band = juce::Rectangle<float> (x0, (float) lane.getY(), juce::jmax (2.0f, x1 - x0), 8.0f);
        g.setColour (Theme::accent.withAlpha (on ? 0.85f : 0.3f));
        g.fillRoundedRectangle (band, 3.0f);
        g.setColour (Theme::accent.withAlpha (on ? 0.16f : 0.06f));
        g.fillRect (juce::Rectangle<float> (x0, (float) lane.getY(), juce::jmax (2.0f, x1 - x0), (float) lane.getHeight()));
        g.setColour (Theme::accent.withAlpha (on ? 0.9f : 0.4f));
        g.drawVerticalLine ((int) x0, (float) lane.getY(), (float) lane.getBottom());
        g.drawVerticalLine ((int) x1, (float) lane.getY(), (float) lane.getBottom());
    }

    // Markers: little flags with names
    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    for (auto& m : ctx.engine.markers)
    {
        const float mx = ctx.view.timeToX (m.seconds, lane);
        if (mx < (float) lane.getX() - 120.0f || mx > (float) lane.getRight())
            continue;
        g.setColour (Theme::accent2);
        g.drawVerticalLine ((int) mx, (float) lane.getY(), (float) lane.getBottom());
        const float w = juce::jmin (120.0f, juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), m.name) + 10.0f);
        auto flag = juce::Rectangle<float> (mx, (float) lane.getBottom() - 15.0f, w, 13.0f);
        g.setColour (Theme::accent2.withAlpha (0.85f));
        g.fillRoundedRectangle (flag, 3.0f);
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.drawText (m.name, flag.reduced (4.0f, 0.0f), juce::Justification::centredLeft, true);
    }

    const float px = ctx.view.timeToX ((double) ctx.engine.getTransportPosition() / sr, lane);
    juce::Path tri;
    tri.addTriangle (px - 6.0f, 0.0f, px + 6.0f, 0.0f, px, 10.0f);
    g.setColour (ctx.engine.isRecording() ? Theme::record : Theme::playhead);
    g.fillPath (tri);
    g.drawVerticalLine ((int) px, 0.0f, (float) getHeight());
}

double RulerComponent::timeAt (int x, bool bypass) const
{
    auto lane = getLocalBounds().withTrimmedLeft (kHeaderWidth);
    const int clamped = juce::jlimit (lane.getX(), lane.getRight(), x);
    return juce::jmax (0.0, ctx.view.snap (ctx.view.xToTime ((float) clamped, lane), ctx.engine.bpm.load(), bypass));
}

void RulerComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { showRulerMenu (e.x); return; }
    if (e.mods.isAltDown())
    {
        // Alt-drag marks the loop region
        loopDragging = true;
        punchAnchor = timeAt (e.x, e.mods.isShiftDown());
        const auto s = (juce::int64) std::llround (punchAnchor * ctx.engine.getSampleRate());
        ctx.engine.loopStart = s;
        ctx.engine.loopEnd = s;
        repaint();
        return;
    }
    if (e.mods.isCtrlDown())
    {
        // Ctrl-drag marks the punch region
        punchDragging = true;
        punchAnchor = timeAt (e.x, e.mods.isShiftDown());
        const auto s = (juce::int64) std::llround (punchAnchor * ctx.engine.getSampleRate());
        ctx.engine.punchIn = s;
        ctx.engine.punchOut = s;
        repaint();
        return;
    }
    dragX = e.x; bypassSnap = e.mods.isShiftDown(); seekTo (e.x); startTimerHz (40);
}

void RulerComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (loopDragging)
    {
        const double t = timeAt (e.x, e.mods.isShiftDown());
        const double sr = ctx.engine.getSampleRate();
        ctx.engine.loopStart = (juce::int64) std::llround (juce::jmin (t, punchAnchor) * sr);
        ctx.engine.loopEnd = (juce::int64) std::llround (juce::jmax (t, punchAnchor) * sr);
        ctx.engine.loopEnabled = ctx.engine.hasLoopRegion();
        repaint();
        return;
    }
    if (punchDragging)
    {
        const double t = timeAt (e.x, e.mods.isShiftDown());
        const double sr = ctx.engine.getSampleRate();
        ctx.engine.punchIn = (juce::int64) std::llround (juce::jmin (t, punchAnchor) * sr);
        ctx.engine.punchOut = (juce::int64) std::llround (juce::jmax (t, punchAnchor) * sr);
        ctx.engine.punchEnabled = ctx.engine.hasPunchRegion();
        repaint();
        return;
    }
    dragX = e.x; bypassSnap = e.mods.isShiftDown(); seekTo (e.x);
}

void RulerComponent::showRulerMenu (int x)
{
    const double t = timeAt (x, false);
    auto& eng = ctx.engine;
    juce::PopupMenu m;
    m.addSectionHeader ("Punch-in");
    m.addItem (1, "Punch recording on", eng.hasPunchRegion(), eng.punchEnabled.load());
    m.addItem (2, "Set punch in here");
    m.addItem (3, "Set punch out here");
    m.addItem (4, "Clear punch region", eng.hasPunchRegion());
    m.addSectionHeader ("Loop  (Alt-drag here, L toggles)");
    m.addItem (10, "Loop playback", eng.hasLoopRegion(), eng.loopEnabled.load());
    m.addItem (11, "Loop this bar");
    m.addItem (12, "Loop the punch region", eng.hasPunchRegion());
    m.addItem (13, "Clear loop", eng.hasLoopRegion());
    m.addSectionHeader ("Markers  (M adds one at the playhead)");
    m.addItem (20, "Add marker here");
    int nearest = -1;
    double best = 1.0e9;
    for (int i = 0; i < (int) eng.markers.size(); ++i)
    {
        const double d = std::abs (ctx.view.timeToX (eng.markers[(size_t) i].seconds, getLocalBounds().withTrimmedLeft (kHeaderWidth)) - (float) x);
        if (d < best) { best = d; nearest = i; }
    }
    if (nearest >= 0 && best < 60.0)
    {
        m.addItem (21, "Rename \"" + eng.markers[(size_t) nearest].name + "\"...");
        m.addItem (22, "Delete \"" + eng.markers[(size_t) nearest].name + "\"");
    }
    m.addItem (23, "Delete all markers", ! eng.markers.empty());
    juce::Component::SafePointer<RulerComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, t, nearest] (int r)
    {
        if (safe == nullptr || r == 0) return;
        auto& e = safe->ctx.engine;
        const auto s = (juce::int64) std::llround (t * e.getSampleRate());
        const auto barLen = (juce::int64) (4.0 * 60.0 / juce::jmax (20.0, e.bpm.load()) * e.getSampleRate());
        if (r == 10) e.loopEnabled = ! e.loopEnabled.load();
        if (r == 11) { e.loopStart = (s / barLen) * barLen; e.loopEnd = e.loopStart.load() + barLen; e.loopEnabled = true; }
        if (r == 12) { e.loopStart = e.punchIn.load(); e.loopEnd = e.punchOut.load(); e.loopEnabled = true; }
        if (r == 13) { e.loopStart = -1; e.loopEnd = -1; e.loopEnabled = false; }
        if (r == 20) addMarkerAt (e, t);
        if (r == 22 && juce::isPositiveAndBelow (nearest, (int) e.markers.size())) e.markers.erase (e.markers.begin() + nearest);
        if (r == 23) e.markers.clear();
        if (r == 21 && juce::isPositiveAndBelow (nearest, (int) e.markers.size()))
        {
            auto* box = new juce::AlertWindow ("Rename marker", {}, juce::MessageBoxIconType::NoIcon);
            box->addTextEditor ("name", e.markers[(size_t) nearest].name);
            box->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
            box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            box->enterModalState (true, juce::ModalCallbackFunction::create ([safe, box, nearest] (int result)
            {
                if (result == 1 && safe != nullptr && juce::isPositiveAndBelow (nearest, (int) safe->ctx.engine.markers.size()))
                {
                    const auto name = box->getTextEditorContents ("name").trim();
                    if (name.isNotEmpty()) safe->ctx.engine.markers[(size_t) nearest].name = name;
                    safe->repaint();
                }
            }), true);
        }
        if (r == 1) e.punchEnabled = ! e.punchEnabled.load();
        if (r == 2) { e.punchIn = s; if (e.punchOut.load() <= s) e.punchOut = s + (juce::int64) (4.0 * 60.0 / e.bpm.load() * e.getSampleRate()); e.punchEnabled = true; }
        if (r == 3) { e.punchOut = s; if (e.punchIn.load() < 0 || e.punchIn.load() >= s) e.punchIn = juce::jmax<juce::int64> (0, s - (juce::int64) (4.0 * 60.0 / e.bpm.load() * e.getSampleRate())); e.punchEnabled = true; }
        if (r == 4) { e.punchIn = -1; e.punchOut = -1; e.punchEnabled = false; }
        safe->repaint();
    });
}

void addMarkerAt (AudioEngine& e, double seconds)
{
    // names count up: Verse 1, Hook 2 ... the user renames them as they like
    static const char* const names[] = { "Intro", "Verse", "Pre-hook", "Hook", "Verse", "Bridge", "Hook", "Outro" };
    juce::String name = e.markers.size() < 8 ? juce::String (names[e.markers.size()]) : "Marker " + juce::String ((int) e.markers.size() + 1);
    e.markers.push_back ({ juce::jmax (0.0, seconds), name });
    std::sort (e.markers.begin(), e.markers.end(), [] (auto& a, auto& b) { return a.seconds < b.seconds; });
}

void RulerComponent::seekTo (int x)
{
    auto lane = getLocalBounds().withTrimmedLeft (kHeaderWidth);
    const int clamped = juce::jlimit (lane.getX(), lane.getRight(), x); // keep following when dragged past the edge
    if (x >= lane.getX() - 40)
    {
        const double t = ctx.view.snap (ctx.view.xToTime ((float) clamped, lane), ctx.engine.bpm.load(), bypassSnap);
        ctx.engine.setTransportPosition ((juce::int64) std::llround (t * ctx.engine.getSampleRate()));
    }
}

void RulerComponent::timerCallback()
{
    // dragging the playhead into an edge keeps the timeline moving under it
    if (ctx.view.autoScroll ((float) dragX, getLocalBounds().withTrimmedLeft (kHeaderWidth)))
    {
        seekTo (dragX);
        if (auto* p = getParentComponent())
            p->repaint();
    }
}

void RulerComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (! handleTimelineWheel (ctx.view, e, w, getLocalBounds().withTrimmedLeft (kHeaderWidth)))
        ctx.view.scrollBy (-(w.deltaY != 0.0f ? w.deltaY : w.deltaX) * 0.25, getLocalBounds().withTrimmedLeft (kHeaderWidth)); // plain wheel over the ruler scrolls sideways
    repaint();
}

bool handleTimelineWheel (TimelineView& view, const juce::MouseEvent& e, const juce::MouseWheelDetails& w, juce::Rectangle<int> lane)
{
    if (e.mods.isCommandDown()) // Ctrl (Cmd on Mac) + wheel: zoom around the mouse
    {
        const float delta = w.deltaY != 0.0f ? w.deltaY : w.deltaX;
        if (delta != 0.0f)
            view.zoomAround (std::pow (1.25, delta / 0.15), (float) juce::jlimit (lane.getX(), lane.getRight(), e.x), lane);
        return true;
    }
    if (e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY)) // sideways
    {
        const float delta = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
        view.scrollBy (-delta * 0.25, lane);
        return true;
    }
    return false;
}

//==============================================================================
// Automation clip lane: an envelope of points. Click empty space to add a point (snaps to the grid,
// Shift = free), drag points to move them, right-click or double-click a point to delete it.

juce::Point<float> TrackComponent::automationPointPos (const AutomationPoint& p) const
{
    const auto lane = laneBounds().reduced (2, 3).toFloat().reduced (0.0f, 6.0f);
    return { ctx.view.timeToX (p.seconds, laneBounds()), lane.getBottom() - p.value * lane.getHeight() };
}

float TrackComponent::automationValueAtY (int y) const
{
    const auto lane = laneBounds().reduced (2, 3).toFloat().reduced (0.0f, 6.0f);
    return juce::jlimit (0.0f, 1.0f, (lane.getBottom() - (float) y) / juce::jmax (1.0f, lane.getHeight()));
}

int TrackComponent::automationPointAt (juce::Point<int> p) const
{
    const auto& pts = track.automation->points;
    for (int i = (int) pts.size(); --i >= 0;)
        if (automationPointPos (pts[(size_t) i]).getDistanceFrom (p.toFloat()) <= 7.0f)
            return i;
    return -1;
}

void TrackComponent::paintAutomation (juce::Graphics& g)
{
    auto& a = *track.automation;
    const bool active = ! track.mute.load() && a.target != nullptr;

    auto header = getLocalBounds().withWidth (kHeaderWidth).toFloat().reduced (3.0f, 2.0f);
    Glass::panel (g, header, 8.0f, 1.0f);
    g.setGradientFill (juce::ColourGradient (Theme::monitor, header.getX(), header.getY(), Theme::accent2, header.getX(), header.getBottom(), false));
    g.fillRoundedRectangle (header.withWidth (3.0f).reduced (0.0f, 8.0f), 1.5f);

    // target chip under the name
    {
        auto chip = juce::Rectangle<float> (menuBtn.getRight() + 6.0f, (float) menuBtn.getY() + 2.0f,
                                            header.getRight() - (float) menuBtn.getRight() - 14.0f, (float) menuBtn.getHeight() - 4.0f);
        g.setColour (Theme::monitor.withAlpha (0.18f));
        g.fillRoundedRectangle (chip, 4.0f);
        g.setColour (Theme::monitor.brighter (0.2f));
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawFittedText ("AUTO  " + AudioEngine::describeTarget (a), chip.reduced (4.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    }

    const auto lane = laneBounds();
    Glass::well (g, lane.toFloat().reduced (2.0f, 3.0f), 10.0f);
    juce::Graphics::ScopedSaveState s (g);
    g.reduceClipRegion (lane.reduced (2, 3));
    drawBarGrid (g, ctx.view, lane, ctx.engine.bpm.load(), false);

    const auto col = active ? Theme::monitor : Theme::textDim;
    const auto& pts = a.points;
    if (pts.empty())
        return;

    const auto inner = lane.reduced (2, 3).toFloat();
    juce::Path line;
    const auto first = automationPointPos (pts.front());
    line.startNewSubPath (inner.getX(), first.y);
    line.lineTo (first);
    for (size_t i = 1; i < pts.size(); ++i)
        line.lineTo (automationPointPos (pts[i]));
    line.lineTo (inner.getRight(), automationPointPos (pts.back()).y);

    juce::Path fill (line);
    fill.lineTo (inner.getRight(), inner.getBottom());
    fill.lineTo (inner.getX(), inner.getBottom());
    fill.closeSubPath();
    g.setGradientFill (juce::ColourGradient (col.withAlpha (0.28f), 0.0f, inner.getY(), col.withAlpha (0.03f), 0.0f, inner.getBottom(), false));
    g.fillPath (fill);
    g.setColour (col);
    g.strokePath (line, juce::PathStrokeType (1.8f));

    for (int i = 0; i < (int) pts.size(); ++i)
    {
        const auto p = automationPointPos (pts[(size_t) i]);
        const float r = (i == autoHoverIndex || i == autoDragIndex) ? 5.5f : 4.0f;
        g.setColour (Theme::background);
        g.fillEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (p));
        g.setColour (col.brighter (i == autoDragIndex ? 0.5f : 0.0f));
        g.drawEllipse (juce::Rectangle<float> (r * 2, r * 2).withCentre (p), 1.8f);
    }

    const auto now = juce::Time::getMillisecondCounter();
    if (now < flashUntil)
    {
        const float t = (float) (flashUntil - now) / 900.0f;
        g.setColour (Theme::monitor.withAlpha (0.25f * t));
        g.fillRoundedRectangle (inner, 10.0f);
    }

    if (autoDragIndex >= 0 && autoDragIndex < (int) pts.size())
    {
        const auto& p = pts[(size_t) autoDragIndex];
        const auto pos = automationPointPos (p);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (juce::String (juce::roundToInt (p.value * 100.0f)) + "%",
                    juce::Rectangle<float> (60.0f, 16.0f).withCentre ({ pos.x, pos.y - 14.0f }).toNearestInt(), juce::Justification::centred);
    }
}

void TrackComponent::automationMouseDown (const juce::MouseEvent& e)
{
    autoDragIndex = -1;
    if (! isInLane (e.getPosition()))
        return;

    auto& pts = track.automation->points;
    const int hit = automationPointAt (e.getPosition());

    if (e.mods.isPopupMenu())
    {
        if (hit >= 0 && pts.size() > 1)
        {
            const juce::ScopedLock sl (ctx.engine.getLock());
            pts.erase (pts.begin() + hit);
            autoHoverIndex = -1;
        }
        else if (hit < 0)
            showAutomationMenu();
        repaint();
        return;
    }

    if (hit >= 0)
    {
        autoDragIndex = hit;
        return;
    }

    // add a point where you clicked
    const double t = juce::jmax (0.0, ctx.view.snap (timeAtX (e.x), ctx.engine.bpm.load(), e.mods.isShiftDown()));
    const AutomationPoint p { t, automationValueAtY (e.y) };
    {
        const juce::ScopedLock sl (ctx.engine.getLock());
        auto it = std::upper_bound (pts.begin(), pts.end(), t, [] (double sec, const AutomationPoint& q) { return sec < q.seconds; });
        autoDragIndex = (int) (pts.insert (it, p) - pts.begin());
    }
    repaint();
}

void TrackComponent::automationMouseDrag (const juce::MouseEvent& e)
{
    auto& pts = track.automation->points;
    if (autoDragIndex < 0 || autoDragIndex >= (int) pts.size())
        return;

    double t = juce::jmax (0.0, ctx.view.snap (timeAtX (e.x), ctx.engine.bpm.load(), e.mods.isShiftDown()));
    {
        const juce::ScopedLock sl (ctx.engine.getLock());
        // stay between the neighbours so the list stays sorted
        if (autoDragIndex > 0)                    t = juce::jmax (t, pts[(size_t) autoDragIndex - 1].seconds);
        if (autoDragIndex + 1 < (int) pts.size()) t = juce::jmin (t, pts[(size_t) autoDragIndex + 1].seconds);
        pts[(size_t) autoDragIndex] = { t, automationValueAtY (e.y) };
    }
    repaint();
}

void TrackComponent::automationMouseMove (const juce::MouseEvent& e)
{
    const int h = isInLane (e.getPosition()) ? automationPointAt (e.getPosition()) : -1;
    setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor
                           : (isInLane (e.getPosition()) ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor));
    if (h != autoHoverIndex)
    {
        autoHoverIndex = h;
        repaint();
    }
}

void TrackComponent::automationDoubleClick (const juce::MouseEvent& e)
{
    auto& pts = track.automation->points;
    const int hit = automationPointAt (e.getPosition());
    if (hit >= 0 && pts.size() > 1)
    {
        const juce::ScopedLock sl (ctx.engine.getLock());
        pts.erase (pts.begin() + hit);
        autoDragIndex = autoHoverIndex = -1;
        repaint();
    }
}

void TrackComponent::showAutomationMenu()
{
    auto& a = *track.automation;
    juce::PopupMenu m;
    m.addSectionHeader (AudioEngine::describeTarget (a));
    m.addItem (1, "Automation on", true, ! track.mute.load());
    m.addSeparator();
    m.addItem (2, "Flatten to the control's current value", a.target != nullptr);
    m.addItem (3, "Ramp up (0% to 100%)");
    m.addItem (4, "Ramp down (100% to 0%)");
    m.addItem (5, "Off for the last beat of every bar (Tape Stop drops)");
    m.addSeparator();
    m.addItem (6, "Delete automation clip");

    juce::Component::SafePointer<TrackComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuBtn), [safe] (int r)
    {
        if (safe == nullptr || r == 0)
            return;
        auto& t = safe->track;
        auto& au = *t.automation;
        auto& engine = safe->ctx.engine;
        const double sr = juce::jmax (1.0, engine.getSampleRate());
        const double end = juce::jmax (8.0, (double) engine.getSessionLengthSamples() / sr);
        std::vector<AutomationPoint> pts;
        switch (r)
        {
            case 1: t.mute = ! t.mute.load(); break;
            case 2: { const float v = AudioEngine::getControlValue (au); pts = { { 0.0, v }, { end, v } }; break; }
            case 3: pts = { { 0.0, 0.0f }, { end, 1.0f } }; break;
            case 4: pts = { { 0.0, 1.0f }, { end, 0.0f } }; break;
            case 5:
            {
                const double bar = 4.0 * 60.0 / juce::jmax (20.0, engine.bpm.load());
                for (double sec = 0.0; sec < end; sec += bar)
                {
                    pts.push_back ({ sec, 1.0f });
                    pts.push_back ({ sec + bar * 0.75, 1.0f });
                    pts.push_back ({ sec + bar * 0.75 + 0.001, 0.0f });
                    pts.push_back ({ sec + bar - 0.001, 0.0f });
                }
                break;
            }
            case 6: if (safe->ctx.removeTrack) safe->ctx.removeTrack (&t); return;
            default: break;
        }
        if (! pts.empty())
        {
            const juce::ScopedLock sl (engine.getLock());
            au.points = std::move (pts);
        }
        safe->repaint();
    });
}
