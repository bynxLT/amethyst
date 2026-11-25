#include "Studio2026.h"
#include "Presets.h"
#include "Exporter.h"
#include "Loudness.h"
#include "TrackComponent.h"
#include "Plugins/Builtins.h"

namespace
{
    juce::String keyLabel (const juce::String& k) { return k; }

    double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

    juce::PopupMenu::Item actionItem (const juce::String& text, std::function<void()> fn, bool ticked = false, bool enabled = true)
    {
        static int nextId = 700000;   // high ids: every menu handler here ignores them; the action does the work
        juce::PopupMenu::Item it (text);
        it.setID (++nextId).setTicked (ticked).setEnabled (enabled).setAction (std::move (fn));
        if (nextId > 790000) nextId = 700000;
        return it;
    }

    juce::String tierSuffix (int need, int have) { return have >= need ? juce::String() : need >= 2 ? juce::String ("   (Ultra Pro)") : juce::String ("   (Pro)"); }

    std::unique_ptr<AmethystPlugin> makeBuiltin (AudioEngine& engine, const char* id)
    {
        for (auto& d : Builtins::getDescriptions())
            if (d.fileOrIdentifier == id)
            {
                juce::String err;
                auto p = engine.createPlugin (d, err);
                return std::unique_ptr<AmethystPlugin> (dynamic_cast<AmethystPlugin*> (p.release()));
            }
        return nullptr;
    }

    std::shared_ptr<AudioClip> copyOf (const AudioClip& c)
    {
        auto n = std::make_shared<AudioClip>();
        n->source = c.source; n->buffer = c.buffer; n->file = c.file; n->lossless = c.lossless;
        n->settings = c.settings; n->name = c.name; n->colour = c.colour; n->enabled = c.enabled.load();
        n->startSample = c.startSample.load(); n->trimOffset = c.trimOffset.load(); n->trimLength = c.trimLength.load();
        return n;
    }

    // Cuts [from, to) out of a track's clips (splitting the ones that cross the edges).
    void removeRange (AudioEngine& e, Track& t, juce::int64 from, juce::int64 to)
    {
        auto clips = t.clips;   // copy: the list changes as we split
        for (auto& c : clips)
        {
            const auto s = c->startSample.load(), end = c->getEnd();
            if (end <= from || s >= to) continue;
            std::shared_ptr<AudioClip> middle = c;
            if (s < from)
                if (auto right = e.splitClip (t, c, from)) middle = right;
            if (middle->getEnd() > to)
                e.splitClip (t, middle, to);
            if (middle->startSample.load() >= from && middle->getEnd() <= to + 1)
                e.removeClip (t, middle.get());
        }
    }

    bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& b, double sr, int numChannels = 2)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (file.createOutputStream());
        if (os == nullptr)
            return false;
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, (unsigned int) numChannels, 24, {}, 0));
        if (w == nullptr)
            return false;
        os.release();
        return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    // Background job with a progress window (renders run off the message thread).
    class Job : public juce::ThreadWithProgressWindow
    {
    public:
        Job (const juce::String& title, juce::Component* centre, std::function<juce::Result (Job&)> work, std::function<void (juce::Result)> done)
            : juce::ThreadWithProgressWindow (title, true, true, 10000, "Cancel", centre), fn (std::move (work)), onDone (std::move (done)) {}
        void run() override { result = fn (*this); }
        void threadComplete (bool cancelled) override
        {
            if (onDone) onDone (cancelled ? juce::Result::fail ("Cancelled.") : result);
            delete this;
        }
        bool progress (float p) { setProgress (p); return ! threadShouldExit(); }
        void status (const juce::String& s) { setStatusMessage (s); }
    private:
        std::function<juce::Result (Job&)> fn;
        std::function<void (juce::Result)> onDone;
        juce::Result result { juce::Result::ok() };
    };
}

//==============================================================================
struct Studio2026::Toast
{
    juce::String text;
    bool warning = false;
    double born = 0.0;
    float alpha() const
    {
        const double age = nowSeconds() - born;
        if (age < 0.15) return (float) (age / 0.15);
        if (age > 3.2) return (float) juce::jmax (0.0, 1.0 - (age - 3.2) / 0.5);
        return 1.0f;
    }
    bool expired() const { return nowSeconds() - born > 3.7; }
};

//==============================================================================
class Studio2026::Palette : public juce::Component, private juce::ListBoxModel, private juce::TextEditor::Listener, private juce::KeyListener
{
public:
    Palette (Studio2026& o) : owner (o)
    {
        search.setTextToShowWhenEmpty ("Type a command...  (Enter runs it, Esc closes)", Theme::textDim);
        search.setFont (juce::FontOptions (17.0f));
        search.addListener (this);
        search.addKeyListener (this);
        search.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black.withAlpha (0.3f));
        search.setColour (juce::TextEditor::outlineColourId, Theme::accent.withAlpha (0.5f));
        addAndMakeVisible (search);
        list.setModel (this);
        list.setRowHeight (30);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (list);
        filter();
    }

    void open()
    {
        search.setText ({}, false);
        filter();
        setVisible (true);
        toFront (true);
        search.grabKeyboardFocus();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Theme::windowBg.withAlpha (0.96f));
        g.fillRoundedRectangle (r, 14.0f);
        Glass::panel (g, r, 14.0f, 1.3f);
        g.setColour (Theme::accent.withAlpha (0.6f));
        g.drawRoundedRectangle (r.reduced (0.5f), 14.0f, 1.2f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);
        search.setBounds (r.removeFromTop (38));
        r.removeFromTop (8);
        list.setBounds (r);
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey) { setVisible (false); return true; }
        if (k == juce::KeyPress::returnKey) { runSelected(); return true; }
        if (k == juce::KeyPress::downKey) { list.selectRow (juce::jmin (getNumRows() - 1, list.getSelectedRow() + 1)); return true; }
        if (k == juce::KeyPress::upKey)   { list.selectRow (juce::jmax (0, list.getSelectedRow() - 1)); return true; }
        return false;
    }

private:
    bool keyPressed (const juce::KeyPress& k, juce::Component*) override { return keyPressed (k); }
    void textEditorTextChanged (juce::TextEditor&) override { filter(); }
    void textEditorReturnKeyPressed (juce::TextEditor&) override { runSelected(); }
    void textEditorEscapeKeyPressed (juce::TextEditor&) override { setVisible (false); }

    void filter()
    {
        matches.clear();
        const auto words = juce::StringArray::fromTokens (search.getText().toLowerCase(), " ", {});
        for (int i = 0; i < (int) owner.commands.size(); ++i)
        {
            const auto hay = (owner.commands[(size_t) i].name + " " + owner.commands[(size_t) i].group + " " + owner.commands[(size_t) i].keys).toLowerCase();
            bool ok = true;
            for (auto& w : words) ok = ok && hay.contains (w);
            if (ok) matches.push_back (i);
        }
        list.updateContent();
        list.selectRow (0);
        list.repaint();
    }

    void runSelected()
    {
        const int row = list.getSelectedRow();
        if (! juce::isPositiveAndBelow (row, (int) matches.size()))
            return;
        auto fn = owner.commands[(size_t) matches[(size_t) row]].run;
        setVisible (false);
        if (fn) juce::MessageManager::callAsync (fn);
    }

    int getNumRows() override { return (int) matches.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) matches.size())) return;
        const auto& c = owner.commands[(size_t) matches[(size_t) row]];
        if (selected)
        {
            g.setColour (Theme::accent.withAlpha (0.28f));
            g.fillRoundedRectangle (juce::Rectangle<float> (0, 1, (float) w, (float) h - 2).reduced (2.0f, 0.0f), 7.0f);
        }
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (c.group.toUpperCase(), 12, 0, 90, h, juce::Justification::centredLeft);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (14.5f));
        const int tier = owner.ctx.tier ? owner.ctx.tier() : 0;
        g.drawText (c.name + tierSuffix (c.tier, tier), 104, 0, w - 230, h, juce::Justification::centredLeft, true);
        if (c.keys.isNotEmpty())
        {
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            const float kw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), c.keys) + 14.0f;
            auto pill = juce::Rectangle<float> ((float) w - kw - 12.0f, (float) h * 0.5f - 10.0f, kw, 20.0f);
            g.setColour (Theme::panelLight);
            g.fillRoundedRectangle (pill, 5.0f);
            g.setColour (Theme::text.withAlpha (0.85f));
            g.drawText (c.keys, pill, juce::Justification::centred);
        }
    }
    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { runSelected(); }
    void returnKeyPressed (int) override { runSelected(); }

    Studio2026& owner;
    juce::TextEditor search;
    juce::ListBox list;
    std::vector<int> matches;
};

//==============================================================================
class Studio2026::ShortcutSheet : public juce::Component
{
public:
    explicit ShortcutSheet (Studio2026& o) : owner (o) {}
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Theme::windowBg.withAlpha (0.97f));
        g.fillRoundedRectangle (r, 16.0f);
        Glass::panel (g, r, 16.0f, 1.3f);
        g.setColour (Theme::accent.withAlpha (0.55f));
        g.drawRoundedRectangle (r.reduced (0.5f), 16.0f, 1.2f);

        auto area = getLocalBounds().reduced (24, 18);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        g.drawText ("Keyboard shortcuts", area.removeFromTop (30), juce::Justification::centredLeft);
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Ctrl+K opens the command palette for everything else.  Click or press Esc to close.", area.removeFromTop (20), juce::Justification::centredLeft);
        area.removeFromTop (8);

        std::vector<const Command*> withKeys;
        for (auto& c : owner.commands)
            if (c.keys.isNotEmpty()) withKeys.push_back (&c);
        const int cols = 2, rowH = 22;
        const int perCol = (int) std::ceil (withKeys.size() / (double) cols);
        const int colW = area.getWidth() / cols;
        for (int i = 0; i < (int) withKeys.size(); ++i)
        {
            const int col = i / juce::jmax (1, perCol), row = i % juce::jmax (1, perCol);
            auto cell = juce::Rectangle<int> (area.getX() + col * colW, area.getY() + row * rowH, colW - 16, rowH);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            const float kw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), withKeys[(size_t) i]->keys) + 14.0f;
            auto pill = cell.removeFromLeft (juce::jmax (90, (int) kw)).toFloat().reduced (0.0f, 2.0f);
            g.setColour (Theme::panelLight);
            g.fillRoundedRectangle (pill.withWidth (kw), 5.0f);
            g.setColour (Theme::text);
            g.drawText (withKeys[(size_t) i]->keys, pill.withWidth (kw), juce::Justification::centred);
            g.setFont (juce::FontOptions (13.0f));
            g.setColour (Theme::text.withAlpha (0.9f));
            g.drawText (withKeys[(size_t) i]->name, cell.withTrimmedLeft (8), juce::Justification::centredLeft, true);
        }
    }
    void mouseDown (const juce::MouseEvent&) override { setVisible (false); }
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey || k == juce::KeyPress (juce::KeyPress::F1Key)) { setVisible (false); return true; }
        return false;
    }
private:
    Studio2026& owner;
};

//==============================================================================
class Studio2026::LyricsPanel : public juce::Component, private juce::TextEditor::Listener
{
public:
    explicit LyricsPanel (Studio2026& o) : owner (o)
    {
        editor.setMultiLine (true, true);
        editor.setReturnKeyStartsNewLine (true);
        editor.setScrollbarsShown (true);
        editor.setFont (juce::FontOptions (16.0f));
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black.withAlpha (0.25f));
        editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::focusedOutlineColourId, Theme::accent.withAlpha (0.4f));
        editor.setTextToShowWhenEmpty ("Write your lyrics, ideas and notes here.\nThey're saved with the project.", Theme::textDim);
        editor.addListener (this);
        addAndMakeVisible (editor);
        bigBtn.setClickingTogglesState (true);
        bigBtn.setTooltip ("Big text for singing along");
        bigBtn.onClick = [this] { editor.applyFontToAllText (juce::FontOptions (bigBtn.getToggleState() ? 26.0f : 16.0f)); editor.setFont (juce::FontOptions (bigBtn.getToggleState() ? 26.0f : 16.0f)); };
        addAndMakeVisible (bigBtn);
        closeBtn.onClick = [this] { setVisible (false); };
        addAndMakeVisible (closeBtn);
    }
    void reload() { editor.setText (owner.ctx.engine.lyrics, false); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Theme::windowBg.withAlpha (0.93f));
        g.fillRoundedRectangle (r, 12.0f);
        Glass::panel (g, r, 12.0f, 1.2f);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText ("Lyrics & notes", getLocalBounds().reduced (12, 8).removeFromTop (24), juce::Justification::centredLeft);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);
        auto top = r.removeFromTop (26);
        closeBtn.setBounds (top.removeFromRight (56).reduced (0, 1));
        bigBtn.setBounds (top.removeFromRight (82).reduced (4, 1));
        r.removeFromTop (6);
        editor.setBounds (r);
    }
private:
    void textEditorTextChanged (juce::TextEditor&) override { owner.ctx.engine.lyrics = editor.getText(); }
    Studio2026& owner;
    juce::TextEditor editor;
    juce::TextButton bigBtn { "Big text" }, closeBtn { "Close" };
};

//==============================================================================
Studio2026::Studio2026 (AppContext& c, juce::ApplicationProperties& p, Host h) : ctx (c), props (p), host (std::move (h))
{
    setInterceptsMouseClicks (false, true);
    palette = std::make_unique<Palette> (*this);
    addChildComponent (*palette);
    sheet = std::make_unique<ShortcutSheet> (*this);
    addChildComponent (*sheet);
    lyrics = std::make_unique<LyricsPanel> (*this);
    addChildComponent (*lyrics);

    // restore preferences
    if (auto* s = props.getUserSettings())
    {
        ctx.engine.metronomeVolume = (float) s->getDoubleValue ("metronomeVolume", 1.0);
        ctx.engine.metronomeSound = s->getIntValue ("metronomeSound", 0);
        ctx.engine.metronomeAccent = s->getBoolValue ("metronomeAccent", true);
        ctx.engine.lowLatencyMonitoring = s->getBoolValue ("lowLatencyMonitoring", true);
        Exporter::loudnessTarget() = s->getDoubleValue ("exportLoudnessTarget", 0.0);
        ctx.engine.losslessUntouched = s->getBoolValue ("losslessUntouched", true);
        ctx.engine.returnOnStop = s->getBoolValue ("returnOnStop", true);
        ctx.engine.trimMp3Padding = s->getBoolValue ("trimMp3Padding", true);
    }

    ctx.trackAction = [this] (Track& t, int a) { trackAction (t, a); };
    ctx.clipAction = [this] (Track& t, std::shared_ptr<AudioClip> cl, int a) { clipAction (t, std::move (cl), a); };
    registerOwnCommands();
    startTimerHz (10);
}

Studio2026::~Studio2026()
{
    ctx.trackAction = nullptr;
    ctx.clipAction = nullptr;
}

void Studio2026::addCommand (Command c) { commands.push_back (std::move (c)); }

bool Studio2026::requireTier (int tier, const juce::String& feature)
{
    if (quiet)
        return true;
    if ((ctx.tier ? ctx.tier() : 2) >= tier)
        return true;
    if (ctx.showProPrompt)
        ctx.showProPrompt (feature, tier);
    return false;
}

void Studio2026::registerOwnCommands()
{
    auto& e = ctx.engine;
    addCommand ({ "Command palette", "Help", "Ctrl+K", [this] { showPalette(); } });
    addCommand ({ "Keyboard shortcuts", "Help", "F1", [this] { showShortcuts(); } });
    addCommand ({ "Loop playback on / off", "Transport", "L", [this] { toggleLoop(); } });
    addCommand ({ "Loop the current bar", "Transport", {}, [this, &e]
    {
        const double sr = e.getSampleRate();
        const auto bar = (juce::int64) (4.0 * 60.0 / juce::jmax (20.0, e.bpm.load()) * sr);
        e.loopStart = (e.getTransportPosition() / juce::jmax<juce::int64> (1, bar)) * bar;
        e.loopEnd = e.loopStart.load() + bar;
        e.loopEnabled = true;
        toast ("Looping bar " + juce::String (e.loopStart.load() / juce::jmax<juce::int64> (1, bar) + 1));
    } });
    addCommand ({ "Add marker at playhead", "Markers", "M", [this] { addMarkerAtPlayhead(); } });
    addCommand ({ "Previous marker", "Markers", ",", [this] { jumpMarker (-1); } });
    addCommand ({ "Next marker", "Markers", ".", [this] { jumpMarker (1); } });
    addCommand ({ "Monitor: cycle stereo / mono / side / left / right", "Monitoring", "Ctrl+M", [this] { cycleMonitorMode(); } });
    addCommand ({ "Monitor: back to stereo", "Monitoring", {}, [this, &e] { e.monitorMode = 0; toast ("Monitoring in stereo"); } });
    addCommand ({ "Return to start on stop (on / off)", "Transport", {}, [this, &e]
    {
        e.returnOnStop = ! e.returnOnStop.load();
        props.getUserSettings()->setValue ("returnOnStop", e.returnOnStop.load());
        toast (e.returnOnStop.load() ? "Stop now jumps back to where you pressed play" : "Stop now stays where playback ended");
    } });
    addCommand ({ "Low-latency monitoring while recording", "Monitoring", {}, [this, &e]
    {
        e.lowLatencyMonitoring = ! e.lowLatencyMonitoring.load();
        props.getUserSettings()->setValue ("lowLatencyMonitoring", e.lowLatencyMonitoring.load());
        toast (e.lowLatencyMonitoring.load() ? "Low-latency monitoring on" : "Low-latency monitoring off");
    } });
    addCommand ({ "Capture last performance (retrospective record)", "Recording", "Shift+R", [this] { captureRetro(); }, 1 });
    addCommand ({ "Lyrics & notes panel", "View", "Ctrl+L", [this] { toggleLyrics(); } });
    addCommand ({ "Reference track: load a song to compare", "Mixing", {}, [this] { loadReference(); }, 1 });
    addCommand ({ "Reference track: A/B switch", "Mixing", "Q", [this] { toggleReference(); }, 1 });
    addCommand ({ "Export stems (one file per track)", "Export", {}, [this] { exportStems(); }, 1 });
    addCommand ({ "Smart Vocal Chain on the selected track", "Mixing", {}, [this] { if (auto* t = host.selectedTrack ? host.selectedTrack() : nullptr) smartVocalChain (*t); else toast ("Select a track first (in the mixer)", true); }, 1 });
    addCommand ({ "Bounce the selected track in place", "Mixing", {}, [this] { if (auto* t = host.selectedTrack ? host.selectedTrack() : nullptr) bounceInPlace (*t); else toast ("Select a track first (in the mixer)", true); }, 1 });
    addCommand ({ "Set input level (sing for 5 seconds)", "Recording", {}, [this] { if (auto* t = host.selectedTrack ? host.selectedTrack() : nullptr) startInputLevelCheck (*t); else toast ("Arm or select a track first", true); } });
    addCommand ({ "Save as template...", "File", {}, [this] { saveAsTemplate(); } });
    addCommand ({ "Copy selected clips / highlighted range", "Edit", "Ctrl+C", [this] { copySelection (false); } });
    addCommand ({ "Cut", "Edit", "Ctrl+X", [this] { copySelection (true); } });
    addCommand ({ "Paste at the playhead", "Edit", "Ctrl+V", [this] { paste(); } });
    addCommand ({ "Duplicate (right after)", "Edit", "Ctrl+D", [this] { duplicateSelection(); } });
    addCommand ({ "Delete selection", "Edit", "Delete", [this] { deleteSelection(); } });
    addCommand ({ "Reset master loudness meter", "Mixing", {}, [this, &e] { e.resetMasterLoudness(); toast ("Loudness meter reset"); } });
}

//==============================================================================
bool Studio2026::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    const auto ch = juce::CharacterFunctions::toLowerCase (k.getTextCharacter() != 0 ? k.getTextCharacter() : (juce::juce_wchar) k.getKeyCode());
    if (k == juce::KeyPress ('k', juce::ModifierKeys::commandModifier, 0)) { showPalette(); return true; }
    if (k == juce::KeyPress ('c', juce::ModifierKeys::commandModifier, 0)) { copySelection (false); return true; }
    if (k == juce::KeyPress ('x', juce::ModifierKeys::commandModifier, 0)) { copySelection (true); return true; }
    if (k == juce::KeyPress ('v', juce::ModifierKeys::commandModifier, 0)) { paste(); return true; }
    if (k == juce::KeyPress ('d', juce::ModifierKeys::commandModifier, 0)) { duplicateSelection(); return true; }
    if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
    {
        if (ctx.selection.hasRange() || ! ctx.selection.clips.empty()) { deleteSelection(); return true; }
        return false;
    }
    if (k == juce::KeyPress::escapeKey && (ctx.selection.hasRange() || ! ctx.selection.clips.empty()))
    {
        ctx.selection.clear();
        if (auto* p = getParentComponent()) p->repaint();
        return true;
    }
    if (k == juce::KeyPress (juce::KeyPress::F1Key) || k == juce::KeyPress ('/', juce::ModifierKeys::commandModifier, 0)) { showShortcuts(); return true; }
    if (k == juce::KeyPress ('l', juce::ModifierKeys::commandModifier, 0)) { toggleLyrics(); return true; }
    if (k == juce::KeyPress ('m', juce::ModifierKeys::commandModifier, 0)) { cycleMonitorMode(); return true; }
    if (mods.isShiftDown() && ! mods.isCommandDown() && (ch == 'r')) { captureRetro(); return true; }
    if (mods.isAnyModifierKeyDown())
        return false;
    if (ch == 'l') { toggleLoop(); return true; }
    if (ch == 'm') { addMarkerAtPlayhead(); return true; }
    if (ch == ',') { jumpMarker (-1); return true; }
    if (ch == '.') { jumpMarker (1); return true; }
    if (ch == 'q') { toggleReference(); return true; }
    return false;
}

void Studio2026::toast (const juce::String& text, bool warning)
{
    auto* t = toasts.add (new Toast());
    t->text = text;
    t->warning = warning;
    t->born = nowSeconds();
    while (toasts.size() > 4)
        toasts.remove (0);
    repaint();
}

void Studio2026::showPalette()
{
    sheet->setVisible (false);
    palette->setBounds (getLocalBounds().withSizeKeepingCentre (juce::jmin (640, getWidth() - 40), juce::jmin (440, getHeight() - 80)).withY (90));
    palette->open();
}

void Studio2026::showShortcuts()
{
    palette->setVisible (false);
    sheet->setBounds (getLocalBounds().withSizeKeepingCentre (juce::jmin (860, getWidth() - 40), juce::jmin (560, getHeight() - 60)));
    sheet->setVisible (true);
    sheet->toFront (true);
    sheet->grabKeyboardFocus();
}

void Studio2026::toggleLyrics()
{
    if (lyrics->isVisible()) { lyrics->setVisible (false); return; }
    lyrics->reload();
    resized();
    lyrics->setVisible (true);
    lyrics->toFront (false);
}

bool Studio2026::isLyricsOpen() const { return lyrics->isVisible(); }
void Studio2026::sessionChanged() { lyrics->reload(); repaint(); }

void Studio2026::resized()
{
    lyrics->setBounds (getLocalBounds().removeFromRight (340).reduced (10, 0).withTrimmedTop (statusArea.getBottom() + 6).withTrimmedBottom (40));
    if (palette->isVisible()) showPalette();
}

bool Studio2026::hitTest (int x, int y)
{
    // only our panels take clicks; everything else falls through to the app
    for (auto* c : { (juce::Component*) palette.get(), (juce::Component*) sheet.get(), (juce::Component*) lyrics.get() })
        if (c->isVisible() && c->getBounds().contains (x, y))
            return true;
    return false;
}

void Studio2026::paint (juce::Graphics& g)
{
    auto& e = ctx.engine;
    // Status pill: live loudness + active modes
    if (! statusArea.isEmpty())
    {
        juce::StringArray parts;
        const float st = e.masterShortTermLufs.load();
        if (st > -70.0f && e.isPlaying()) parts.add (juce::String (st, 1) + " LUFS");
        if (e.loopEnabled.load() && e.hasLoopRegion()) parts.add ("LOOP");
        static const char* const modes[] = { "", "MONO", "SIDE", "LEFT", "RIGHT" };
        if (e.monitorMode.load() > 0) parts.add (modes[juce::jlimit (0, 4, e.monitorMode.load())]);
        if (e.referenceOn.load()) parts.add ("REFERENCE" + (e.referenceMatch.load() ? juce::String (" ") + juce::String (e.getReferenceGainDb(), 1) + " dB" : juce::String()));
        if (e.getMaxChannelLatency() > 0) parts.add ("PDC " + juce::String (e.getMaxChannelLatency() / juce::jmax (1.0, e.getSampleRate()) * 1000.0, 1) + " ms");
        if (levelTrack != nullptr) parts.add ("LEVEL CHECK");
        if (! parts.isEmpty())
        {
            const auto text = parts.joinIntoString ("   ");
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 20.0f;
            auto pill = juce::Rectangle<float> ((float) statusArea.getRight() - w, (float) statusArea.getY(), w, (float) statusArea.getHeight());
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
            g.setColour ((e.referenceOn.load() ? Theme::monitor : e.monitorMode.load() > 0 ? Theme::solo : Theme::accent).withAlpha (0.8f));
            g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);
            g.setColour (Theme::text);
            g.drawText (text, pill, juce::Justification::centred);
        }
    }

    // Toasts, newest at the bottom
    auto area = getLocalBounds().reduced (20).removeFromBottom (60 * 4);
    float y = (float) area.getBottom();
    for (int i = toasts.size(); --i >= 0;)
    {
        auto* t = toasts[i];
        g.setFont (juce::FontOptions (14.0f));
        const float w = juce::jmin ((float) getWidth() - 60.0f, juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), t->text) + 40.0f);
        auto r = juce::Rectangle<float> ((float) getWidth() * 0.5f - w * 0.5f, y - 40.0f, w, 36.0f);
        y -= 44.0f;
        const float a = t->alpha();
        g.setColour (Theme::windowBg.withAlpha (0.92f * a));
        g.fillRoundedRectangle (r, 18.0f);
        g.setColour ((t->warning ? Theme::record : Theme::accent).withAlpha (0.85f * a));
        g.drawRoundedRectangle (r.reduced (0.5f), 18.0f, 1.3f);
        g.setColour (Theme::text.withAlpha (a));
        g.drawText (t->text, r.reduced (14.0f, 0.0f), juce::Justification::centred, true);
    }
}

void Studio2026::timerCallback()
{
    for (int i = toasts.size(); --i >= 0;)
        if (toasts[i]->expired())
            toasts.remove (i);

    // input level check: collect the loudest peak for 5 seconds
    if (levelTrack != nullptr)
    {
        if (! ctx.engine.isValidChannel (levelTrack)) { levelTrack = nullptr; return; }
        levelPeak = juce::jmax (levelPeak, levelTrack->inputPeak.exchange (0.0f));
        const int left = (int) std::ceil (levelEndsAt - nowSeconds());
        if (left != levelLastShown && left > 0)
        {
            levelLastShown = left;
            toast ("Sing your loudest part now... " + juce::String (left));
        }
        if (nowSeconds() >= levelEndsAt)
        {
            auto* t = levelTrack;
            levelTrack = nullptr;
            if (levelWasArmed) t->armed = false;   // we armed it just for the check
            const float peakDb = juce::Decibels::gainToDecibels (levelPeak, -100.0f);
            if (peakDb < -55.0f)
                toast ("No input heard on " + t->name + " - check its input and the mic", true);
            else
            {
                // aim the loudest peak at -9 dBFS: loud enough, with headroom for the big notes
                const float newGain = juce::jlimit (-12.0f, 24.0f, std::round (t->inputGainDb.load() + (-9.0f - peakDb)));
                t->inputGainDb = newGain;
                toast (t->name + ": peaks were " + juce::String (peakDb, 1) + " dBFS, input gain set to "
                       + (newGain >= 0 ? "+" : "") + juce::String (newGain, 0) + " dB");
                if (host.refresh) host.refresh();
            }
        }
    }
    repaint();
}

//==============================================================================
void Studio2026::toggleLoop()
{
    auto& e = ctx.engine;
    if (! e.hasLoopRegion())
    {
        // no region yet: loop 4 bars from the playhead's bar
        const auto bar = (juce::int64) (4.0 * 60.0 / juce::jmax (20.0, e.bpm.load()) * e.getSampleRate());
        e.loopStart = (e.getTransportPosition() / juce::jmax<juce::int64> (1, bar)) * bar;
        e.loopEnd = e.loopStart.load() + 4 * bar;
        e.loopEnabled = true;
        toast ("Looping 4 bars (Alt-drag on the ruler to change it)");
    }
    else
    {
        e.loopEnabled = ! e.loopEnabled.load();
        toast (e.loopEnabled.load() ? "Loop on" : "Loop off");
    }
    if (auto* p = getParentComponent()) p->repaint();
}

void Studio2026::addMarkerAtPlayhead()
{
    auto& e = ctx.engine;
    const double t = (double) e.getTransportPosition() / juce::jmax (1.0, e.getSampleRate());
    addMarkerAt (e, t);
    for (auto& m : e.markers)
        if (std::abs (m.seconds - t) < 1.0e-6)
            toast ("Marker \"" + m.name + "\" added (right-click the ruler to rename)");
    if (auto* p = getParentComponent()) p->repaint();
}

void Studio2026::jumpMarker (int direction)
{
    auto& e = ctx.engine;
    if (e.markers.empty()) { toast ("No markers yet - press M to add one"); return; }
    const double sr = juce::jmax (1.0, e.getSampleRate());
    const double now = (double) e.getTransportPosition() / sr;
    const AudioEngine::Marker* target = nullptr;
    if (direction > 0)
    {
        for (auto& m : e.markers) if (m.seconds > now + 0.01) { target = &m; break; }
    }
    else
    {
        for (auto it = e.markers.rbegin(); it != e.markers.rend(); ++it) if (it->seconds < now - 0.25) { target = &*it; break; }
        if (target == nullptr) target = &e.markers.front();
    }
    if (target == nullptr) return;
    e.setTransportPosition ((juce::int64) std::llround (target->seconds * sr));
    toast (target->name);
    if (auto* p = getParentComponent()) p->repaint();
}

void Studio2026::cycleMonitorMode()
{
    auto& e = ctx.engine;
    e.monitorMode = (e.monitorMode.load() + 1) % 5;
    static const char* const names[] = { "Monitoring in stereo", "Mono check: both speakers play L+R",
                                         "Side check: only what's different between L and R", "Left channel only", "Right channel only" };
    toast (names[e.monitorMode.load()]);
}

//==============================================================================
void Studio2026::captureRetro()
{
    if (! requireTier (1, "Capture last performance"))
        return;
    auto& e = ctx.engine;
    if (e.isRecording()) { toast ("You're already recording", true); return; }
    const double secs = e.getRetroSeconds();
    auto [track, clip] = e.captureRetro();
    if (clip == nullptr)
    {
        toast (secs < 0.3 ? "Nothing to capture - arm a track (or turn on monitoring) and sing first" : "Only silence was heard", true);
        return;
    }
    if (ctx.checkpoint) ctx.checkpoint ("Capture performance");
    Track* dest = e.findTrackForTake (track, *clip);
    if (dest == nullptr && host.addTrack)
    {
        dest = host.addTrack (track->name + " capture");
        if (dest != nullptr) e.setPlayThrough (*dest, track);
    }
    if (dest == nullptr) dest = track;
    const double len = clip->getLength() / juce::jmax (1.0, e.getSampleRate());
    e.addClip (*dest, std::move (clip));
    if (host.refresh) host.refresh();
    toast ("Captured " + juce::String (len, 1) + " s from " + track->name);
}

void Studio2026::trackAction (Track& t, int action)
{
    switch (action)
    {
        case 1: bounceInPlace (t); break;
        case 2: smartVocalChain (t); break;
        case 3: startInputLevelCheck (t); break;
        case 4: captureRetro(); break;
        default: break;
    }
}

void Studio2026::clipAction (Track& t, std::shared_ptr<AudioClip> c, int action)
{
    if (action == 1 && c != nullptr)
        reverseReverbSwell (t, c);
}

//==============================================================================
void Studio2026::startInputLevelCheck (Track& t)
{
    if (t.isAutomation() || t.isMaster) return;
    levelTrack = &t;
    levelWasArmed = false;
    if (! t.armed.load() && ! t.monitor.load())
    {
        t.armed = true;   // the engine measures armed tracks; put back afterwards
        levelWasArmed = true;
    }
    t.inputPeak = 0.0f;
    levelPeak = 0.0f;
    levelEndsAt = nowSeconds() + 5.0;
    levelLastShown = -1;
}

//==============================================================================
void Studio2026::loadReference()
{
    if (! requireTier (1, "Reference track"))
        return;
    ctx.chooser = std::make_unique<juce::FileChooser> ("Choose a reference song", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       ctx.engine.audioFormats.getWildcardForAllFormats());
    juce::Component::SafePointer<Studio2026> safe (this);
    ctx.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
        auto& e = safe->ctx.engine;
        auto clip = e.loadClip (fc.getResult(), 0);
        if (clip == nullptr || clip->buffer == nullptr) { safe->toast ("Couldn't read that file", true); return; }
        const auto lufs = Loudness::analyse (*clip->buffer, e.getSampleRate());
        e.setReference (clip->buffer, lufs.integrated);
        e.referenceOn = false;
        safe->toast ("Reference loaded: " + fc.getResult().getFileNameWithoutExtension() + " (" + juce::String (lufs.integrated, 1)
                     + " LUFS). Press Q while playing to compare.");
    });
}

void Studio2026::toggleReference()
{
    auto& e = ctx.engine;
    if (! e.hasReference())
    {
        if ((ctx.tier ? ctx.tier() : 2) >= 1) toast ("Load a reference song first (Tools > Reference track)");
        else requireTier (1, "Reference track");
        return;
    }
    e.referenceOn = ! e.referenceOn.load();
    toast (e.referenceOn.load() ? "Hearing the REFERENCE" + (e.referenceMatch.load() ? juce::String (" (level matched)") : juce::String()) : "Hearing your MIX");
}

//==============================================================================
void Studio2026::exportStems()
{
    if (! requireTier (1, "Stem export"))
        return;
    auto& e = ctx.engine;
    if (e.getSessionLengthSamples() <= 0) { toast ("Nothing to export yet", true); return; }
    const auto project = host.projectFile ? host.projectFile() : juce::File();
    const auto name = project != juce::File() ? project.getFileNameWithoutExtension() : juce::String ("Mix");
    ctx.chooser = std::make_unique<juce::FileChooser> ("Choose a folder for the stems", AudioEngine::getAppFolder());
    juce::Component::SafePointer<Studio2026> safe (this);
    ctx.chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe, name] (const juce::FileChooser& fc)
    {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        auto folder = fc.getResult().getChildFile (juce::File::createLegalFileName (name + " stems"));
        folder.createDirectory();
        auto& e = safe->ctx.engine;
        std::vector<Track*> list;
        for (auto* t : e.getTracks())
        {
            if (t->isAutomation() || t->playThrough != nullptr) continue;
            bool hasAudio = ! t->clips.empty();
            for (auto* l : e.getTracks()) hasAudio = hasAudio || (l->playThrough == t && ! l->clips.empty());
            if (hasAudio) list.push_back (t);
        }
        if (list.empty()) { safe->toast ("No tracks with audio", true); return; }
        auto written = std::make_shared<int> (0);
        (new Job ("Exporting stems", safe.getComponent(), [&e, list, folder, written] (Job& job) -> juce::Result
        {
            for (size_t i = 0; i < list.size(); ++i)
            {
                job.status ("Rendering " + list[i]->name + " (" + juce::String ((int) i + 1) + " of " + juce::String ((int) list.size()) + ")");
                juce::AudioBuffer<float> b;
                auto r = e.renderChannel (*list[i], true, b, [&job, i, n = list.size()] (float p) { return job.progress ((float) ((i + p) / (double) n)); }, 3.0);
                if (r.failed()) return r;
                const auto f = folder.getChildFile (juce::String ((int) i + 1).paddedLeft ('0', 2) + " " + juce::File::createLegalFileName (list[i]->name) + ".wav");
                if (! writeWav (f, b, e.getSampleRate())) return juce::Result::fail ("Couldn't write " + f.getFullPathName());
                ++*written;
            }
            return juce::Result::ok();
        }, [safe, folder, written] (juce::Result r)
        {
            if (safe == nullptr) return;
            if (r.failed()) { safe->toast ("Stems: " + r.getErrorMessage(), true); return; }
            safe->toast (juce::String (*written) + " stems saved (post-fader, without master effects)");
            folder.revealToUser();
        }))->launchThread();
    });
}

//==============================================================================
void Studio2026::bounceInPlace (Track& t)
{
    if (! requireTier (1, "Bounce in place"))
        return;
    auto& e = ctx.engine;
    if (t.isAutomation() || t.isMaster) return;
    if (t.clips.empty()) { toast (t.name + " has no audio to bounce", true); return; }
    auto* tp = &t;
    juce::Component::SafePointer<Studio2026> safe (this);
    auto buffer = std::make_shared<juce::AudioBuffer<float>>();
    (new Job ("Bouncing " + t.name, this, [&e, tp, buffer] (Job& job) -> juce::Result
    {
        job.status ("Rendering " + tp->name + " with its effects...");
        return e.renderChannel (*tp, false, *buffer, [&job] (float p) { return job.progress (p); }, 2.0);
    }, [safe, tp, buffer] (juce::Result r)
    {
        if (safe == nullptr) return;
        auto& eng = safe->ctx.engine;
        if (r.failed() || ! eng.isValidChannel (tp)) { safe->toast ("Bounce: " + r.getErrorMessage(), true); return; }
        AudioEngine::getTakesFolder().createDirectory();
        auto file = AudioEngine::getTakesFolder().getNonexistentChildFile (juce::File::createLegalFileName (tp->name) + "_bounce_"
                                                                           + juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S"), ".wav");
        if (! writeWav (file, *buffer, eng.getSampleRate())) { safe->toast ("Couldn't write the bounce", true); return; }
        auto clip = eng.loadClip (file, 0);
        if (clip == nullptr) { safe->toast ("Couldn't load the bounce", true); return; }
        clip->name = tp->name + " (bounced)";
        if (safe->ctx.checkpoint) safe->ctx.checkpoint ("Bounce in place");
        // the old audio stays in the project, muted, so nothing is lost
        for (auto& c : tp->clips) c->enabled = false;
        for (auto* lane : eng.getTracks())
            if (lane->playThrough == tp)
                for (auto& c : lane->clips) c->enabled = false;
        eng.addClip (*tp, std::move (clip));
        int bypassed = 0;
        for (auto& s : tp->inserts)
            if (s->plugin != nullptr && ! s->bypassed.load()) { s->bypassed = true; ++bypassed; }
        tp->lowCutHz = 0.0f;
        tp->phaseInvert = false;
        if (safe->host.refresh) safe->host.refresh();
        safe->toast (tp->name + " bounced: " + juce::String (bypassed) + " effects switched off, the original audio is kept muted");
    }))->launchThread();
}

//==============================================================================
void Studio2026::reverseReverbSwell (Track& t, std::shared_ptr<AudioClip> clip)
{
    if (! requireTier (1, "Reverse reverb swell"))
        return;
    auto& e = ctx.engine;
    if (clip == nullptr || clip->buffer == nullptr) return;
    const double sr = e.getSampleRate();
    const int len = (int) juce::jmin<juce::int64> (clip->getLength(), (juce::int64) (sr * 3.0));   // the first 3 s of the phrase
    const int tail = (int) (sr * 2.5);
    if (len < (int) (sr * 0.1)) return;

    // reverse -> reverb (wet only) -> reverse: the tail becomes a swell that rises into the clip
    juce::AudioBuffer<float> work (2, len + tail);
    work.clear();
    const auto& src = *clip->buffer;
    const auto offset = clip->getOffset();
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < len; ++i)
            work.setSample (c, i, src.getSample (juce::jmin (c, src.getNumChannels() - 1), (int) offset + len - 1 - i));
    auto rv = Builtins::create ("amethyst.reverb");
    auto* am = dynamic_cast<AmethystPlugin*> (rv.get());
    if (am == nullptr) return;
    am->setPlayConfigDetails (2, 2, sr, 512);
    am->prepareToPlay (sr, 512);
    am->setParam ("type", 1); am->setParam ("time", 3.2f); am->setParam ("size", 85); am->setParam ("predelay", 0);
    am->setParam ("dry", 0); am->setParam ("er", 0); am->setParam ("wet", 100); am->setParam ("lowcut", 250); am->setParam ("duck", 0);
    juce::MidiBuffer midi;
    for (int i = 0; i < work.getNumSamples(); i += 512)
    {
        juce::AudioBuffer<float> view (work.getArrayOfWritePointers(), 2, i, juce::jmin (512, work.getNumSamples() - i));
        am->processBlock (view, midi);
    }
    // keep the swell (the reversed tail) plus a short overlap, with a fade at the end
    const int keep = tail + (int) (sr * 0.2);
    juce::AudioBuffer<float> out (2, keep);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < keep; ++i)
            out.setSample (c, i, work.getSample (c, work.getNumSamples() - 1 - i));
    const float peak = out.getMagnitude (0, keep);
    if (peak > 1.0e-6f) out.applyGain (0.5f / peak);   // a swell sits under the vocal, not on top
    out.applyGainRamp (keep - (int) (sr * 0.2), (int) (sr * 0.2), 1.0f, 0.0f);
    out.applyGainRamp (0, (int) (sr * 0.05), 0.0f, 1.0f);

    AudioEngine::getTakesFolder().createDirectory();
    auto file = AudioEngine::getTakesFolder().getNonexistentChildFile (juce::File::createLegalFileName (clip->getDisplayName()) + "_swell", ".wav");
    if (! writeWav (file, out, sr)) { toast ("Couldn't write the swell", true); return; }
    auto swell = e.loadClip (file, juce::jmax<juce::int64> (0, clip->startSample.load() - tail));
    if (swell == nullptr) return;
    swell->name = clip->getDisplayName() + " swell";
    if (ctx.checkpoint) ctx.checkpoint ("Reverse reverb swell");
    Track* dest = &t;
    if (! t.layerTakes.load() && host.addTrack)
    {
        // "newest on top" would hide the clip's first moments: give the swell its own lane through this channel
        if (auto* lane = host.addTrack (t.name + " swell"))
        {
            e.setPlayThrough (*lane, t.playThrough != nullptr ? t.playThrough : &t);
            dest = lane;
        }
    }
    e.addClip (*dest, std::move (swell));
    if (host.refresh) host.refresh();
    toast ("Reverse reverb swell added before \"" + clip->getDisplayName() + "\"");
}

//==============================================================================
void Studio2026::smartVocalChain (Track& t)
{
    if (! requireTier (1, "Smart Vocal Chain"))
        return;
    auto& e = ctx.engine;
    const double sr = e.getSampleRate();

    // gather up to 90 s of this channel's audio (mono)
    std::vector<float> audio;
    auto addClips = [&] (const Track& tr)
    {
        for (auto& c : tr.clips)
        {
            if (c->buffer == nullptr || ! c->enabled.load()) continue;
            const auto& b = *c->buffer;
            const auto off = c->getOffset(), len = c->getLength();
            for (juce::int64 i = 0; i < len && audio.size() < (size_t) (sr * 90.0); ++i)
            {
                float v = b.getSample (0, (int) (off + i));
                if (b.getNumChannels() > 1) v = 0.5f * (v + b.getSample (1, (int) (off + i)));
                audio.push_back (v);
            }
        }
    };
    addClips (t);
    for (auto* lane : e.getTracks()) if (lane->playThrough == &t) addClips (*lane);
    if (audio.size() < (size_t) (sr * 2.0)) { toast ("Record or import at least a few seconds of vocals on " + t.name + " first", true); return; }

    // level of the sung parts (50 ms frames above -50 dBFS)
    const int frame = (int) (sr * 0.05);
    double activeE = 0.0;
    long long activeN = 0;
    float peak = 0.0f;
    std::vector<int> activeFrames;
    for (size_t f = 0; f + (size_t) frame <= audio.size(); f += (size_t) frame)
    {
        double s = 0.0;
        for (int i = 0; i < frame; ++i) { const float v = audio[f + (size_t) i]; s += v * v; peak = juce::jmax (peak, std::abs (v)); }
        const double ms = s / frame;
        if (10.0 * std::log10 (ms + 1e-12) > -50.0) { activeE += s; activeN += frame; activeFrames.push_back ((int) f); }
    }
    if (activeN == 0) { toast ("That track is silent", true); return; }
    const float rmsDb = (float) (10.0 * std::log10 (activeE / (double) activeN + 1e-12));

    // average spectrum of the sung parts
    constexpr int order = 12, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<double> spec (N / 2, 0.0);
    std::vector<float> data (2 * N);
    int frames = 0;
    for (size_t f = 0; f + N <= audio.size(); f += N)
    {
        double s = 0.0;
        for (int i = 0; i < N; ++i) s += audio[f + (size_t) i] * audio[f + (size_t) i];
        if (10.0 * std::log10 (s / N + 1e-12) < -50.0) continue;
        std::fill (data.begin(), data.end(), 0.0f);
        for (int i = 0; i < N; ++i) data[(size_t) i] = audio[f + (size_t) i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N));
        fft.performFrequencyOnlyForwardTransform (data.data());
        for (int k = 0; k < N / 2; ++k) spec[(size_t) k] += (double) data[(size_t) k] * data[(size_t) k];
        ++frames;
    }
    auto bandDensity = [&] (double lo, double hi)   // dB per octave
    {
        double s = 0.0;
        for (int k = (int) (lo * N / sr); k <= (int) (hi * N / sr) && k < N / 2; ++k) s += spec[(size_t) k];
        return 10.0 * std::log10 (s / juce::jmax (1, frames) + 1e-12) - 10.0 * std::log10 (std::log2 (hi / lo));
    };
    auto peakFreq = [&] (double lo, double hi)
    {
        int best = (int) (lo * N / sr);
        for (int k = best; k <= (int) (hi * N / sr) && k < N / 2; ++k) if (spec[(size_t) k] > spec[(size_t) best]) best = k;
        return best * sr / N;
    };
    const double low = bandDensity (80, 160), mud = bandDensity (200, 500), body = bandDensity (500, 1000), presence = bandDensity (2000, 4500);
    const double air = bandDensity (10000, 16000), sib = bandDensity (5000, 10000);
    const double mudFreq = peakFreq (200, 500), sibFreq = juce::jlimit (4000.0, 10000.0, peakFreq (5000, 10000));
    const bool deepVoice = low > body + 2.0;

    juce::StringArray notes;
    notes.add ("Analysed " + juce::String (audio.size() / sr, 1) + " s of " + t.name + ": sung level " + juce::String (rmsDb, 1) + " dBFS RMS, peaks "
               + juce::String (juce::Decibels::gainToDecibels (peak), 1) + " dBFS" + (deepVoice ? ", deep voice" : ""));

    // ---- Facet EQ
    auto eq = makeBuiltin (e, "amethyst.eq");
    if (eq == nullptr) { toast ("Couldn't create the effects", true); return; }
    for (int b = 1; b <= 7; ++b) eq->setParam ("b" + juce::String (b) + "_on", 0.0f);
    auto band = [&] (int b, int type, double f, float gain, float q)
    {
        const auto id = [b] (const char* w) { return "b" + juce::String (b) + "_" + w; };
        eq->setParam (id ("on"), 1); eq->setParam (id ("type"), (float) type); eq->setParam (id ("freq"), (float) f);
        eq->setParam (id ("gain"), gain); eq->setParam (id ("q"), q);
    };
    const double lowCut = deepVoice ? 70.0 : 100.0;
    band (1, 0, lowCut, 0.0f, 0.7f);
    eq->setParam ("b1_slope", 1);
    notes.add ("EQ: low cut at " + juce::String ((int) lowCut) + " Hz (24 dB/oct) to clear rumble and pops");
    const double mudExcess = mud - body - 1.5;
    if (mudExcess > 0.5)
    {
        const float cut = (float) -juce::jlimit (1.0, 5.0, mudExcess * 0.7);
        band (3, 2, mudFreq, cut, 1.3f);
        notes.add ("EQ: " + juce::String (cut, 1) + " dB at " + juce::String ((int) mudFreq) + " Hz - it was boxy / muddy there");
    }
    const double presenceDeficit = (body - presence) - 6.0;
    if (presenceDeficit > 0.5)
    {
        const float boost = (float) juce::jlimit (1.0, 3.5, presenceDeficit * 0.5);
        band (5, 2, 3200.0, boost, 0.9f);
        notes.add ("EQ: +" + juce::String (boost, 1) + " dB presence at 3.2 kHz so the words cut through");
    }
    const double airDeficit = (presence - air) - 12.0;
    if (airDeficit > 0.5)
    {
        const float boost = (float) juce::jlimit (1.0, 4.0, airDeficit * 0.4);
        band (7, 3, 12000.0, boost, 0.7f);
        notes.add ("EQ: +" + juce::String (boost, 1) + " dB air shelf at 12 kHz for openness");
    }

    // ---- Prism compressor
    auto comp = makeBuiltin (e, "amethyst.dynamics");
    const float thr = juce::jlimit (-40.0f, -8.0f, rmsDb - 4.0f);
    comp->setParam ("mode", 1); comp->setParam ("character", 1); comp->setParam ("detect", 1);
    comp->setParam ("thresh", thr); comp->setParam ("ratio", 3.0f); comp->setParam ("att", 8.0f); comp->setParam ("rel", 140.0f); comp->setParam ("auto_mk", 1);
    notes.add ("Compressor: threshold " + juce::String (thr, 1) + " dB (4 dB under your sung level), 3:1, smooth character, auto gain");

    // ---- De-esser
    auto des = makeBuiltin (e, "amethyst.deesser");
    const float desThr = (float) juce::jlimit (-50.0, -12.0, (double) rmsDb - 8.0 + juce::jlimit (-6.0, 6.0, sib - presence + 8.0));
    des->setParam ("freq", (float) sibFreq); des->setParam ("thresh", desThr);
    notes.add ("De-esser: centred on your S sounds at " + juce::String ((int) sibFreq) + " Hz, threshold " + juce::String (desThr, 1) + " dB");

    // ---- Space: tempo echo + plate
    auto echo = makeBuiltin (e, "amethyst.echo");
    echo->setParam ("sync", 1); echo->setParam ("div", 8); echo->setParam ("wet", 9); echo->setParam ("fb", 22); echo->setParam ("fb_cut", 5000);
    echo->setParam ("duck", 50);
    auto plate = makeBuiltin (e, "amethyst.reverb");
    plate->setParam ("type", 2); plate->setParam ("time", 1.5f); plate->setParam ("predelay", 30); plate->setParam ("wet", 14);
    plate->setParam ("lowcut", 220); plate->setParam ("highcut", 9000); plate->setParam ("duck", 35);
    notes.add ("Space: a ducked 1/4-note echo and a short plate reverb, kept low so the vocal stays upfront");

    if (ctx.checkpoint) ctx.checkpoint ("Smart Vocal Chain");
    std::vector<std::unique_ptr<InsertSlot>> slots ((size_t) Track::maxInserts);
    auto put = [&] (int index, std::unique_ptr<AmethystPlugin> p)
    {
        auto s = std::make_unique<InsertSlot>();
        s->plugin = std::move (p);
        slots[(size_t) index] = std::move (s);
    };
    // keep any Tune that's already on the track first in line
    int next = 0;
    for (auto& s : t.inserts)
        if (auto* am = dynamic_cast<AmethystPlugin*> (s->plugin.get()); am != nullptr && am->pluginId == "amethyst.tune")
        {
            auto keep = std::make_unique<InsertSlot>();
            keep->plugin = std::move (s->plugin);
            keep->bypassed = s->bypassed.load();
            slots[(size_t) next++] = std::move (keep);
            notes.add ("Kept AMETHYST TUNE first in the chain");
            break;
        }
    put (next++, std::move (eq));
    put (next++, std::move (comp));
    put (next++, std::move (des));
    put (next++, std::move (echo));
    put (next++, std::move (plate));
    e.replaceInserts (t, std::move (slots)).clear();
    if (host.refresh) host.refresh();
    if (quiet) std::printf ("%s\n", notes.joinIntoString ("\n  ").toRawUTF8());
    else if (host.report) host.report ("Smart Vocal Chain - " + t.name, notes.joinIntoString ("\n") + "\n\nEvery setting is a normal effect you can tweak or bypass in the mixer.");
    toast ("Smart Vocal Chain built on " + t.name);
}

//==============================================================================
juce::File Studio2026::templatesFolder() const { return AudioEngine::getAppFolder().getChildFile ("Templates"); }

void Studio2026::saveAsTemplate()
{
    auto* box = new juce::AlertWindow ("Save as template", "Saves the tracks, routing, effects and settings (no audio).\nNew projects can start from it.",
                                       juce::MessageBoxIconType::NoIcon);
    box->addTextEditor ("name", "My vocal setup");
    box->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<Studio2026> safe (this);
    box->enterModalState (true, juce::ModalCallbackFunction::create ([safe, box] (int r)
    {
        if (r != 1 || safe == nullptr) return;
        const auto name = juce::File::createLegalFileName (box->getTextEditorContents ("name").trim());
        if (name.isEmpty()) return;
        safe->templatesFolder().createDirectory();
        const auto f = safe->templatesFolder().getChildFile (name + ProjectIO::fileExtension);
        auto res = ProjectIO::save (safe->ctx.engine, f);
        if (res.wasOk())
        {
            // drop the audio clips: a template is a starting point, not a song
            if (auto xml = juce::XmlDocument::parse (f))
            {
                for (auto* tx : xml->getChildWithTagNameIterator ("TRACK"))
                    while (auto* c = tx->getChildByName ("CLIP"))
                        tx->removeChildElement (c, true);
                if (auto* ly = xml->getChildByName ("LYRICS")) xml->removeChildElement (ly, true);
                xml->writeTo (f);
            }
            safe->toast ("Template \"" + name + "\" saved - find it under File > New from template");
        }
        else
            safe->toast (res.getErrorMessage(), true);
    }), true);
}

juce::File Studio2026::versionsFolderFor (const juce::File& project) const
{
    return project.getParentDirectory().getChildFile (project.getFileNameWithoutExtension() + " versions");
}

void Studio2026::archiveBeforeSave (const juce::File& file)
{
    if (! file.existsAsFile())
        return;
    auto dir = versionsFolderFor (file);
    dir.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S");
    file.copyFileTo (dir.getChildFile (file.getFileNameWithoutExtension() + " " + stamp + file.getFileExtension()));
    // keep the 20 newest
    auto all = dir.findChildFiles (juce::File::findFiles, false, "*" + file.getFileExtension());
    std::sort (all.begin(), all.end(), [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
    for (int i = 20; i < all.size(); ++i)
        all[i].deleteFile();
}

//==============================================================================
void Studio2026::addToFileMenu (juce::PopupMenu& m)
{
    juce::PopupMenu templates;
    auto files = templatesFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + ProjectIO::fileExtension);
    files.sort();
    juce::Component::SafePointer<Studio2026> safe (this);
    for (auto& f : files)
        templates.addItem (actionItem (f.getFileNameWithoutExtension(), [safe, f]
        {
            if (safe != nullptr && safe->host.openAs) safe->host.openAs (f, juce::File());
        }));
    if (files.isEmpty())
        templates.addItem (actionItem ("No templates yet - use Save as template", {}, false, false));
    templates.addSeparator();
    templates.addItem (actionItem ("Open templates folder", [safe] { if (safe != nullptr) { safe->templatesFolder().createDirectory(); safe->templatesFolder().startAsProcess(); } }));
    m.addSubMenu ("New from template", templates);
    m.addItem (actionItem ("Save as template...", [safe] { if (safe != nullptr) safe->saveAsTemplate(); }));

    const auto project = host.projectFile ? host.projectFile() : juce::File();
    juce::PopupMenu versions;
    if (project.existsAsFile())
    {
        auto all = versionsFolderFor (project).findChildFiles (juce::File::findFiles, false, "*" + project.getFileExtension());
        std::sort (all.begin(), all.end(), [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
        for (auto& v : all)
        {
            const auto when = v.getLastModificationTime();
            const auto label = when.formatted ("%d %b  %H:%M:%S") + "   (" + juce::String (v.getSize() / 1024) + " KB)";
            versions.addItem (actionItem (label, [safe, v, project]
            {
                if (safe == nullptr) return;
                juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Restore this version?",
                    "Opens the version saved " + v.getLastModificationTime().formatted ("%d %b at %H:%M") + ".\nSaving afterwards replaces the current "
                    + project.getFileName() + " (the current one is kept as a version too).", "Restore", "Cancel", nullptr,
                    juce::ModalCallbackFunction::create ([safe, v, project] (int r)
                    {
                        if (r == 1 && safe != nullptr && safe->host.openAs)
                        {
                            safe->host.openAs (v, project);
                            safe->toast ("Restored the version from " + v.getLastModificationTime().formatted ("%H:%M"));
                        }
                    }));
            }));
        }
    }
    if (versions.getNumItems() == 0)
        versions.addItem (actionItem (project.existsAsFile() ? "No earlier versions yet (one is kept each time you save)" : "Save the project first", {}, false, false));
    m.addSubMenu ("Restore earlier version", versions);
}

void Studio2026::addMetronomeMenu (juce::PopupMenu& m)
{
    auto& e = ctx.engine;
    auto* s = props.getUserSettings();
    m.addSectionHeader ("Metronome");
    static const char* const sounds[] = { "Classic beep", "Woodblock", "Hi-hat" };
    for (int i = 0; i < 3; ++i)
        m.addItem (actionItem (sounds[i], [&e, s, i] { e.metronomeSound = i; s->setValue ("metronomeSound", i); }, e.metronomeSound.load() == i));
    m.addSeparator();
    for (int v : { 25, 50, 75, 100, 150 })
        m.addItem (actionItem ("Volume " + juce::String (v) + "%", [&e, s, v] { e.metronomeVolume = v / 100.0f; s->setValue ("metronomeVolume", v / 100.0); },
                               std::abs (e.metronomeVolume.load() * 100.0f - (float) v) < 1.0f));
    m.addItem (actionItem ("Accent the first beat", [&e, s] { e.metronomeAccent = ! e.metronomeAccent.load(); s->setValue ("metronomeAccent", e.metronomeAccent.load()); },
                           e.metronomeAccent.load()));
}

void Studio2026::addToSettingsMenu (juce::PopupMenu& m)
{
    auto& e = ctx.engine;
    juce::Component::SafePointer<Studio2026> safe (this);
    m.addItem (actionItem ("Startup animation", [safe]
    {
        if (safe == nullptr) return;
        auto* s = safe->props.getUserSettings();
        s->setValue ("startupAnimation", ! s->getBoolValue ("startupAnimation", true));
    }, props.getUserSettings()->getBoolValue ("startupAnimation", true)));
    m.addItem (actionItem ("Return to start on stop", [safe, &e]
    {
        e.returnOnStop = ! e.returnOnStop.load();
        if (safe != nullptr) safe->props.getUserSettings()->setValue ("returnOnStop", e.returnOnStop.load());
    }, e.returnOnStop.load()));
    juce::PopupMenu mon;
    static const char* const modes[] = { "Stereo", "Mono check", "Side (L-R) check", "Left only", "Right only" };
    for (int i = 0; i < 5; ++i)
        mon.addItem (actionItem (modes[i], [&e, i] { e.monitorMode = i; }, e.monitorMode.load() == i));
    mon.addSeparator();
    mon.addItem (actionItem ("Low-latency monitoring while recording", [safe, &e]
    {
        e.lowLatencyMonitoring = ! e.lowLatencyMonitoring.load();
        if (safe != nullptr) safe->props.getUserSettings()->setValue ("lowLatencyMonitoring", e.lowLatencyMonitoring.load());
    }, e.lowLatencyMonitoring.load()));
    m.addSubMenu ("Monitoring", mon);
    juce::PopupMenu metro;
    addMetronomeMenu (metro);
    m.addSubMenu ("Metronome", metro);

    juce::PopupMenu files;
    files.addItem (actionItem ("WAV / AIFF / FLAC play untouched (lossless: no automatic edge fades)", [safe, &e]
    {
        e.losslessUntouched = ! e.losslessUntouched.load();
        if (safe != nullptr) safe->props.getUserSettings()->setValue ("losslessUntouched", e.losslessUntouched.load());
    }, e.losslessUntouched.load()));
    files.addItem (actionItem ("Trim MP3 encoder padding so beats line up (for files imported from now on)", [safe, &e]
    {
        e.trimMp3Padding = ! e.trimMp3Padding.load();
        if (safe != nullptr) safe->props.getUserSettings()->setValue ("trimMp3Padding", e.trimMp3Padding.load());
    }, e.trimMp3Padding.load()));
    m.addSubMenu ("Audio files", files);
}

void Studio2026::addToExportMenu (juce::PopupMenu& m)
{
    const int tier = ctx.tier ? ctx.tier() : 2;
    juce::Component::SafePointer<Studio2026> safe (this);
    m.addSeparator();
    m.addItem (actionItem ("Stems (one WAV per track)..." + tierSuffix (1, tier), [safe] { if (safe != nullptr) safe->exportStems(); }));
    juce::PopupMenu loud;
    struct T { const char* name; double lufs; };
    const T targets[] = { { "Off (as mixed)", 0.0 }, { "Spotify / YouTube  -14 LUFS", -14.0 }, { "Apple Music  -16 LUFS", -16.0 },
                          { "Loud / club  -9 LUFS", -9.0 }, { "Podcast  -19 LUFS", -19.0 } };
    for (auto& t : targets)
    {
        const double v = t.lufs;
        loud.addItem (actionItem (t.name, [safe, v]
        {
            if (safe == nullptr) return;
            if (v < 0.0 && ! safe->requireTier (1, "Loudness-matched export")) return;
            Exporter::loudnessTarget() = v;
            safe->props.getUserSettings()->setValue ("exportLoudnessTarget", v);
            safe->toast (v < 0.0 ? "Exports will be matched to " + juce::String (v, 0) + " LUFS (true peak -1 dB)" : "Exports keep their mixed loudness");
        }, std::abs (Exporter::loudnessTarget() - v) < 0.01));
    }
    m.addSubMenu ("Loudness target" + tierSuffix (1, tier), loud);
}

void Studio2026::addToToolsMenu (juce::PopupMenu& m)
{
    const int tier = ctx.tier ? ctx.tier() : 2;
    auto& e = ctx.engine;
    juce::Component::SafePointer<Studio2026> safe (this);
    m.addSectionHeader ("AMETHYST 2026");
    m.addItem (actionItem ("Command palette...   Ctrl+K", [safe] { if (safe != nullptr) safe->showPalette(); }));
    m.addItem (actionItem ("Keyboard shortcuts   F1", [safe] { if (safe != nullptr) safe->showShortcuts(); }));
    m.addItem (actionItem ("Lyrics & notes   Ctrl+L", [safe] { if (safe != nullptr) safe->toggleLyrics(); }, isLyricsOpen()));
    m.addItem (actionItem ("Capture last performance   Shift+R" + tierSuffix (1, tier), [safe] { if (safe != nullptr) safe->captureRetro(); }));
    Track* sel = host.selectedTrack ? host.selectedTrack() : nullptr;
    const juce::String selName = sel != nullptr ? "  (" + sel->name + ")" : juce::String();
    m.addItem (actionItem ("Smart Vocal Chain" + selName + tierSuffix (1, tier), [safe, sel]
    {
        if (safe == nullptr) return;
        if (sel != nullptr && safe->ctx.engine.isValidChannel (sel)) safe->smartVocalChain (*sel); else safe->toast ("Select a track in the mixer first", true);
    }));
    m.addItem (actionItem ("Bounce in place" + selName + tierSuffix (1, tier), [safe, sel]
    {
        if (safe == nullptr) return;
        if (sel != nullptr && safe->ctx.engine.isValidChannel (sel)) safe->bounceInPlace (*sel); else safe->toast ("Select a track in the mixer first", true);
    }));
    juce::PopupMenu ref;
    ref.addItem (actionItem ("Load reference song...", [safe] { if (safe != nullptr) safe->loadReference(); }));
    ref.addItem (actionItem ("A/B: hear the reference   Q", [safe] { if (safe != nullptr) safe->toggleReference(); }, e.referenceOn.load(), e.hasReference()));
    ref.addItem (actionItem ("Match loudness to the mix", [&e] { e.referenceMatch = ! e.referenceMatch.load(); }, e.referenceMatch.load(), e.hasReference()));
    ref.addItem (actionItem ("Remove reference", [&e] { e.setReference (nullptr, -100.0); }, false, e.hasReference()));
    m.addSubMenu ("Reference track" + tierSuffix (1, tier), ref);
    juce::PopupMenu loop;
    loop.addItem (actionItem ("Loop playback   L", [safe] { if (safe != nullptr) safe->toggleLoop(); }, e.loopEnabled.load() && e.hasLoopRegion()));
    loop.addItem (actionItem ("Clear loop", [&e] { e.loopStart = -1; e.loopEnd = -1; e.loopEnabled = false; }, false, e.hasLoopRegion()));
    loop.addSeparator();
    loop.addItem (actionItem ("Add marker at playhead   M", [safe] { if (safe != nullptr) safe->addMarkerAtPlayhead(); }));
    loop.addItem (actionItem ("Previous marker   ,", [safe] { if (safe != nullptr) safe->jumpMarker (-1); }, false, ! e.markers.empty()));
    loop.addItem (actionItem ("Next marker   .", [safe] { if (safe != nullptr) safe->jumpMarker (1); }, false, ! e.markers.empty()));
    m.addSubMenu ("Loop & markers", loop);
}

bool Studio2026::isPaletteOpen() const   { return palette->isVisible(); }
bool Studio2026::isShortcutsOpen() const { return sheet->isVisible(); }
void Studio2026::closeOverlays()         { palette->setVisible (false); sheet->setVisible (false); lyrics->setVisible (false); }

//==============================================================================
bool Studio2026::copySelection (bool cut)
{
    auto& e = ctx.engine;
    auto& sel = ctx.selection;
    auto& cb = ctx.clipboard;
    std::vector<std::shared_ptr<AudioClip>> copies;
    Track* source = nullptr;

    if (sel.hasRange() && e.isValidChannel (sel.rangeTrack))
    {
        source = sel.rangeTrack;
        for (auto& c : source->clips)
        {
            const auto s0 = c->startSample.load(), e0 = c->getEnd();
            const auto a = juce::jmax (s0, sel.rangeStart), b = juce::jmin (e0, sel.rangeEnd);
            if (b - a < AudioEngine::minClipSamples) continue;
            auto n = copyOf (*c);
            n->trimOffset = c->getOffset() + (a - s0);
            n->trimLength = b - a;
            n->startSample = a;
            copies.push_back (n);
        }
        cb.from = sel.rangeStart;
        cb.length = sel.rangeEnd - sel.rangeStart;
    }
    else
    {
        juce::int64 from = std::numeric_limits<juce::int64>::max(), to = 0;
        for (auto& [t, w] : sel.clips)
            if (auto c = w.lock(); c != nullptr && e.isValidChannel (t))
            {
                if (source == nullptr) source = t;
                copies.push_back (copyOf (*c));
                from = juce::jmin (from, c->startSample.load());
                to = juce::jmax (to, c->getEnd());
            }
        cb.from = from;
        cb.length = to - from;
    }
    if (copies.empty())
    {
        toast ("Select a clip first (click it), or Ctrl-drag across a track to highlight part of it", true);
        return false;
    }

    for (auto& c : copies)
        c->startSample = c->startSample.load() - cb.from;   // relative to the copy start
    cb.clips = copies;
    cb.sourceTrack = source;
    const double secs = (double) cb.length / juce::jmax (1.0, e.getSampleRate());

    if (cut)
    {
        if (ctx.checkpoint) ctx.checkpoint ("Cut");
        if (sel.hasRange()) removeRange (e, *sel.rangeTrack, sel.rangeStart, sel.rangeEnd);
        else
            for (auto& [t, w] : sel.clips)
                if (auto c = w.lock(); c != nullptr && e.isValidChannel (t))
                    e.removeClip (*t, c.get());
        sel.clear();
        if (auto* p = getParentComponent()) p->repaint();
    }
    toast ((cut ? "Cut " : "Copied ") + juce::String (secs, 1) + " s  -  Ctrl+V pastes it at the playhead, then drag it anywhere");
    return true;
}

void Studio2026::paste()
{
    auto& e = ctx.engine;
    auto& cb = ctx.clipboard;
    if (cb.isEmpty()) { toast ("Nothing copied yet - select a clip and press Ctrl+C", true); return; }

    Track* target = e.isValidChannel (ctx.selection.lastTrack) && ! ctx.selection.lastTrack->isAutomation() && ! ctx.selection.lastTrack->isMaster
                        ? ctx.selection.lastTrack : (e.isValidChannel (cb.sourceTrack) ? cb.sourceTrack : nullptr);
    if (target == nullptr) { toast ("Click a track first, then paste", true); return; }

    // at the playhead - or straight after the copied part if the playhead is still inside it on the same track
    juce::int64 at = e.getTransportPosition();
    if (target == cb.sourceTrack && at >= cb.from && at < cb.from + cb.length)
        at = cb.from + cb.length;

    if (ctx.checkpoint) ctx.checkpoint ("Paste");
    ctx.selection.clear();
    for (auto& c : cb.clips)
    {
        auto n = copyOf (*c);
        n->startSample = at + c->startSample.load();
        ctx.selection.clips.push_back ({ target, n });
        e.addClip (*target, std::move (n));
    }
    ctx.selection.lastTrack = target;
    if (auto* p = getParentComponent()) p->repaint();
    toast ("Pasted on " + target->name + " - it's selected, drag it where you want it");
}

void Studio2026::duplicateSelection()
{
    auto& e = ctx.engine;
    if (! copySelection (false))
        return;
    // paste right after the original, on the same track
    ctx.selection.lastTrack = ctx.clipboard.sourceTrack;
    const auto saved = e.getTransportPosition();
    e.setTransportPosition (ctx.clipboard.from + ctx.clipboard.length);
    paste();
    e.setTransportPosition (saved);
}

void Studio2026::deleteSelection()
{
    auto& e = ctx.engine;
    auto& sel = ctx.selection;
    if (! sel.hasRange() && sel.clips.empty()) return;
    if (ctx.checkpoint) ctx.checkpoint ("Delete");
    if (sel.hasRange() && e.isValidChannel (sel.rangeTrack))
        removeRange (e, *sel.rangeTrack, sel.rangeStart, sel.rangeEnd);
    for (auto& [t, w] : sel.clips)
        if (auto c = w.lock(); c != nullptr && e.isValidChannel (t))
            e.removeClip (*t, c.get());
    sel.clear();
    if (auto* p = getParentComponent()) p->repaint();
}
