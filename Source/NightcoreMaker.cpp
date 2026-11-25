#include "NightcoreMaker.h"
#include "Exporter.h"
#include "Theme.h"
#include "Mascot.h"

namespace
{
    juce::String formatTime (double seconds)
    {
        const int s = juce::jmax (0, (int) seconds);
        return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
    }
}

//==============================================================================
juce::AudioBuffer<float> NightcoreMaker::render (const juce::AudioBuffer<float>& src, double sr, const Settings& st)
{
    const int inLen = src.getNumSamples();
    const double speed = juce::jlimit (0.5, 1.5, st.speed);
    const int tail = (int) (sr * 3.0 * st.reverb);
    const int outLen = juce::jmax (1, (int) ((inLen - 8) / speed));

    juce::AudioBuffer<float> out (2, outLen + tail);
    out.clear();

    // 1. speed + pitch together (resampling)
    for (int c = 0; c < 2; ++c)
    {
        juce::LagrangeInterpolator interp;
        interp.process (speed, src.getReadPointer (juce::jmin (c, src.getNumChannels() - 1)), out.getWritePointer (c), outLen);
    }

    // 2. bass boost: low shelf at 100 Hz
    if (st.bassDb > 0.05f)
    {
        auto coeffs = juce::dsp::IIR::Coefficients<float>::makeLowShelf (sr, 100.0, 0.8f, juce::Decibels::decibelsToGain (st.bassDb));
        for (int c = 0; c < 2; ++c)
        {
            juce::dsp::IIR::Filter<float> f (coeffs);
            auto* d = out.getWritePointer (c);
            for (int i = 0; i < outLen; ++i)
                d[i] = f.processSample (d[i]);
        }
    }

    // 3. reverb
    if (st.reverb > 0.005f)
    {
        juce::Reverb verb;
        juce::Reverb::Parameters p;
        p.roomSize = 0.55f + 0.42f * st.reverb;
        p.damping = 0.45f;
        p.wetLevel = 0.45f * st.reverb;
        p.dryLevel = 1.0f - 0.3f * st.reverb;
        p.width = 1.0f;
        verb.setParameters (p);
        verb.setSampleRate (sr);
        verb.processStereo (out.getWritePointer (0), out.getWritePointer (1), out.getNumSamples());
    }

    // 4. volume, then keep peaks under 0 dB
    out.applyGain (st.volume);
    const float peak = out.getMagnitude (0, out.getNumSamples());
    if (peak > 0.98f)
        out.applyGain (0.98f / peak);
    return out;
}

//==============================================================================
NightcoreMaker::NightcoreMaker (AudioEngine& e, std::function<void (const juce::File&)> addToProject)
    : engine (e), onAddToProject (std::move (addToProject))
{
    sampleRate = engine.getSampleRate() > 0 ? engine.getSampleRate() : 48000.0;

    auto setupSlider = [this] (ValueSlider& s, juce::Label& l, const juce::String& name, double lo, double hi, double def, double step, const juce::String& suffix)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
        s.setRange (lo, hi, step);
        s.setValue (def, juce::dontSendNotification);
        s.setDoubleClickReturnValue (true, def);
        s.setTextValueSuffix (suffix);
        s.onValueChange = [this] { customBtn.setToggleState (true, juce::dontSendNotification); nightcoreBtn.setToggleState (false, juce::dontSendNotification);
                                   slowedBtn.setToggleState (false, juce::dontSendNotification); settingsChanged(); };
        l.setText (name, juce::dontSendNotification);
        l.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        l.setColour (juce::Label::textColourId, Theme::text);
        addAndMakeVisible (s);
        addAndMakeVisible (l);
    };
    setupSlider (speed,  speedL,  "Speed",      0.5, 1.5, 1.25, 0.01, "x");
    setupSlider (reverb, reverbL, "Reverb",     0.0, 100.0, 10.0, 1.0, " %");
    setupSlider (bass,   bassL,   "Bass boost", 0.0, 12.0, 0.0, 0.1, " dB");
    setupSlider (volume, volumeL, "Volume",     0.0, 100.0, 80.0, 1.0, " %");
    volume.onValueChange = [this] { engine.auditionGain = (float) (volume.getValue() / 80.0); };   // live, no re-render

    for (auto* b : { &nightcoreBtn, &slowedBtn, &customBtn })
    {
        b->setClickingTogglesState (false);
        b->setRadioGroupId (0);
        addAndMakeVisible (b);
    }
    nightcoreBtn.onClick = [this] { setMode (0); };
    slowedBtn.onClick = [this] { setMode (1); };
    customBtn.onClick = [this] { setMode (2); };
    nightcoreBtn.setToggleState (true, juce::dontSendNotification);

    loadBtn.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a song", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       engine.audioFormats.getWildcardForAllFormats());
        juce::Component::SafePointer<NightcoreMaker> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc)
        {
            if (safe != nullptr && fc.getResult().existsAsFile())
                safe->loadFile (fc.getResult());
        });
    };
    playBtn.onClick = [this] { togglePlay(); };
    exportBtn.onClick = [this] { exportResult(); };
    addBtn.onClick = [this] { addResultToProject(); };
    for (auto* b : { &loadBtn, &playBtn, &exportBtn, &addBtn })
        addAndMakeVisible (b);

    for (auto* c : getChildren())
        c->setWantsKeyboardFocus (false);

    setSize (620, 560);
    startTimerHz (30);
}

NightcoreMaker::~NightcoreMaker()
{
    alive.reset();
    engine.stopAudition();
    pool.removeAllJobs (true, 4000);
}

void NightcoreMaker::show (AudioEngine& engine, juce::Component* centreAround, std::function<void (const juce::File&)> addToProject)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new NightcoreMaker (engine, std::move (addToProject)));
    o.dialogTitle = "Nightcore Maker";
    o.dialogBackgroundColour = Theme::windowBg;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = centreAround;
    o.launchAsync();
}

NightcoreMaker::Settings NightcoreMaker::currentSettings() const
{
    Settings s;
    s.speed = speed.getValue();
    s.reverb = (float) reverb.getValue() / 100.0f;
    s.bassDb = (float) bass.getValue();
    s.volume = 1.0f;   // volume is applied live by the audition gain and at export
    return s;
}

void NightcoreMaker::setMode (int mode)
{
    nightcoreBtn.setToggleState (mode == 0, juce::dontSendNotification);
    slowedBtn.setToggleState (mode == 1, juce::dontSendNotification);
    customBtn.setToggleState (mode == 2, juce::dontSendNotification);
    if (mode == 0) { speed.setValue (1.25, juce::dontSendNotification); reverb.setValue (10.0, juce::dontSendNotification); }
    if (mode == 1) { speed.setValue (0.82, juce::dontSendNotification); reverb.setValue (45.0, juce::dontSendNotification); }
    settingsChanged();
}

//==============================================================================
bool NightcoreMaker::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && engine.canImport (juce::File (files[0]));
}

void NightcoreMaker::filesDropped (const juce::StringArray& files, int, int)
{
    if (! files.isEmpty())
        loadFile (juce::File (files[0]));
}

void NightcoreMaker::loadFile (const juce::File& f)
{
    auto clip = engine.loadClip (f, 0);   // decoded and resampled to the device rate
    if (clip == nullptr || clip->source == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Nightcore Maker", "Couldn't read " + f.getFileName());
        return;
    }
    engine.stopAudition();
    source = std::make_shared<juce::AudioBuffer<float>> (*clip->source);
    songName = f.getFileNameWithoutExtension();
    rendered = nullptr;
    peaks.clear();
    settingsChanged();
    lastChange = 0;   // render right away
}

void NightcoreMaker::settingsChanged()
{
    dirty = true;
    lastChange = juce::Time::getMillisecondCounter();
    repaint();
}

void NightcoreMaker::startRender()
{
    if (source == nullptr)
        return;
    dirty = false;
    rendering = true;
    const int gen = ++generation;
    auto src = source;
    const auto st = currentSettings();
    const double sr = sampleRate;
    std::weak_ptr<int> weak = alive;
    juce::Component::SafePointer<NightcoreMaker> safe (this);

    pool.addJob ([src, st, sr, gen, weak, safe]
    {
        auto out = std::make_shared<juce::AudioBuffer<float>> (render (*src, sr, st));

        // waveform overview: 600 columns of peak level
        std::vector<float> pk (600, 0.0f);
        const int per = juce::jmax (1, out->getNumSamples() / (int) pk.size());
        for (size_t i = 0; i < pk.size(); ++i)
        {
            const int s0 = (int) i * per;
            if (s0 >= out->getNumSamples()) break;
            pk[i] = out->getMagnitude (s0, juce::jmin (per, out->getNumSamples() - s0));
        }

        juce::MessageManager::callAsync ([out, pk = std::move (pk), gen, weak, safe]() mutable
        {
            if (weak.expired() || safe == nullptr || gen != safe->generation.load())
                return;
            auto& self = *safe;
            const auto oldLen = self.rendered != nullptr ? self.rendered->getNumSamples() : 0;
            self.rendered = out;
            self.peaks = std::move (pk);
            self.rendering = false;
            if (self.engine.isAuditioning() && oldLen > 0)
            {
                // keep the same spot in the song after a speed change
                const double frac = (double) self.engine.getAuditionPosition() / oldLen;
                self.engine.swapAuditionBuffer (out);
                self.engine.setAuditionPosition ((juce::int64) (frac * out->getNumSamples()));
            }
            self.repaint();
        });
    });
}

void NightcoreMaker::timerCallback()
{
    if (dirty && juce::Time::getMillisecondCounter() - lastChange > 250)
        startRender();
    playBtn.setButtonText (engine.isAuditioning() ? "Stop" : "Play");
    if (engine.isAuditioning() || rendering)
        repaint (waveArea().expanded (4, 20));
}

void NightcoreMaker::togglePlay()
{
    if (engine.isAuditioning())
    {
        engine.stopAudition();
        return;
    }
    if (rendered == nullptr)
        return;
    if (engine.isPlaying())
        engine.stop();
    engine.auditionGain = (float) (volume.getValue() / 80.0);
    auto from = engine.getAuditionPosition();
    if (from >= rendered->getNumSamples() - 1024)
        from = 0;
    engine.startAudition (rendered, from);
}

juce::Rectangle<int> NightcoreMaker::waveArea() const
{
    return getLocalBounds().reduced (24).withTrimmedTop (128).withHeight (110);
}

void NightcoreMaker::seekToX (int x)
{
    if (rendered == nullptr)
        return;
    const auto wa = waveArea();
    const double frac = juce::jlimit (0.0, 1.0, (double) (x - wa.getX()) / juce::jmax (1, wa.getWidth()));
    engine.setAuditionPosition ((juce::int64) (frac * rendered->getNumSamples()));
    repaint();
}

void NightcoreMaker::mouseDown (const juce::MouseEvent& e) { if (waveArea().contains (e.getPosition())) seekToX (e.x); }
void NightcoreMaker::mouseDrag (const juce::MouseEvent& e) { if (waveArea().expanded (0, 20).contains (e.getPosition())) seekToX (e.x); }

//==============================================================================
void NightcoreMaker::exportResult()
{
    if (rendered == nullptr)
        return;
    juce::PopupMenu m;
    m.addItem (1, "WAV");
    m.addItem (2, "MP3", Exporter::isAvailable (Exporter::Format::Mp3));
    juce::Component::SafePointer<NightcoreMaker> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportBtn), [safe] (int r)
    {
        if (safe == nullptr || r == 0)
            return;
        const auto format = r == 2 ? Exporter::Format::Mp3 : Exporter::Format::Wav;
        auto& self = *safe;
        const auto suffix = self.speed.getValue() >= 1.0 ? " (nightcore)" : " (slowed + reverb)";
        auto def = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (self.songName + suffix + Exporter::extension (format));
        self.chooser = std::make_unique<juce::FileChooser> ("Export", def, "*" + Exporter::extension (format));
        self.chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe, format] (const juce::FileChooser& fc)
        {
            if (safe == nullptr || fc.getResult() == juce::File())
                return;
            juce::AudioBuffer<float> out (*safe->rendered);
            out.applyGain ((float) (safe->volume.getValue() / 80.0));
            const float peak = out.getMagnitude (0, out.getNumSamples());
            if (peak > 0.99f) out.applyGain (0.99f / peak);
            const auto result = Exporter::write (out, safe->sampleRate, format, fc.getResult().withFileExtension (Exporter::extension (format)));
            juce::AlertWindow::showMessageBoxAsync (result.wasOk() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                                                    "Nightcore Maker", result.wasOk() ? "Exported." : result.getErrorMessage());
        });
    });
}

void NightcoreMaker::addResultToProject()
{
    if (rendered == nullptr || onAddToProject == nullptr)
        return;
    AudioEngine::getTakesFolder().createDirectory();
    const auto suffix = speed.getValue() >= 1.0 ? "_nightcore" : "_slowed";
    auto f = AudioEngine::getTakesFolder().getNonexistentChildFile (juce::File::createLegalFileName (songName) + suffix, ".wav");
    juce::AudioBuffer<float> out (*rendered);
    out.applyGain ((float) (volume.getValue() / 80.0));
    const auto result = Exporter::write (out, sampleRate, Exporter::Format::Wav, f);
    if (result.failed())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Nightcore Maker", result.getErrorMessage());
        return;
    }
    onAddToProject (f);
}

//==============================================================================
void NightcoreMaker::paint (juce::Graphics& g)
{
    Glass::background (g, getLocalBounds());

    auto r = getLocalBounds().reduced (24);
    auto top = r.removeFromTop (70);
    // logo: the mascot's face (she's the Nightcore Maker's icon)
    if (Mascot::face().isValid())
    {
        Mascot::drawBadge (g, top.removeFromLeft (56).toFloat().reduced (3.0f));
    }
    else
    {
        auto icon = top.removeFromLeft (56).toFloat().reduced (4.0f);
        g.setGradientFill (juce::ColourGradient (Theme::accent2, icon.getX(), icon.getY(), Theme::accent, icon.getRight(), icon.getBottom(), false));
        // crescent: the disc, minus a shifted disc (clip to the disc, fill everything outside the cut)
        juce::Path disc;
        disc.addEllipse (icon);
        juce::Graphics::ScopedSaveState keep (g);
        g.reduceClipRegion (disc);
        juce::Path crescent;
        crescent.addRectangle (icon.expanded (2.0f));
        crescent.addEllipse (icon.translated (icon.getWidth() * 0.38f, -icon.getHeight() * 0.18f));
        crescent.setUsingNonZeroWinding (false);
        g.fillPath (crescent);
    }
    top.removeFromLeft (10);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (28.0f, juce::Font::bold));
    g.drawText ("Nightcore Maker", top.removeFromTop (38), juce::Justification::centredLeft);
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (songName.isEmpty() ? "Load or drop a song to start. Make it nightcore, or slow it down with reverb."
                                   : songName, top, juce::Justification::centredLeft);

    // waveform
    const auto wa = waveArea();
    Glass::well (g, wa.toFloat(), 10.0f);
    if (! peaks.empty())
    {
        const float mid = (float) wa.getCentreY();
        const float half = wa.getHeight() * 0.45f;
        const float colW = (float) wa.getWidth() / (float) peaks.size();
        const double played = rendered != nullptr ? (double) engine.getAuditionPosition() / juce::jmax (1, rendered->getNumSamples()) : 0.0;
        for (size_t i = 0; i < peaks.size(); ++i)
        {
            const float x = (float) wa.getX() + colW * (float) i;
            const bool before = (double) i / (double) peaks.size() < played;
            g.setColour ((before ? Theme::accent2 : Theme::accent).withAlpha (rendering ? 0.35f : 0.85f));
            const float h = juce::jmax (1.0f, peaks[i] * half);
            g.fillRect (x, mid - h, juce::jmax (1.0f, colW - 0.5f), h * 2.0f);
        }
        if (rendered != nullptr)
        {
            const float px = (float) wa.getX() + (float) played * (float) wa.getWidth();
            g.setColour (Theme::playhead);
            g.drawLine (px, (float) wa.getY() + 4, px, (float) wa.getBottom() - 4, 1.5f);
            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (12.0f));
            const double len = rendered->getNumSamples() / sampleRate;
            g.drawText (formatTime (played * len), wa.getX(), wa.getBottom() + 2, 60, 16, juce::Justification::centredLeft);
            g.drawText (formatTime (len), wa.getRight() - 60, wa.getBottom() + 2, 60, 16, juce::Justification::centredRight);
        }
    }
    else
    {
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (source == nullptr ? "Drop an MP3 / WAV here" : "Rendering...", wa, juce::Justification::centred);
    }
    if (rendering && ! peaks.empty())
    {
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("Updating...", wa.reduced (8), juce::Justification::topRight);
    }
}

void NightcoreMaker::resized()
{
    auto r = getLocalBounds().reduced (24);
    r.removeFromTop (78);
    auto modes = r.removeFromTop (34);
    const int mw = 150;
    nightcoreBtn.setBounds (modes.removeFromLeft (mw).reduced (2));
    slowedBtn.setBounds (modes.removeFromLeft (mw).reduced (2));
    customBtn.setBounds (modes.removeFromLeft (100).reduced (2));
    loadBtn.setBounds (modes.removeFromRight (120).reduced (2));

    r.removeFromTop (16 + 110 + 26);   // waveform + time labels

    auto row = [&r] (juce::Label& l, juce::Slider& s)
    {
        auto line = r.removeFromTop (40);
        l.setBounds (line.removeFromLeft (110));
        s.setBounds (line.reduced (0, 6));
    };
    row (volumeL, volume);
    row (speedL, speed);
    row (reverbL, reverb);
    row (bassL, bass);

    auto buttons = r.removeFromBottom (36);
    playBtn.setBounds (buttons.removeFromLeft (120).reduced (2));
    addBtn.setBounds (buttons.removeFromRight (150).reduced (2));
    exportBtn.setBounds (buttons.removeFromRight (110).reduced (2));
}
