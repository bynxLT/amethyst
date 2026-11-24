#include "AudioEngine.h"
#include "Plugins/Builtins.h"

namespace
{
    constexpr int kMaxChannels = 8;

    float dbToGain (float db) { return db <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain (db); }
}

AudioEngine::AudioEngine()
{
    pluginFormats.addDefaultFormats();   // VST3 (JUCE_PLUGINHOST_VST3=1)
    audioFormats.registerBasicFormats(); // WAV, AIFF, FLAC, Ogg, MP3, Windows Media
    writerThread.startThread();
    ensureScratch (4096);
}

AudioEngine::~AudioEngine()
{
    aliveToken.reset();
    renderPool.removeAllJobs (true, 5000);
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();

    {
        const juce::ScopedLock sl (lock);
        for (auto* t : tracks)
            t->recorder.reset();
        bounceWriter.reset();
    }

    tracks.clear();
    for (auto& s : master.inserts)
        s->plugin.reset();
    writerThread.stopThread (2000);
}

juce::File AudioEngine::getAppFolder()
{
    const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    auto folder = docs.getChildFile ("AMETHYST");

    // One-time carry-over from the app's old name: bring channel presets along.
    // (Old takes/projects stay where they are, so saved projects keep pointing at valid files.)
    static bool migrated = false;
    if (! migrated)
    {
        migrated = true;
        const auto oldPresets = docs.getChildFile ("VocalDAW").getChildFile ("Channel Presets");
        const auto newPresets = folder.getChildFile ("Channel Presets");
        if (oldPresets.isDirectory() && ! newPresets.exists())
            oldPresets.copyDirectoryTo (newPresets);
    }
    return folder;
}

//==============================================================================
Track* AudioEngine::addTrack (const juce::String& name)
{
    auto* t = new Track (nextTrackId++, name);
    const juce::ScopedLock sl (lock);
    tracks.add (t);
    return t;
}

void AudioEngine::removeTrack (Track* t)
{
    std::unique_ptr<Track> doomed;
    {
        const juce::ScopedLock sl (lock);
        const int idx = tracks.indexOf (t);
        if (idx < 0)
            return;
        doomed.reset (tracks.removeAndReturn (idx));
    }
    // plug-ins and buffers are freed here, outside the audio lock
}

void AudioEngine::clearTracks()
{
    juce::OwnedArray<Track> old;
    {
        const juce::ScopedLock sl (lock);
        old.swapWith (tracks);
    }
}

bool AudioEngine::isValidChannel (const Track* t) const
{
    return t == &master || tracks.contains (const_cast<Track*> (t));
}

//==============================================================================
void AudioEngine::play()
{
    bounceDone = false;
    playing = true;
}

void AudioEngine::stop()
{
    std::vector<Track*> recorded;
    std::vector<std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>> finished;

    {
        const juce::ScopedLock sl (lock);
        playing = false;
        recording = false;

        for (auto* t : tracks)
        {
            if (t->recorder != nullptr)
            {
                finished.push_back (std::move (t->recorder));
                recorded.push_back (t);
            }
        }

        if (bounceWriter != nullptr)
            finished.push_back (std::move (bounceWriter));
    }

    finished.clear(); // flushes and closes the WAV files
    bounceDone = false;

    if (recorded.empty())
        return;

    // Compensate for the round-trip device latency so the take lines up with what the singer heard.
    juce::int64 latency = 0;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        latency = dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples();

    for (auto* t : recorded)
        if (auto clip = loadClip (t->recordingFile, recordStart - latency))
            addClip (*t, std::move (clip));
}

bool AudioEngine::record()
{
    if (recording)
        return true;

    getTakesFolder().createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");

    std::vector<std::pair<Track*, std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>>> writers;

    for (auto* t : tracks)
    {
        if (! t->armed)
            continue;

        auto file = getTakesFolder().getNonexistentChildFile (juce::File::createLegalFileName (t->name) + "_" + stamp, ".wav");

        if (auto w = createWavWriter (file, t->stereoInput ? 2 : 1))
        {
            t->recordingFile = file;
            writers.emplace_back (t, std::move (w));
        }
    }

    if (writers.empty())
        return false;

    const juce::ScopedLock sl (lock);
    for (auto& [t, w] : writers)
        t->recorder = std::move (w);

    recordStart = position.load();
    bounceDone = false;
    recording = true;
    playing = true;
    return true;
}

void AudioEngine::setTransportPosition (juce::int64 samplePos)
{
    if (! recording && bounceWriter == nullptr)
        position = juce::jmax<juce::int64> (0, samplePos);
}

juce::int64 AudioEngine::getSessionLengthSamples() const
{
    juce::int64 len = 0;
    for (auto* t : tracks)
        for (auto& c : t->clips)
            len = juce::jmax (len, c->startSample.load() + c->getLength());
    return len;
}

//==============================================================================
std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> AudioEngine::createWavWriter (const juce::File& file, int numChannels)
{
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr || ! stream->openedOk())
        return nullptr;

    juce::WavAudioFormat wav;
    auto* writer = wav.createWriterFor (stream.get(), getSampleRate(), (unsigned int) numChannels, 24, {}, 0);
    if (writer == nullptr)
        return nullptr;

    stream.release(); // now owned by the writer
    return std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer, writerThread, 65536);
}

bool AudioEngine::startBounce (const juce::File& wavFile)
{
    stop();

    const auto length = getSessionLengthSamples();
    if (length <= 0)
        return false;

    wavFile.deleteFile();
    auto w = createWavWriter (wavFile, 2);
    if (w == nullptr)
        return false;

    const juce::ScopedLock sl (lock);
    bounceWriter = std::move (w);
    bounceEnd = length + (juce::int64) (getSampleRate() * 2.0); // 2 s tail for reverbs/delays
    bounceDone = false;
    position = 0;
    playing = true;
    return true;
}

//==============================================================================
std::unique_ptr<juce::AudioPluginInstance> AudioEngine::createPlugin (const juce::PluginDescription& desc, juce::String& error)
{
    const double sr = sampleRate.load() > 0 ? sampleRate.load() : 48000.0;
    const int bs = blockSize.load() > 0 ? blockSize.load() : 512;

    std::unique_ptr<juce::AudioPluginInstance> instance;

    if (desc.pluginFormatName == Builtins::formatName)
    {
        if (isProUnlocked && ! isProUnlocked())
        {
            error = "Requires AMETHYST Pro - sign in from the Account button";
            return nullptr;
        }
        instance = Builtins::create (desc.fileOrIdentifier);
        if (instance == nullptr)
            error = "Unknown built-in effect: " + desc.name;
        else
            preparePlugin (*instance, sr, bs);
        return instance;
    }

    instance = pluginFormats.createPluginInstance (desc, sr, bs, error);

    // The plug-in may have moved since the preset was saved; try a scanned copy with the same name.
    if (instance == nullptr)
    {
        for (const auto& known : knownPlugins.getTypes())
        {
            if (known.name == desc.name && known.manufacturerName == desc.manufacturerName
                && known.fileOrIdentifier != desc.fileOrIdentifier)
            {
                juce::String err2;
                instance = pluginFormats.createPluginInstance (known, sr, bs, err2);
                if (instance != nullptr)
                    break;
            }
        }
    }

    if (instance != nullptr)
        preparePlugin (*instance, sr, bs);

    return instance;
}

void AudioEngine::preparePlugin (juce::AudioPluginInstance& p, double sr, int bs)
{
    const auto stereo = juce::AudioChannelSet::stereo();
    if (auto* in = p.getBus (true, 0))   in->setCurrentLayout (stereo);
    if (auto* out = p.getBus (false, 0)) out->setCurrentLayout (stereo);

    p.setPlayHead (this);
    p.setRateAndBufferSizeDetails (sr, bs);
    p.prepareToPlay (sr, bs);
}

int AudioEngine::channelsFor (juce::AudioPluginInstance& p)
{
    return juce::jlimit (1, kMaxChannels, juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()));
}

int AudioEngine::firstFreeSlot (const Track& t) const
{
    for (int i = 0; i < (int) t.inserts.size(); ++i)
        if (t.inserts[(size_t) i]->plugin == nullptr)
            return i;
    return -1;
}

std::unique_ptr<InsertSlot> AudioEngine::insertPlugin (Track& t, int index, std::unique_ptr<juce::AudioPluginInstance> p)
{
    auto slot = std::make_unique<InsertSlot>();
    slot->numChannels = channelsFor (*p);
    slot->plugin = std::move (p);

    if (index < 0 || index >= (int) t.inserts.size())
        return slot; // invalid slot: hand it straight back for deletion

    const juce::ScopedLock sl (lock);
    std::swap (t.inserts[(size_t) index], slot);
    return slot; // the old one
}

std::unique_ptr<InsertSlot> AudioEngine::removeInsert (Track& t, int index)
{
    if (index < 0 || index >= (int) t.inserts.size())
        return nullptr;

    auto empty = std::make_unique<InsertSlot>();
    const juce::ScopedLock sl (lock);
    std::swap (t.inserts[(size_t) index], empty);
    return empty; // the old one
}

void AudioEngine::moveInsert (Track& t, int from, int to)
{
    const int n = (int) t.inserts.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;

    const juce::ScopedLock sl (lock);
    std::swap (t.inserts[(size_t) from], t.inserts[(size_t) to]);
}

std::vector<std::unique_ptr<InsertSlot>> AudioEngine::replaceInserts (Track& t, std::vector<std::unique_ptr<InsertSlot>> newSlots)
{
    newSlots.resize ((size_t) Track::maxInserts);
    for (auto& s : newSlots)
    {
        if (s == nullptr)
            s = std::make_unique<InsertSlot>();
        else if (s->plugin != nullptr)
            s->numChannels = channelsFor (*s->plugin);
    }

    const juce::ScopedLock sl (lock);
    std::swap (t.inserts, newSlots);
    return newSlots; // the old chain
}

//==============================================================================
bool AudioEngine::canImport (const juce::File& f)
{
    return f.existsAsFile() && audioFormats.findFormatForFileExtension (f.getFileExtension()) != nullptr;
}

std::shared_ptr<AudioClip> AudioEngine::loadClip (const juce::File& file, juce::int64 startSample)
{
    std::unique_ptr<juce::AudioFormatReader> reader (audioFormats.createReaderFor (file));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return nullptr;

    const int numCh = (int) juce::jlimit (1u, 2u, reader->numChannels);
    const int len = (int) juce::jmin<juce::int64> (reader->lengthInSamples, std::numeric_limits<int>::max() - 16);

    juce::AudioBuffer<float> raw (numCh, len);
    raw.clear();
    reader->read (&raw, 0, len, 0, true, numCh > 1);

    auto clip = std::make_shared<AudioClip>();
    clip->file = file;
    clip->startSample = startSample;

    auto decoded = std::make_shared<juce::AudioBuffer<float>>();
    const double devRate = getSampleRate();
    if (devRate > 0 && std::abs (reader->sampleRate - devRate) > 1.0)
    {
        const double ratio = reader->sampleRate / devRate;
        const int outLen = juce::jmax (1, (int) ((len - 8) / ratio));
        decoded->setSize (numCh, outLen);
        for (int c = 0; c < numCh; ++c)
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, raw.getReadPointer (c), decoded->getWritePointer (c), outLen);
        }
    }
    else
    {
        *decoded = std::move (raw);
    }

    clip->source = decoded;
    clip->buffer = decoded;
    return clip;
}

bool AudioEngine::replaceClipSource (AudioClip& clip, const juce::File& file)
{
    auto fresh = loadClip (file, 0);
    if (fresh == nullptr)
        return false;

    clip.source = fresh->source;
    clip.file = file;
    renderNow (clip);
    return true;
}

void AudioEngine::setClipBuffer (AudioClip& clip, AudioClip::BufferPtr newBuffer)
{
    {
        const juce::ScopedLock sl (lock);
        std::swap (clip.buffer, newBuffer);
    }
    // old buffer released here, outside the audio lock
}

void AudioEngine::renderNow (AudioClip& clip)
{
    ++clip.renderGeneration;
    if (clip.source == nullptr)
        return;

    if (clip.settings.isNeutral())
        setClipBuffer (clip, clip.source);
    else
        setClipBuffer (clip, std::make_shared<juce::AudioBuffer<float>> (
                                 ClipFx::render (*clip.source, clip.settings, getSampleRate(), bpm.load())));
}

void AudioEngine::requestRender (const std::shared_ptr<AudioClip>& clip)
{
    if (clip == nullptr || clip->source == nullptr)
        return;

    const int generation = ++clip->renderGeneration;

    if (clip->settings.isNeutral())
    {
        setClipBuffer (*clip, clip->source);
        return;
    }

    auto source = clip->source;
    auto settings = clip->settings;
    const double sr = getSampleRate();
    const double tempo = bpm.load();
    std::weak_ptr<AudioClip> weakClip = clip;
    std::weak_ptr<int> alive = aliveToken;

    ++pendingRenders;
    renderPool.addJob ([this, source, settings, sr, tempo, weakClip, alive, generation]
    {
        auto rendered = std::make_shared<juce::AudioBuffer<float>> (ClipFx::render (*source, settings, sr, tempo));

        juce::MessageManager::callAsync ([this, rendered, weakClip, alive, generation]
        {
            if (alive.expired())
                return;
            --pendingRenders;
            if (auto c = weakClip.lock())
                if (c->renderGeneration == generation) // a newer edit supersedes this one
                    setClipBuffer (*c, rendered);
        });
    });
}

void AudioEngine::addClip (Track& t, std::shared_ptr<AudioClip> c)
{
    const juce::ScopedLock sl (lock);
    t.clips.push_back (std::move (c));
}

std::shared_ptr<AudioClip> AudioEngine::removeClip (Track& t, const AudioClip* c)
{
    const juce::ScopedLock sl (lock);
    for (auto it = t.clips.begin(); it != t.clips.end(); ++it)
    {
        if (it->get() == c)
        {
            auto old = std::move (*it);
            t.clips.erase (it);
            return old; // freed by the caller, outside the lock
        }
    }
    return nullptr;
}

//==============================================================================
void AudioEngine::ensureScratch (int numSamples)
{
    if (trackBuffer.getNumSamples() < numSamples || trackBuffer.getNumChannels() < kMaxChannels)
        trackBuffer.setSize (kMaxChannels, numSamples, false, true, true);
    if (masterBuffer.getNumSamples() < numSamples)
        masterBuffer.setSize (2, numSamples, false, true, true);
    if (dryBuffer.getNumSamples() < numSamples)
        dryBuffer.setSize (2, numSamples, false, true, true);
    if (scratchSize < numSamples)
    {
        silence.allocate ((size_t) numSamples, true);
        coverage.allocate ((size_t) numSamples, true);
        scratchSize = numSamples;
    }
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int bs = device->getCurrentBufferSizeSamples();
    sampleRate = sr;
    blockSize = bs;

    const juce::ScopedLock sl (lock);
    ensureScratch (juce::jmax (bs, 4096));

    forEachChannel ([&] (Track& t)
    {
        for (auto& s : t.inserts)
            if (s->plugin != nullptr)
            {
                preparePlugin (*s->plugin, sr, bs);
                s->numChannels = channelsFor (*s->plugin);
            }
    });
}

void AudioEngine::audioDeviceStopped()
{
    const juce::ScopedLock sl (lock);
    forEachChannel ([] (Track& t)
    {
        for (auto& s : t.inserts)
            if (s->plugin != nullptr)
                s->plugin->releaseResources();
    });
}

void AudioEngine::processChannel (Track& t, int numSamples)
{
    // Insert chain
    for (auto& slot : t.inserts)
    {
        if (slot->plugin == nullptr || slot->bypassed.load())
            continue;

        const float mix = juce::jlimit (0.0f, 1.0f, slot->mix.load());
        if (mix <= 0.0001f)
            continue;

        const bool blend = mix < 0.999f;
        if (blend)
        {
            dryBuffer.copyFrom (0, 0, trackBuffer, 0, 0, numSamples);
            dryBuffer.copyFrom (1, 0, trackBuffer, 1, 0, numSamples);
        }

        juce::AudioBuffer<float> view (trackBuffer.getArrayOfWritePointers(), slot->numChannels, numSamples);
        midiScratch.clear();

        {
            const juce::ScopedLock pluginLock (slot->plugin->getCallbackLock());
            if (! slot->plugin->isSuspended())
                slot->plugin->processBlock (view, midiScratch);
        }

        if (slot->plugin->getTotalNumOutputChannels() == 1)
            trackBuffer.copyFrom (1, 0, trackBuffer, 0, 0, numSamples);

        for (int c = 2; c < slot->numChannels; ++c)
            trackBuffer.clear (c, 0, numSamples);

        if (blend)
        {
            for (int c = 0; c < 2; ++c)
            {
                trackBuffer.applyGain (c, 0, numSamples, mix);
                trackBuffer.addFrom (c, 0, dryBuffer, c, 0, numSamples, 1.0f - mix);
            }
        }
    }

    // Fader + equal-power pan (smoothed to avoid zipper noise)
    const float gain = dbToGain (t.volumeDb.load());
    const float panAngle = (t.pan.load() + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
    const float gl = gain * std::cos (panAngle) * juce::MathConstants<float>::sqrt2;
    const float gr = gain * std::sin (panAngle) * juce::MathConstants<float>::sqrt2;

    trackBuffer.applyGainRamp (0, 0, numSamples, t.smoothedGainL, gl);
    trackBuffer.applyGainRamp (1, 0, numSamples, t.smoothedGainR, gr);
    t.smoothedGainL = gl;
    t.smoothedGainR = gr;

    t.meterL = juce::jmax (t.meterL.load(), trackBuffer.getMagnitude (0, 0, numSamples));
    t.meterR = juce::jmax (t.meterR.load(), trackBuffer.getMagnitude (1, 0, numSamples));
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* input, int numInputs,
                                                    float* const* output, int numOutputs,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    for (int c = 0; c < numOutputs; ++c)
        if (output[c] != nullptr)
            juce::FloatVectorOperations::clear (output[c], numSamples);

    const juce::ScopedLock sl (lock);
    ensureScratch (numSamples);

    const bool isPlay = playing.load();
    const bool isRec = recording.load();
    const juce::int64 pos = position.load();
    const double sr = sampleRate.load();

    masterBuffer.clear (0, numSamples);

    bool anySolo = false;
    for (auto* t : tracks)
        anySolo = anySolo || t->solo.load();

    auto getInput = [&] (int idx) -> const float*
    {
        return (idx >= 0 && idx < numInputs) ? input[idx] : nullptr;
    };

    for (auto* t : tracks)
    {
        for (int c = 0; c < kMaxChannels; ++c)
            trackBuffer.clear (c, 0, numSamples);

        const int inCh = t->inputChannel.load();
        const float* inL = getInput (inCh);
        const float* inR = t->stereoInput.load() ? getInput (inCh + 1) : inL;

        // 1. Record the dry input
        if (isRec && t->recorder != nullptr)
        {
            const float* chans[2] = { inL != nullptr ? inL : silence.get(),
                                      inR != nullptr ? inR : silence.get() };
            t->recorder->write (chans, numSamples);
        }

        // 2. Play clips (newest on top). Skipped while this track records a new take.
        if (isPlay && ! t->clips.empty() && t->recorder == nullptr)
        {
            std::fill (coverage.get(), coverage.get() + numSamples, (juce::uint8) 0);
            auto* dl = trackBuffer.getWritePointer (0);
            auto* dr = trackBuffer.getWritePointer (1);

            for (auto it = t->clips.rbegin(); it != t->clips.rend(); ++it)
            {
                const auto& clip = **it;
                if (clip.buffer == nullptr || ! clip.enabled.load())
                    continue;

                const auto start = clip.startSample.load();
                const auto s0 = juce::jmax (pos, start);
                const auto s1 = juce::jmin (pos + numSamples, start + clip.getLength());
                if (s1 <= s0)
                    continue;

                const auto& cb = *clip.buffer;
                const float* srcL = cb.getReadPointer (0);
                const float* srcR = cb.getReadPointer (cb.getNumChannels() > 1 ? 1 : 0);

                for (auto p = s0; p < s1; ++p)
                {
                    const int i = (int) (p - pos);
                    if (coverage[i] != 0)
                        continue;
                    coverage[i] = 1;
                    const auto si = p - start;
                    dl[i] = srcL[si];
                    dr[i] = srcR[si];
                }
            }
        }

        // 3. Input monitoring (goes through the insert chain so the singer hears the FX)
        if (t->monitor.load())
        {
            if (inL != nullptr) trackBuffer.addFrom (0, 0, inL, numSamples);
            if (inR != nullptr) trackBuffer.addFrom (1, 0, inR, numSamples);
        }

        // 4. Inserts, fader, pan, meters
        processChannel (*t, numSamples);

        const bool audible = ! t->mute.load() && (! anySolo || t->solo.load());
        if (audible)
        {
            masterBuffer.addFrom (0, 0, trackBuffer, 0, 0, numSamples);
            masterBuffer.addFrom (1, 0, trackBuffer, 1, 0, numSamples);
        }
    }

    // Master bus: run it through its own insert chain + fader
    for (int c = 0; c < kMaxChannels; ++c)
        trackBuffer.clear (c, 0, numSamples);
    trackBuffer.copyFrom (0, 0, masterBuffer, 0, 0, numSamples);
    trackBuffer.copyFrom (1, 0, masterBuffer, 1, 0, numSamples);
    processChannel (master, numSamples);
    if (master.mute.load())
        trackBuffer.clear (0, numSamples);
    masterBuffer.copyFrom (0, 0, trackBuffer, 0, 0, numSamples);
    masterBuffer.copyFrom (1, 0, trackBuffer, 1, 0, numSamples);

    // Bounce (before the click is added)
    if (bounceWriter != nullptr && isPlay)
    {
        const float* chans[2] = { masterBuffer.getReadPointer (0), masterBuffer.getReadPointer (1) };
        bounceWriter->write (chans, numSamples);
        if (pos + numSamples >= bounceEnd)
            bounceDone = true;
    }

    // Metronome
    if (isPlay && metronomeOn.load() && bounceWriter == nullptr)
    {
        const double beatLen = sr * 60.0 / juce::jmax (20.0, bpm.load());
        const double clickLen = sr * 0.03;
        auto* l = masterBuffer.getWritePointer (0);
        auto* r = masterBuffer.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            const double p = (double) (pos + i);
            const auto beat = (juce::int64) std::floor (p / beatLen);
            const double within = p - (double) beat * beatLen;
            if (within < clickLen)
            {
                const double freq = (beat % 4 == 0) ? 1600.0 : 1000.0;
                const float env = 1.0f - (float) (within / clickLen);
                const float s = 0.3f * env * (float) std::sin (juce::MathConstants<double>::twoPi * freq * within / sr);
                l[i] += s;
                r[i] += s;
            }
        }
    }

    if (numOutputs >= 2)
    {
        if (output[0] != nullptr) juce::FloatVectorOperations::copy (output[0], masterBuffer.getReadPointer (0), numSamples);
        if (output[1] != nullptr) juce::FloatVectorOperations::copy (output[1], masterBuffer.getReadPointer (1), numSamples);
    }
    else if (numOutputs == 1 && output[0] != nullptr)
    {
        juce::FloatVectorOperations::copyWithMultiply (output[0], masterBuffer.getReadPointer (0), 0.5f, numSamples);
        juce::FloatVectorOperations::addWithMultiply (output[0], masterBuffer.getReadPointer (1), 0.5f, numSamples);
    }

    if (isPlay)
    {
        auto expected = pos;
        position.compare_exchange_strong (expected, pos + numSamples); // a UI seek during this block wins
    }
}

//==============================================================================
juce::Optional<juce::AudioPlayHead::PositionInfo> AudioEngine::getPosition() const
{
    PositionInfo info;
    const auto pos = position.load();
    const double sr = juce::jmax (1.0, sampleRate.load());
    const double tempo = bpm.load();
    const double ppq = (double) pos / sr * tempo / 60.0;

    info.setTimeInSamples (pos);
    info.setTimeInSeconds ((double) pos / sr);
    info.setBpm (tempo);
    info.setTimeSignature (TimeSignature { 4, 4 });
    info.setPpqPosition (ppq);
    info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
    info.setIsPlaying (playing.load());
    info.setIsRecording (recording.load());
    return info;
}
