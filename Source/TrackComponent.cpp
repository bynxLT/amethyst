#include "TrackComponent.h"
#include "PluginWindows.h"

namespace
{
    void drawBarGrid (juce::Graphics& g, const TimelineView& view, juce::Rectangle<int> lane, double bpm, bool labels)
    {
        const double barSec = 240.0 / juce::jmax (20.0, bpm);
        int step = 1;
        while (barSec * step * view.pixelsPerSecond < 48.0)
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

            g.setColour (labels ? Theme::text.withAlpha (0.35f) : Theme::grid);
            g.drawVerticalLine ((int) x, (float) lane.getY(), (float) lane.getBottom());

            if (labels)
            {
                g.setColour (Theme::text.withAlpha (0.8f));
                g.drawText (juce::String (bar + 1), (int) x + 3, lane.getY(), 40, lane.getHeight(),
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

    m.addSeparator();
    const int nFx = track.countPlugins();
    m.addItem (5, "Plug-ins in mixer..." + (nFx > 0 ? "  (" + juce::String (nFx) + ")" : juce::String()));
    m.addItem (6, "Channel presets...");
    juce::PopupMenu takes;
    takes.addItem (8, "Layer - hear every take", true, track.layerTakes.load());
    takes.addItem (9, "Newest take on top", true, ! track.layerTakes.load());
    m.addSubMenu ("Overlapping takes", takes);
    m.addSeparator();
    m.addItem (7, "Delete track");

    juce::Component::SafePointer<TrackComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuBtn), [safe] (int r)
    {
        if (safe == nullptr || r == 0)
            return;
        auto& t = safe->track;
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
            const auto col = c == "MON" ? Theme::monitor : c == "SOLO" ? Theme::solo : Theme::accent;
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

        // liquid glass clip body
        g.setGradientFill (juce::ColourGradient (Theme::clip.withAlpha ((isDragged ? 0.75f : 0.55f) * alpha), visible.getX(), visible.getY(),
                                                 juce::Colour (0xff3b2475).withAlpha (0.65f * alpha), visible.getX(), visible.getBottom(), false));
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
            g.setColour (Theme::wave.withAlpha (0.92f * alpha));
            it->second.thumbnail->drawChannels (g, visible.reduced (0.0f, 8.0f).withTrimmedTop (6.0f).toNearestInt(), t0, t1, 1.0f);
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
        juce::String label = clip.file.getFileNameWithoutExtension();
        if (! clip.settings.isNeutral())
            label << "  *fx";
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

juce::int64 TrackComponent::sampleAtX (int x, bool snap) const
{
    double t = timeAtX (x);
    if (snap) // Shift snaps to beats
    {
        const double beat = 60.0 / juce::jmax (20.0, ctx.engine.bpm.load());
        t = std::round (t / beat) * beat;
    }
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
        if (clip != nullptr) showClipMenu (clip, sampleAtX (e.x, false));
        else                 showLaneMenu();
        return;
    }

    if (clip == nullptr)
    {
        ctx.engine.setTransportPosition ((juce::int64) (timeAtX (e.x) * ctx.engine.getSampleRate()));
        return;
    }

    switch (ctx.view.tool)
    {
        case EditTool::Cut:
            splitAt (clip, sampleAtX (e.x, e.mods.isShiftDown()));
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
}

void TrackComponent::mouseDrag (const juce::MouseEvent& e)
{
    auto clip = draggingClip.lock();
    if (clip == nullptr)
        return;

    const bool snap = e.mods.isShiftDown();

    switch (dragMode)
    {
        case Drag::TrimStart: ctx.engine.trimClipStart (*clip, sampleAtX (e.x, snap)); break;
        case Drag::TrimEnd:   ctx.engine.trimClipEnd (*clip, sampleAtX (e.x, snap));   break;
        case Drag::Move:
        {
            const double sr = juce::jmax (1.0, ctx.engine.getSampleRate());
            double newStart = timeAtX (e.x) - dragGrabOffsetSec;
            if (snap)
            {
                const double beat = 60.0 / juce::jmax (20.0, ctx.engine.bpm.load());
                newStart = std::round (newStart / beat) * beat;
            }
            clip->startSample = (juce::int64) std::llround (juce::jmax (0.0, newStart) * sr);
            break;
        }
        case Drag::None: break;
    }
    repaint();
}

void TrackComponent::mouseUp (const juce::MouseEvent& e)
{
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

void TrackComponent::showClipMenu (std::shared_ptr<AudioClip> clip, juce::int64 clickSample)
{
    const auto playhead = ctx.engine.getTransportPosition();
    const bool playheadInside = playhead > clip->startSample.load() && playhead < clip->getEnd();
    const bool trimmed = clip->getOffset() > 0 || clip->trimLength.load() >= 0;

    juce::PopupMenu m;
    m.addSectionHeader (clip->file.getFileName());
    m.addItem (4, "Clip properties...  (double-click)");
    m.addItem (5, "Enabled", true, clip->enabled.load());
    m.addSeparator();
    m.addItem (6, "Split here");
    m.addItem (7, "Split at playhead", playheadInside);
    m.addItem (8, "Trim start to playhead", playheadInside);
    m.addItem (9, "Trim end to playhead", playheadInside);
    m.addItem (10, "Restore full length (undo trims)", trimmed);
    m.addSeparator();
    m.addItem (1, "Delete clip");
    m.addItem (2, "Show file in Explorer");
    m.addItem (3, "Move to playhead");
    m.addSeparator();
    juce::PopupMenu takes;
    takes.addItem (11, "Layer - hear every take", true, track.layerTakes.load());
    takes.addItem (12, "Newest take on top", true, ! track.layerTakes.load());
    m.addSubMenu ("Overlapping takes", takes);

    std::weak_ptr<AudioClip> weak = clip;
    juce::Component::SafePointer<TrackComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, weak, clickSample, playhead] (int result)
    {
        auto c = weak.lock();
        if (safe == nullptr || c == nullptr)
            return;
        auto& engine = safe->ctx.engine;

        switch (result)
        {
            case 1:  engine.removeClip (safe->track, c.get()); break;
            case 2:  c->file.revealToUser(); break;
            case 3:  c->startSample = playhead; break;
            case 4:  if (safe->ctx.openClipEditor) safe->ctx.openClipEditor (&safe->track, c); break;
            case 5:  c->enabled = ! c->enabled.load(); break;
            case 6:  safe->splitAt (c, clickSample); break;
            case 7:  safe->splitAt (c, playhead); break;
            case 8:  engine.trimClipStart (*c, playhead); break;
            case 9:  engine.trimClipEnd (*c, playhead); break;
            case 10:
            {
                const auto originalStart = c->startSample.load() - c->getOffset();
                engine.trimClipStart (*c, originalStart);
                engine.trimClipEnd (*c, originalStart + c->getBufferLength());
                break;
            }
            case 11: safe->track.layerTakes = true; break;
            case 12: safe->track.layerTakes = false; break;
            default: break;
        }
        safe->repaint();
    });
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
        ctx.engine.setTransportPosition ((juce::int64) (ctx.view.xToTime ((float) e.x, lane) * ctx.engine.getSampleRate()));
}
