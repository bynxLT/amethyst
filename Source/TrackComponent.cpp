#include "TrackComponent.h"
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

    juce::PopupMenu through;
    through.addItem (2000, "Its own mixer channel", true, track.playThrough == nullptr);
    through.addSeparator();
    {
        int i = 0;
        for (auto* other : ctx.engine.getTracks())
        {
            if (other != &track && other->playThrough == nullptr)
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
    m.addSeparator();
    m.addItem (7, "Delete track");

    juce::Component::SafePointer<TrackComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuBtn), [safe] (int r)
    {
        if (safe == nullptr || r == 0)
            return;
        auto& t = safe->track;
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
    // Compact list row
    auto header = getLocalBounds().withWidth (kHeaderWidth).toFloat().reduced (3.0f, 2.0f);
    Glass::panel (g, header, 8.0f, track.armed ? 1.4f : 1.0f);
    g.setGradientFill (juce::ColourGradient (track.armed ? Theme::record : Theme::accent, header.getX(), header.getY(),
                                             track.armed ? Theme::record.darker() : Theme::accent2, header.getX(), header.getBottom(), false));
    g.fillRoundedRectangle (header.withWidth (3.0f).reduced (0.0f, 8.0f), 1.5f);

    // status chips under the name: input, monitoring, solo, plug-ins
    {
        juce::StringArray chips;
        const juce::String arrow (juce::CharPointer_UTF8 ("\xe2\x86\x92 "));
        if (track.playThrough != nullptr)
            chips.add (arrow + track.playThrough->name.substring (0, 12));
        if (inputBox.getNumItems() > 0)
            chips.add (inputBox.getSelectedId() >= 1001 ? "ST " + juce::String (track.inputChannel.load() + 1)
                                                        : "IN " + juce::String (track.inputChannel.load() + 1));
        if (track.monitor.load()) chips.add ("MON");
        if (track.solo.load())    chips.add ("SOLO");
        if (const int n = track.countPlugins()) chips.add ("FX " + juce::String (n));

        auto chipArea = juce::Rectangle<float> (menuBtn.getRight() + 6.0f, (float) menuBtn.getY(), header.getRight() - menuBtn.getRight() - 24.0f, (float) menuBtn.getHeight());
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
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
        const auto base = clip.colour.isTransparent() ? Theme::clip : clip.colour;
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
            it->second.thumbnail->drawChannels (g, waveArea.toNearestInt(), t0, t1, 1.0f);
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

        g.setColour (Theme::text.withAlpha (0.9f * alpha));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        juce::String label = clip.getDisplayName();
        if (! clip.settings.isNeutral())
            label << "  *fx";
        if (ctx.engine.isPreviewing() && ctx.engine.isPlaying())
            label << "  (preview)";
        g.drawText (label, visible.reduced (7.0f, 3.0f).toNearestInt(), juce::Justification::topLeft, true);
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
    dragMode = Drag::None;
    if (! isInLane (e.getPosition()))
        return;

    auto clip = clipAt (e.getPosition());

    if (e.mods.isPopupMenu())
    {
        if (clip != nullptr) showClipMenu (clip, sampleAtX (e.x, e.mods.isShiftDown()));
        else                 showLaneMenu();
        return;
    }

    if (clip == nullptr)
    {
        ctx.engine.setTransportPosition (sampleAtX (e.x, e.mods.isShiftDown()));
        return;
    }

    switch (ctx.view.tool)
    {
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
    if (ctx.checkpoint)
        ctx.checkpoint (dragMode == Drag::Move ? "Move clip" : "Trim clip");
}

void TrackComponent::mouseDrag (const juce::MouseEvent& e)
{
    auto clip = draggingClip.lock();
    if (clip == nullptr)
        return;

    const bool bypassSnap = e.mods.isShiftDown();

    switch (dragMode)
    {
        case Drag::TrimStart: ctx.engine.trimClipStart (*clip, sampleAtX (e.x, bypassSnap)); break;
        case Drag::TrimEnd:   ctx.engine.trimClipEnd (*clip, sampleAtX (e.x, bypassSnap));   break;
        case Drag::Move:
        {
            const double sr = juce::jmax (1.0, ctx.engine.getSampleRate());
            const double newStart = ctx.view.snap (timeAtX (e.x) - dragGrabOffsetSec, ctx.engine.bpm.load(), bypassSnap);
            clip->startSample = (juce::int64) std::llround (juce::jmax (0.0, newStart) * sr);
            break;
        }
        case Drag::None: break;
    }
    repaint();
}

void TrackComponent::mouseUp (const juce::MouseEvent& e)
{
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
            case EditTool::Trim:
                cursorType = juce::MouseCursor::LeftRightResizeCursor;
                edgeClip = clip.get();
                edgeSide = (float) p.x < clipBounds (*clip).getCentreX() ? -1 : 1;
                break;
            case EditTool::Move:
            default:
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
    updateCursor (e.getPosition());
}

void TrackComponent::mouseExit (const juce::MouseEvent&)
{
    hoverX = -1.0f;
    hoverEdgeClip = nullptr;
    hoverEdgeSide = 0;
    repaint();
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
        idReverse, idNormalize, idDetectTempo, idFitTempo, idResetFades, idLayer, idNewest,
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
            copy->source = c->source; copy->buffer = c->buffer; copy->file = c->file;
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
            juce::Component::SafePointer<TrackComponent> safe (this);
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Detected tempo",
                                                "This clip sounds like about " + juce::String (bpm, 1) + " BPM.\n\nSet the project tempo to "
                                                    + juce::String (juce::roundToInt (bpm)) + " BPM?",
                                                "Set tempo", "Close", nullptr,
                                                juce::ModalCallbackFunction::create ([safe, bpm] (int ok)
            {
                if (safe != nullptr && ok == 1)
                    safe->ctx.engine.bpm = (double) juce::roundToInt (bpm);
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
    const float px = ctx.view.timeToX ((double) ctx.engine.getTransportPosition() / sr, lane);
    juce::Path tri;
    tri.addTriangle (px - 6.0f, 0.0f, px + 6.0f, 0.0f, px, 10.0f);
    g.setColour (ctx.engine.isRecording() ? Theme::record : Theme::playhead);
    g.fillPath (tri);
    g.drawVerticalLine ((int) px, 0.0f, (float) getHeight());
}

void RulerComponent::seek (const juce::MouseEvent& e)
{
    auto lane = getLocalBounds().withTrimmedLeft (kHeaderWidth);
    if (e.x >= lane.getX())
    {
        const double t = ctx.view.snap (ctx.view.xToTime ((float) e.x, lane), ctx.engine.bpm.load(), e.mods.isShiftDown());
        ctx.engine.setTransportPosition ((juce::int64) std::llround (t * ctx.engine.getSampleRate()));
    }
}
