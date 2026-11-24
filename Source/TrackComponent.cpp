#include "TrackComponent.h"
#include "PluginWindows.h"

namespace
{
    void styleToggle (juce::TextButton& b, juce::Colour on, const juce::String& tip)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonOnColourId, on);
        b.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        b.setTooltip (tip);
    }

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
    nameLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    nameLabel.setTooltip ("Double-click to rename");
    nameLabel.onTextChange = [this]
    {
        track.name = nameLabel.getText().trim().isEmpty() ? "Track" : nameLabel.getText().trim();
    };
    addAndMakeVisible (nameLabel);

    styleToggle (armBtn, Theme::record, "Arm for recording");
    styleToggle (monBtn, Theme::monitor, "Input monitoring (hear yourself through the FX chain)");
    styleToggle (muteBtn, Theme::mute, "Mute");
    styleToggle (soloBtn, Theme::solo, "Solo");

    armBtn.onClick  = [this] { track.armed = armBtn.getToggleState(); };
    monBtn.onClick  = [this] { track.monitor = monBtn.getToggleState(); };
    muteBtn.onClick = [this] { track.mute = muteBtn.getToggleState(); };
    soloBtn.onClick = [this] { track.solo = soloBtn.getToggleState(); };

    fxBtn.setTooltip ("Show this channel's plug-in slots in the mixer");
    fxBtn.onClick = [this] { if (ctx.showInMixer) ctx.showInMixer (&track); };

    presetBtn.setTooltip ("Save or load a channel preset");
    presetBtn.onClick = [this] { UiHelpers::showPresetMenu (ctx, track, presetBtn, {}); };

    removeBtn.setTooltip ("Delete track");
    removeBtn.onClick = [this] { if (ctx.removeTrack) ctx.removeTrack (&track); };

    for (auto* b : { &armBtn, &monBtn, &muteBtn, &soloBtn, &fxBtn, &presetBtn, &removeBtn })
        addAndMakeVisible (b);

    inputBox.setTooltip ("Recording input");
    inputBox.setTextWhenNoChoicesAvailable ("No inputs enabled");
    inputBox.onChange = [this] { UiHelpers::applyInputSelection (inputBox, track); };
    addAndMakeVisible (inputBox);

    volume.setSliderStyle (juce::Slider::LinearHorizontal);
    volume.setRange (-60.0, 12.0, 0.1);
    volume.setSkewFactorFromMidPoint (-12.0);
    volume.setDoubleClickReturnValue (true, 0.0);
    volume.setTextValueSuffix (" dB");
    volume.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 20);
    volume.onValueChange = [this] { track.volumeDb = (float) volume.getValue(); };
    addAndMakeVisible (volume);

    pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    pan.setRange (-1.0, 1.0, 0.01);
    pan.setDoubleClickReturnValue (true, 0.0);
    pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    pan.setTooltip ("Pan (double-click to centre)");
    pan.onValueChange = [this] { track.pan = (float) pan.getValue(); };
    addAndMakeVisible (pan);

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
    armBtn.setToggleState (track.armed.load(), juce::dontSendNotification);
    monBtn.setToggleState (track.monitor.load(), juce::dontSendNotification);
    muteBtn.setToggleState (track.mute.load(), juce::dontSendNotification);
    soloBtn.setToggleState (track.solo.load(), juce::dontSendNotification);
    if (! volume.isMouseButtonDown()) volume.setValue (track.volumeDb.load(), juce::dontSendNotification);
    if (! pan.isMouseButtonDown())    pan.setValue (track.pan.load(), juce::dontSendNotification);

    const int n = track.countPlugins();
    const auto fxText = n == 0 ? juce::String ("FX") : "FX " + juce::String (n);
    if (fxBtn.getButtonText() != fxText)
        fxBtn.setButtonText (fxText);

    repaint();
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
    // Header card
    auto header = getLocalBounds().withWidth (kHeaderWidth).toFloat().reduced (4.0f, 3.0f);
    Glass::panel (g, header, 12.0f);
    if (track.armed)
    {
        g.setColour (Theme::record.withAlpha (0.35f));
        g.fillRoundedRectangle (header.withWidth (8.0f).translated (-1.0f, 0.0f).reduced (0.0f, 10.0f), 3.0f);
    }
    g.setGradientFill (juce::ColourGradient (track.armed ? Theme::record : Theme::accent, header.getX(), header.getY(),
                                             track.armed ? Theme::record.darker() : Theme::accent2, header.getX(), header.getBottom(), false));
    g.fillRoundedRectangle (header.withWidth (4.0f).reduced (0.0f, 12.0f), 2.0f);

    auto meterArea = getLocalBounds().withWidth (kHeaderWidth).removeFromRight (24).reduced (4, 12).toFloat();
    drawMeter (g, meterArea.removeFromLeft (6), levelL);
    meterArea.removeFromLeft (2);
    drawMeter (g, meterArea.removeFromLeft (6), levelR);

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
            const double t0 = (vx0 - full.getX()) / ctx.view.pixelsPerSecond;
            const double t1 = (vx1 - full.getX()) / ctx.view.pixelsPerSecond;
            g.setColour (Theme::wave.withAlpha (0.92f * alpha));
            it->second.thumbnail->drawChannels (g, visible.reduced (0.0f, 8.0f).withTrimmedTop (6.0f).toNearestInt(), t0, t1, 1.0f);
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

    const float px = ctx.view.timeToX ((double) ctx.engine.getTransportPosition() / sr, lane);
    g.setColour (Theme::accent2.withAlpha (0.25f));
    g.fillRect (juce::Rectangle<float> (px - 2.0f, (float) lane.getY(), 5.0f, (float) lane.getHeight()));
    g.setColour (Theme::playhead);
    g.drawVerticalLine ((int) px, (float) lane.getY(), (float) lane.getBottom());
}

void TrackComponent::resized()
{
    auto r = getLocalBounds().withWidth (kHeaderWidth).reduced (10, 8);
    r.removeFromRight (20); // meter

    auto row1 = r.removeFromTop (26);
    removeBtn.setBounds (row1.removeFromRight (26));
    row1.removeFromRight (6);
    presetBtn.setBounds (row1.removeFromRight (58));
    row1.removeFromRight (4);
    fxBtn.setBounds (row1.removeFromRight (46));
    row1.removeFromRight (8);
    for (auto* b : { &soloBtn, &muteBtn, &monBtn, &armBtn })
    {
        b->setBounds (row1.removeFromRight (26));
        row1.removeFromRight (3);
    }
    nameLabel.setBounds (row1);

    r.removeFromTop (8);
    auto row2 = r.removeFromTop (28);
    pan.setBounds (row2.removeFromRight (34));
    row2.removeFromRight (6);
    inputBox.setBounds (row2.removeFromLeft (150));
    row2.removeFromLeft (8);
    volume.setBounds (row2);
}

//==============================================================================
void TrackComponent::mouseDown (const juce::MouseEvent& e)
{
    if (! isInLane (e.getPosition()))
        return;

    if (auto clip = clipAt (e.getPosition()))
    {
        if (e.mods.isPopupMenu())
        {
            showClipMenu (clip);
            return;
        }

        draggingClip = clip;
        const double sr = juce::jmax (1.0, ctx.engine.getSampleRate());
        dragGrabOffsetSec = timeAtX (e.x) - (double) clip->startSample.load() / sr;
        return;
    }

    if (! e.mods.isPopupMenu())
        ctx.engine.setTransportPosition ((juce::int64) (timeAtX (e.x) * ctx.engine.getSampleRate()));
}

void TrackComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (auto clip = draggingClip.lock())
    {
        const double sr = juce::jmax (1.0, ctx.engine.getSampleRate());
        double newStart = ctx.view.startSeconds + ((double) e.x - laneBounds().getX()) / ctx.view.pixelsPerSecond - dragGrabOffsetSec;

        // Hold Shift to snap to beats
        if (e.mods.isShiftDown())
        {
            const double beat = 60.0 / juce::jmax (20.0, ctx.engine.bpm.load());
            newStart = std::round (newStart / beat) * beat;
        }

        clip->startSample = (juce::int64) std::llround (juce::jmax (0.0, newStart) * sr);
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        repaint();
    }
}

void TrackComponent::mouseUp (const juce::MouseEvent&)
{
    draggingClip.reset();
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void TrackComponent::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (clipAt (e.getPosition()) != nullptr ? juce::MouseCursor::DraggingHandCursor
                                                         : juce::MouseCursor::NormalCursor);
}

void TrackComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (auto clip = clipAt (e.getPosition()))
        if (ctx.openClipEditor)
            ctx.openClipEditor (&track, clip);
}

void TrackComponent::showClipMenu (std::shared_ptr<AudioClip> clip)
{
    juce::PopupMenu m;
    m.addSectionHeader (clip->file.getFileName());
    m.addItem (4, "Clip properties...  (double-click)");
    m.addItem (5, "Enabled", true, clip->enabled.load());
    m.addSeparator();
    m.addItem (1, "Delete clip");
    m.addItem (2, "Show file in Explorer");
    m.addItem (3, "Move to playhead");

    std::weak_ptr<AudioClip> weak = clip;
    juce::Component::SafePointer<TrackComponent> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, weak] (int result)
    {
        auto c = weak.lock();
        if (safe == nullptr || c == nullptr)
            return;

        if (result == 1)
            safe->ctx.engine.removeClip (safe->track, c.get());
        else if (result == 2)
            c->file.revealToUser();
        else if (result == 3)
            c->startSample = safe->ctx.engine.getTransportPosition();
        else if (result == 4 && safe->ctx.openClipEditor)
            safe->ctx.openClipEditor (&safe->track, c);
        else if (result == 5)
            c->enabled = ! c->enabled.load();
    });
}

//==============================================================================
void RulerComponent::paint (juce::Graphics& g)
{
    auto lane = getLocalBounds().withTrimmedLeft (kHeaderWidth);
    Glass::panel (g, lane.toFloat().reduced (2.0f, 2.0f), 8.0f, 0.8f);

    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("ZOOM", getLocalBounds().withWidth (kHeaderWidth).reduced (16, 0), juce::Justification::centredLeft);
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
