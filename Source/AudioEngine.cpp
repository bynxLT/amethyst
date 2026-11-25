#include "AudioEngine.h"
#include "Plugins/Builtins.h"
#include "Loudness.h"

struct AudioEngine::LoudnessHolder
{
    Loudness::Meter meter;
    int publish = 0;
};

namespace
{
    constexpr int kMaxChannels = 8;
    constexpr int kMp3InfoFrameSamples = 0;   // samples JUCE decodes from the Xing/Info frame (measured)

    float dbToGain (float db) { return db <= -60.0f ? 0.0f : juce::Decibels::decibelsToGain (db); }
}

AudioEngine::AudioEngine()
{
    AmethystPlugin::sidechainOwner() = this;
    AmethystPlugin::sidechainSource() = [this] (int number, int n, const float*& l, const float*& r)
    {
        const int idx = number - 1;
        if (idx < 0 || idx >= kMaxSidechainTracks || sidechainBus.getNumSamples() < n)
            return false;
        l = sidechainBus.getReadPointer (idx * 2);
        r = sidechainBus.getReadPointer (idx * 2 + 1);
        return true;
    };
    pluginFormats.addDefaultFormats();   // VST3 (JUCE_PLUGINHOST_VST3=1)
    audioFormats.registerBasicFormats(); // WAV, AIFF, FLAC, Ogg, MP3, Windows Media
    writerThread.startThread();
    ensureScratch (4096);
    masterLoudness = std::make_unique<LoudnessHolder>();
    masterLoudness->meter.prepare (48000.0);
    retroRing.setSize (2, 48000 * 60);
    retroRing.clear();
}

AudioEngine::~AudioEngine()
{
    if (AmethystPlugin::sidechainOwner() == this)
    {
        AmethystPlugin::sidechainSource() = nullptr;
        AmethystPlugin::sidechainOwner() = nullptr;
    }
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
        for (auto* other : tracks)        // lanes of a deleted channel go back to their own channel
        {
            if (other->playThrough == t)
                other->playThrough = nullptr;
            if (other->automation != nullptr && other->automation->target == t)
                other->automation->target = nullptr;   // its automation clips go idle
        }
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
    previewClip = nullptr;
    std::vector<Track*> recorded;
    std::vector<std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>> finished;

    {
        const juce::ScopedLock sl (lock);
        playing = false;
        recording = false;
        punchRecording = false;

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
    // Plus the plug-in delay compensation: the backing was heard that much later too.
    juce::int64 latency = recordPdc;
    if (auto* dev = deviceManager.getCurrentAudioDevice())
        latency += dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples();

    for (auto* t : recorded)
        if (auto clip = loadClip (t->recordingFile, recordStart - latency))
        {
            newTakes.emplace_back (t, clip);
            addClip (*t, std::move (clip));
        }
}

Track* AudioEngine::findTrackForTake (Track* source, const AudioClip& take) const
{
    const auto s = take.startSample.load(), e = take.getEnd();
    auto overlaps = [&] (const Track& t)
    {
        for (auto& c : t.clips)
            if (c.get() != &take && c->startSample.load() < e && c->getEnd() > s)
                return true;
        return false;
    };

    if (! overlaps (*source))
        return source;

    // The take keeps the source channel's sound, so it may only land on one of the source's
    // take lanes, or on a completely empty track (no clips, no plug-ins, not routed elsewhere).
    auto* channel = source->playThrough != nullptr ? source->playThrough : source;
    auto usable = [&] (const Track& t)
    {
        if (t.isAutomation() || t.armed.load() || t.mute.load() || overlaps (t))
            return false;
        if (t.playThrough == channel)
            return true;
        return t.playThrough == nullptr && t.clips.empty() && t.countPlugins() == 0
               && std::none_of (tracks.begin(), tracks.end(), [&] (const Track* o) { return o->playThrough == &t; });
    };

    bool below = false;
    for (auto* t : tracks)
    {
        if (t == source) { below = true; continue; }
        if (below && usable (*t))
            return t;
    }
    return nullptr;
}

void AudioEngine::setPlayThrough (Track& lane, Track* channel)
{
    if (channel == &lane || channel == &master)
        channel = nullptr;
    if (channel != nullptr && channel->playThrough != nullptr)
        channel = channel->playThrough; // never chain lanes

    const juce::ScopedLock sl (lock);
    lane.playThrough = channel;
    // anything that was routed into this lane follows it to its new channel
    for (auto* t : tracks)
        if (t->playThrough == &lane)
            t->playThrough = channel;
}

void AudioEngine::previewClipSolo (const AudioClip& c)
{
    if (recording)
        return;
    previewClip = &c;
    position = juce::jmax<juce::int64> (0, c.startSample.load());
    bounceDone = false;
    playing = true;
}

void AudioEngine::setClips (Track& t, std::vector<std::shared_ptr<AudioClip>> clips)
{
    {
        const juce::ScopedLock sl (lock);
        std::swap (t.clips, clips);
    }
    // the old list is released here, outside the audio lock
}

bool AudioEngine::record (juce::int64 countInSamples)
{
    if (recording)
        return true;

    getTakesFolder().createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");

    std::vector<std::pair<Track*, std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>>> writers;

    for (auto* t : tracks)
    {
        if (! t->armed || t->isAutomation())
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

    const auto playhead = position.load();
    const bool punch = punchEnabled.load() && hasPunchRegion();
    recordStart = punch ? punchIn.load() : playhead;
    recordEnd = punch ? punchOut.load() : std::numeric_limits<juce::int64>::max();
    countInSamples = juce::jmax<juce::int64> (0, countInSamples);
    countInFrom = recordStart - countInSamples;

    // Where the transport starts: the count-in, or for punch at least a bar of lead-in
    // (or from the playhead, if you parked it earlier on purpose).
    juce::int64 from = recordStart - countInSamples;
    if (punch)
    {
        const auto bar = (juce::int64) (4.0 * 60.0 / juce::jmax (20.0, bpm.load()) * sampleRate.load());
        from = playhead < recordStart ? juce::jmin (playhead, recordStart - countInSamples) : recordStart - juce::jmax (countInSamples, bar);
    }
    position = from;
    recordPdc = computeMaxLatency (true);
    punchRecording = punch;
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
        const int needed = Builtins::requiredTier (desc.fileOrIdentifier);
        if (unlockedTier && unlockedTier() < needed)
        {
            error = "Requires " + Builtins::tierName (needed) + " - sign in from the Account button";
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

    // MP3: drop the encoder delay + decoder delay at the start and the padding at the end, so the
    // song starts exactly where it should (uses the gapless header; files without one are left alone)
    if (trimMp3Padding.load() && file.hasFileExtension ("mp3"))
    {
        const auto g = readMp3Gapless (file);
        if (g.found && g.trimStart > 0 && g.trimStart < len)
        {
            const int keep = (int) juce::jlimit<juce::int64> (1, len - g.trimStart, g.validLength > 0 ? g.validLength : len - g.trimStart);
            juce::AudioBuffer<float> trimmed (numCh, keep);
            for (int c = 0; c < numCh; ++c)
                trimmed.copyFrom (c, 0, raw, c, g.trimStart, keep);
            raw = std::move (trimmed);
        }
    }
    const int rawLen = raw.getNumSamples();

    auto clip = std::make_shared<AudioClip>();
    clip->file = file;
    clip->startSample = startSample;
    clip->lossless = isLossless (file);

    auto decoded = std::make_shared<juce::AudioBuffer<float>>();
    const double devRate = getSampleRate();
    if (devRate > 0 && std::abs (reader->sampleRate - devRate) > 1.0)
    {
        const double ratio = reader->sampleRate / devRate;
        const int outLen = juce::jmax (1, (int) ((rawLen - 8) / ratio));
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
                                 ClipFx::render (*clip.source, renderSettingsFor (clip), getSampleRate(), bpm.load())));
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
    auto settings = renderSettingsFor (*clip);
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

void AudioEngine::trimClipStart (AudioClip& c, juce::int64 newStart)
{
    const juce::ScopedLock sl (lock); // start/offset/length change together
    const auto start = c.startSample.load(), end = c.getEnd();
    const auto earliest = start - c.getOffset();                       // can't reveal audio before the buffer begins
    newStart = juce::jlimit (earliest, juce::jmax (earliest, end - (juce::int64) minClipSamples), newStart);

    c.trimOffset = c.getOffset() + (newStart - start);
    c.startSample = newStart;
    c.trimLength = end - newStart;
}

void AudioEngine::trimClipEnd (AudioClip& c, juce::int64 newEnd)
{
    const juce::ScopedLock sl (lock);
    const auto start = c.startSample.load();
    const auto latest = start + (c.getBufferLength() - c.getOffset()); // can't play past the buffer's end
    newEnd = juce::jlimit (juce::jmin (latest, start + (juce::int64) minClipSamples), latest, newEnd);
    c.trimLength = newEnd - start;
}

std::shared_ptr<AudioClip> AudioEngine::splitClip (Track& t, const std::shared_ptr<AudioClip>& c, juce::int64 at)
{
    if (c == nullptr)
        return nullptr;

    const auto start = c->startSample.load(), end = c->getEnd();
    if (at - start < minClipSamples || end - at < minClipSamples)
        return nullptr; // too close to an edge

    auto right = std::make_shared<AudioClip>();
    right->source = c->source;
    right->buffer = c->buffer;          // shared, read-only
    right->file = c->file;
    right->lossless = c->lossless;
    right->settings = c->settings;
    right->enabled = c->enabled.load();
    right->startSample = at;
    right->trimOffset = c->getOffset() + (at - start);
    right->trimLength = end - at;

    const juce::ScopedLock sl (lock);
    c->trimLength = at - start;
    auto it = std::find (t.clips.begin(), t.clips.end(), c);
    t.clips.insert (it == t.clips.end() ? it : std::next (it), right); // same layer as the original
    return right;
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
    if (inputScratch.getNumSamples() < numSamples)
        inputScratch.setSize (2, numSamples, false, true, true);
    if (sidechainBus.getNumSamples() < numSamples)
        sidechainBus.setSize (kMaxSidechainTracks * 2, numSamples, false, true, true);
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
    masterLoudness->meter.prepare (sr);
    if (retroRing.getNumSamples() != (int) (sr * 60.0))
    {
        retroRing.setSize (2, (int) (sr * 60.0));
        retroRing.clear();
        retroPos = 0;
        retroWritten = 0;
    }

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

void AudioEngine::renderClips (Track& t, juce::int64 pos, int numSamples)
{
    // Layer mode sums overlapping takes; otherwise the newest clip wins.
    const bool layer = t.layerTakes.load();
    if (! layer)
        std::fill (coverage.get(), coverage.get() + numSamples, (juce::uint8) 0);
    auto* dl = trackBuffer.getWritePointer (0);
    auto* dr = trackBuffer.getWritePointer (1);
    const auto* preview = previewClip.load();

    for (auto it = t.clips.rbegin(); it != t.clips.rend(); ++it)
    {
        const auto& clip = **it;
        if (clip.buffer == nullptr || (preview != nullptr ? preview != &clip : ! clip.enabled.load()))
            continue;

        const auto start = clip.startSample.load();
        const auto len = clip.getLength();
        const auto s0 = juce::jmax (pos, start);
        const auto s1 = juce::jmin (pos + numSamples, start + len);
        if (s1 <= s0)
            continue;

        const auto& cb = *clip.buffer;
        const auto offset = clip.getOffset();
        const float* srcL = cb.getReadPointer (0);
        const float* srcR = cb.getReadPointer (cb.getNumChannels() > 1 ? 1 : 0);

        // short fades at the played edges so trims and cuts never click. Lossless files keep their own
        // edges exactly as recorded: only an edge you cut or trimmed gets a fade.
        const auto fadeLen = juce::jmax<juce::int64> (1, juce::jmin<juce::int64> (96, len / 2));
        const bool untouched = clip.lossless && losslessUntouched.load();
        const bool fadeStart = ! untouched || offset > 0;
        const bool fadeEnd = ! untouched || offset + len < clip.getBufferLength();

        for (auto p = s0; p < s1; ++p)
        {
            const int i = (int) (p - pos);
            if (! layer)
            {
                if (coverage[i] != 0)
                    continue;
                coverage[i] = 1;
            }

            const auto rel = p - start;
            float g = 1.0f;
            if (fadeStart && rel < fadeLen)        g = (float) rel / (float) fadeLen;
            if (fadeEnd && len - rel <= fadeLen)   g = juce::jmin (g, (float) (len - rel) / (float) fadeLen);

            const auto si = offset + rel;
            dl[i] += srcL[si] * g;
            dr[i] += srcR[si] * g;
        }
    }
}

void AudioEngine::processChannel (Track& t, int numSamples, bool skipLatent)
{
    // Channel strip: low cut and polarity, before the plug-ins
    const float lowCut = t.lowCutHz.load();
    if (lowCut > 1.0f)
    {
        if (std::abs (lowCut - t.lowCutDesigned) > 0.01f)
        {
            const auto coeffs = juce::IIRCoefficients::makeHighPass (juce::jmax (8000.0, sampleRate.load()), lowCut, 0.707);
            for (auto& flt : t.lowCutFilter) flt.setCoefficients (coeffs);
            if (t.lowCutDesigned <= 0.0f)
                for (auto& flt : t.lowCutFilter) flt.reset();
            t.lowCutDesigned = lowCut;
        }
        t.lowCutFilter[0].processSamples (trackBuffer.getWritePointer (0), numSamples);
        t.lowCutFilter[1].processSamples (trackBuffer.getWritePointer (1), numSamples);
    }
    else
        t.lowCutDesigned = -1.0f;
    if (t.phaseInvert.load())
    {
        trackBuffer.applyGain (0, 0, numSamples, -1.0f);
        trackBuffer.applyGain (1, 0, numSamples, -1.0f);
    }

    // Insert chain
    for (auto& slot : t.inserts)
    {
        if (slot->plugin == nullptr || slot->bypassed.load())
            continue;
        if (skipLatent && slot->plugin->getLatencySamples() > 0)
            continue;   // low-latency monitoring

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

    const float pkL = trackBuffer.getMagnitude (0, 0, numSamples), pkR = trackBuffer.getMagnitude (1, 0, numSamples);
    t.meterL = juce::jmax (t.meterL.load(), pkL);
    t.meterR = juce::jmax (t.meterR.load(), pkR);
    if (pkL > 1.0f || pkR > 1.0f)
        t.clipped = true;
}

int AudioEngine::channelLatency (const Track& t, bool skipLatent) const
{
    int total = 0;
    for (auto& slot : t.inserts)
    {
        if (slot->plugin == nullptr || slot->bypassed.load() || slot->mix.load() <= 0.0001f)
            continue;
        const int l = slot->plugin->getLatencySamples();
        if (skipLatent && l > 0)
            continue;
        total += juce::jmax (0, l);
    }
    return juce::jmin (total, Track::pdcSize - 1);
}

int AudioEngine::computeMaxLatency (bool recordingNow) const
{
    const bool lowLat = recordingNow && lowLatencyMonitoring.load();
    int maxL = 0;
    for (auto* t : tracks)
    {
        if (t->playThrough != nullptr || t->isAutomation())
            continue;
        maxL = juce::jmax (maxL, channelLatency (*t, lowLat && t->monitor.load() && t->armed.load()));
    }
    return maxL;
}

void AudioEngine::delayChannel (Track& t, int numSamples, int delay)
{
    constexpr int mask = Track::pdcSize - 1;
    delay = juce::jlimit (0, mask, delay);
    for (int c = 0; c < 2; ++c)
    {
        auto* d = trackBuffer.getWritePointer (c);
        auto& line = t.pdcLine[c];
        int p = t.pdcPos;
        for (int i = 0; i < numSamples; ++i)
        {
            line[(size_t) p] = d[i];
            d[i] = line[(size_t) ((p - delay) & mask)];
            p = (p + 1) & mask;
        }
    }
    t.pdcPos = (t.pdcPos + numSamples) & mask;
}

void AudioEngine::writeRetro (const float* inL, const float* inR, int numSamples, float gain, bool playingNow, juce::int64 pos)
{
    const int len = retroRing.getNumSamples();
    if (len <= 0)
        return;
    // a jump in time (seek, loop, start / stop) begins a new capture
    if (playingNow != (retroTimelineEnd >= 0) || (playingNow && retroTimelineEnd != pos))
        retroWritten = 0;
    auto* l = retroRing.getWritePointer (0);
    auto* r = retroRing.getWritePointer (1);
    for (int i = 0; i < numSamples; ++i)
    {
        l[retroPos] = inL != nullptr ? inL[i] * gain : 0.0f;
        r[retroPos] = inR != nullptr ? inR[i] * gain : 0.0f;
        if (++retroPos >= len) retroPos = 0;
    }
    retroWritten += numSamples;
    retroTimelineEnd = playingNow ? pos + numSamples : -1;
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* input, int numInputs,
                                                    float* const* output, int numOutputs,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    if (offlineActive.load())
    {
        // an export is rendering the song on another thread: the speakers stay silent meanwhile
        for (int c = 0; c < numOutputs; ++c)
            if (output[c] != nullptr)
                juce::FloatVectorOperations::clear (output[c], numSamples);
        return;
    }
    processAudio (input, numInputs, output, numOutputs, numSamples);
}

juce::Result AudioEngine::renderSong (juce::AudioBuffer<float>& result, std::function<bool (float)> onProgress)
{
    if (recording)
        return juce::Result::fail ("Stop recording before exporting.");

    const auto length = getSessionLengthSamples();
    if (length <= 0)
        return juce::Result::fail ("There's nothing to export yet - record or import something first.");

    const double sr = getSampleRate();
    const auto total = length + (juce::int64) (sr * 3.0); // 3 s tail for reverbs and delays
    if (total > (juce::int64) std::numeric_limits<int>::max())
        return juce::Result::fail ("The song is too long to export.");

    stop();
    const auto savedPosition = position.load();
    result.setSize (2, (int) total);
    result.clear();

    const int block = juce::jmax (64, blockSize.load());
    juce::AudioBuffer<float> out (2, block);

    // Delay compensation delays every channel by the slowest one's latency: render that much longer
    // and drop it from the start, so the export starts exactly at 0.
    int pdc = 0;
    {
        const juce::ScopedLock sl (lock);
        pdc = computeMaxLatency (false);
    }

    auto setNonRealtime = [this] (bool offline)
    {
        const juce::ScopedLock sl (lock);
        forEachChannel ([offline] (Track& t)
        {
            for (auto& s : t.inserts)
                if (s->plugin != nullptr)
                    s->plugin->setNonRealtime (offline);
        });
    };

    setNonRealtime (true); // plug-ins may use higher-quality modes while exporting
    offlineActive = true;
    position = 0;
    playing = true;

    bool cancelled = false;
    for (juce::int64 done = 0; done < total + pdc; done += block)
    {
        const int n = (int) juce::jmin<juce::int64> (block, total + pdc - done);
        float* outs[2] = { out.getWritePointer (0), out.getWritePointer (1) };
        processAudio (nullptr, 0, outs, 2, n);
        // keep the part that lands in [0, total) after removing the compensation delay
        const auto dst = done - pdc;
        const auto from = juce::jmax<juce::int64> (0, -dst);
        const auto count = juce::jmin<juce::int64> (n - from, total - (dst + from));
        if (count > 0)
        {
            result.copyFrom (0, (int) (dst + from), out, 0, (int) from, (int) count);
            result.copyFrom (1, (int) (dst + from), out, 1, (int) from, (int) count);
        }

        if (onProgress && ! onProgress ((float) ((double) (done + n) / (double) (total + pdc))))
        {
            cancelled = true;
            break;
        }
    }

    playing = false;
    position = savedPosition;
    offlineActive = false;
    setNonRealtime (false);
    return cancelled ? juce::Result::fail ("Export cancelled.") : juce::Result::ok();
}

void AudioEngine::processAudio (const float* const* input, int numInputs, float* const* output, int numOutputs, int numSamples)
{
    // Loop: if the loop end falls inside this block, play up to it, jump back, and play the rest.
    const auto pos = position.load(), ls = loopStart.load(), le = loopEnd.load();
    const bool wrap = playing.load() && ! recording.load() && loopEnabled.load() && hasLoopRegion() && ! offlineActive.load()
                      && previewClip.load() == nullptr && pos < le && pos + numSamples > le;
    if (! wrap)
    {
        processSegment (input, numInputs, output, numOutputs, numSamples);
        return;
    }
    const int first = (int) (le - pos);
    processSegment (input, numInputs, output, numOutputs, first);
    position = ls;
    constexpr int maxCh = 64;
    const float* ins[maxCh] = {};
    float* outs[maxCh] = {};
    const int ni = juce::jmin (numInputs, maxCh), no = juce::jmin (numOutputs, maxCh);
    for (int i = 0; i < ni; ++i) ins[i] = input != nullptr && input[i] != nullptr ? input[i] + first : nullptr;
    for (int i = 0; i < no; ++i) outs[i] = output[i] != nullptr ? output[i] + first : nullptr;
    processSegment (ins, ni, outs, no, numSamples - first);
}

void AudioEngine::processSegment (const float* const* input, int numInputs, float* const* output, int numOutputs, int numSamples)
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

    // Plug-in delay compensation target for this block
    const bool lowLatNow = isRec && lowLatencyMonitoring.load();
    const int maxLat = computeMaxLatency (isRec);
    maxLatency = maxLat;

    // Input level of armed / monitored tracks (for the input level check)
    for (auto* t : tracks)
    {
        if ((! t->armed.load() && ! t->monitor.load()) || t->isAutomation())
            continue;
        const int inCh = t->inputChannel.load();
        float pk = 0.0f;
        if (auto* inL = getInput (inCh)) pk = juce::FloatVectorOperations::findMaximum (inL, numSamples) > -juce::FloatVectorOperations::findMinimum (inL, numSamples)
                                              ? juce::FloatVectorOperations::findMaximum (inL, numSamples) : -juce::FloatVectorOperations::findMinimum (inL, numSamples);
        if (t->stereoInput.load())
            if (auto* inR = getInput (inCh + 1))
                pk = juce::jmax (pk, juce::jmax (juce::FloatVectorOperations::findMaximum (inR, numSamples), -juce::FloatVectorOperations::findMinimum (inR, numSamples)));
        pk *= juce::Decibels::decibelsToGain (t->inputGainDb.load());
        if (pk > t->inputPeak.load())
            t->inputPeak = pk;
    }

    // Retrospective record: keep the armed (else monitored) track's input while not recording
    if (! isRec && ! offlineActive.load())
    {
        Track* src = nullptr;
        for (auto* t : tracks) if (t->armed.load() && ! t->isAutomation()) { src = t; break; }
        if (src == nullptr)
            for (auto* t : tracks) if (t->monitor.load() && ! t->isAutomation()) { src = t; break; }
        if (src != nullptr && numInputs > 0)
        {
            const int inCh = src->inputChannel.load();
            const float* inL = getInput (inCh);
            const float* inR = src->stereoInput.load() ? getInput (inCh + 1) : inL;
            retroTrack = src;
            writeRetro (inL, inR, numSamples, juce::Decibels::decibelsToGain (src->inputGainDb.load()), isPlay, pos);
        }
    }

    // 1. Record the dry input of every armed track
    if (isRec)
    {
        for (auto* t : tracks)
        {
            if (t->recorder == nullptr)
                continue;
            // only the part of this block inside [recordStart, recordEnd) goes into the take
            // (count-in and punch lead-in are heard, not recorded)
            const auto w0 = juce::jmax (pos, recordStart), w1 = juce::jmin (pos + numSamples, recordEnd);
            if (w1 <= w0)
                continue;
            const int skip = (int) (w0 - pos), count = (int) (w1 - w0);

            const int inCh = t->inputChannel.load();
            const float* inL = getInput (inCh);
            const float* inR = t->stereoInput.load() ? getInput (inCh + 1) : inL;
            const float* chans[2] = { inL != nullptr ? inL + skip : silence.get(), inR != nullptr ? inR + skip : silence.get() };
            const float gain = juce::Decibels::decibelsToGain (t->inputGainDb.load());
            if (std::abs (gain - 1.0f) > 1.0e-4f)
            {
                // input gain is part of the take
                for (int c = 0; c < 2; ++c)
                    juce::FloatVectorOperations::copyWithMultiply (inputScratch.getWritePointer (c), chans[c], gain, count);
                chans[0] = inputScratch.getReadPointer (0);
                chans[1] = inputScratch.getReadPointer (1);
            }
            t->recorder->write (chans, count);
        }
    }

    // Automation clips move their controls before anything is processed.
    if (isPlay)
        applyAutomation ((double) pos / juce::jmax (1.0, sr));

    // 2. Mix each channel. Take lanes (playThrough set) are summed into their channel *before*
    //    its plug-ins and fader, so a take sounds the same whichever track row it sits on.
    int scIndex = -1;
    for (auto* t : tracks)
    {
        ++scIndex;
        if (t->playThrough != nullptr || t->isAutomation())
            continue;

        for (int c = 0; c < kMaxChannels; ++c)
            trackBuffer.clear (c, 0, numSamples);

        bool soloed = t->solo.load();
        auto addSource = [&] (Track& src)
        {
            // Clips: existing takes keep playing while recording, so you can sing along.
            if (isPlay && ! src.clips.empty())
                renderClips (src, pos, numSamples);

            // Input monitoring (goes through the channel's insert chain so the singer hears the FX)
            if (src.monitor.load())
            {
                const int inCh = src.inputChannel.load();
                const float* inL = getInput (inCh);
                const float* inR = src.stereoInput.load() ? getInput (inCh + 1) : inL;
                const float gain = juce::Decibels::decibelsToGain (src.inputGainDb.load());
                if (inL != nullptr) trackBuffer.addFrom (0, 0, inL, numSamples, gain);
                if (inR != nullptr) trackBuffer.addFrom (1, 0, inR, numSamples, gain);
            }
        };

        addSource (*t);
        for (auto* lane : tracks)
        {
            if (lane->playThrough != t || lane->mute.load())
                continue;
            soloed = soloed || lane->solo.load();
            addSource (*lane);
        }

        // Inserts, fader, pan, meters - then the delay that lines this channel up with the slowest one
        const bool skipLatent = lowLatNow && t->monitor.load() && t->armed.load();
        processChannel (*t, numSamples, skipLatent);
        const int lat = channelLatency (*t, skipLatent);
        t->latencySamples = lat;
        delayChannel (*t, numSamples, skipLatent ? 0 : maxLat - lat);

        // publish for sidechains (post-fader)
        if (scIndex < kMaxSidechainTracks)
        {
            sidechainBus.copyFrom (scIndex * 2, 0, trackBuffer, 0, 0, numSamples);
            sidechainBus.copyFrom (scIndex * 2 + 1, 0, trackBuffer, 1, 0, numSamples);
        }

        const bool audible = ! t->mute.load() && (! anySolo || soloed);
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

    // Live master loudness (what the mix measures, never the reference or the click)
    if (isPlay && ! offlineActive.load())
    {
        auto& lm = *masterLoudness;
        if (resetLoudness.exchange (false))
            lm.meter.reset();
        lm.meter.process (masterBuffer.getReadPointer (0), masterBuffer.getReadPointer (1), numSamples);
        if (++lm.publish >= 8)
        {
            lm.publish = 0;
            masterShortTermLufs = (float) lm.meter.shortTerm();
            masterIntegratedLufs = (float) lm.meter.integrated();
        }
    }

    // Reference A/B: hear the reference song at the same position instead of the mix
    if (referenceOn.load() && referenceBuffer != nullptr && isPlay && ! offlineActive.load() && bounceWriter == nullptr)
    {
        const auto& rb = *referenceBuffer;
        const float g = juce::Decibels::decibelsToGain (getReferenceGainDb());
        masterBuffer.clear (0, numSamples);
        const int avail = (int) juce::jlimit<juce::int64> (0, numSamples, (juce::int64) rb.getNumSamples() - pos);
        if (pos >= 0 && avail > 0)
            for (int c = 0; c < 2; ++c)
                masterBuffer.addFrom (c, 0, rb, juce::jmin (c, rb.getNumChannels() - 1), (int) pos, avail, g);
    }

    // Bounce (before the click is added)
    if (bounceWriter != nullptr && isPlay)
    {
        const float* chans[2] = { masterBuffer.getReadPointer (0), masterBuffer.getReadPointer (1) };
        bounceWriter->write (chans, numSamples);
        if (pos + numSamples >= bounceEnd)
            bounceDone = true;
    }

    // Metronome (never in exports)
    const bool countingIn = isRec && pos < recordStart && pos + numSamples > countInFrom;
    if (isPlay && (metronomeOn.load() || countingIn) && bounceWriter == nullptr && ! offlineActive.load())
    {
        const double beatLen = sr * 60.0 / juce::jmax (20.0, bpm.load());
        const int sound = metronomeSound.load();
        const double clickLen = sr * (sound == 2 ? 0.05 : 0.03);
        const float vol = juce::jlimit (0.0f, 1.5f, metronomeVolume.load());
        const bool accent = metronomeAccent.load();
        auto* l = masterBuffer.getWritePointer (0);
        auto* r = masterBuffer.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            const double p = (double) (pos + i);
            const auto beat = (juce::int64) std::floor (p / beatLen);
            const double within = p - (double) beat * beatLen;
            if (within < clickLen)
            {
                const bool first = accent && beat % 4 == 0;
                const double t = within / sr;
                float s = 0.0f;
                if (sound == 1)        // woodblock: two inharmonic partials, very fast decay
                {
                    const double f = first ? 1900.0 : 1400.0;
                    const float env = (float) std::exp (-t / 0.006);
                    s = env * (0.42f * (float) std::sin (juce::MathConstants<double>::twoPi * f * t)
                               + 0.16f * (float) std::sin (juce::MathConstants<double>::twoPi * f * 2.63 * t));
                }
                else if (sound == 2)   // hi-hat: high-passed noise burst
                {
                    metroNoise = metroNoise * 1664525u + 1013904223u;
                    const float nz = (float) ((metroNoise >> 9) & 0x7fff) / 16384.0f - 1.0f;
                    const float hp = nz - metroPrev;
                    metroPrev = nz;
                    s = 0.22f * (first ? 1.5f : 1.0f) * (float) std::exp (-t / 0.012) * hp;
                }
                else                   // classic beep
                {
                    const double freq = first ? 1600.0 : 1000.0;
                    const float env = 1.0f - (float) (within / clickLen);
                    s = 0.3f * env * (float) std::sin (juce::MathConstants<double>::twoPi * freq * t);
                }
                l[i] += s * vol;
                r[i] += s * vol;
            }
        }
    }

    // Monitor modes (mono / side / single side) - for checking only, never in exports
    const int mon = monitorMode.load();
    if (mon != 0 && ! offlineActive.load() && bounceWriter == nullptr)
    {
        auto* l = masterBuffer.getWritePointer (0);
        auto* r = masterBuffer.getWritePointer (1);
        for (int i = 0; i < numSamples; ++i)
        {
            float v = 0.0f;
            switch (mon)
            {
                case 1: v = 0.5f * (l[i] + r[i]); break;
                case 2: v = 0.5f * (l[i] - r[i]); break;
                case 3: v = l[i]; break;
                default: v = r[i]; break;
            }
            l[i] = r[i] = v;
        }
    }

    // Safety clip on the master output: untouched below -0.9 dBFS, rounds anything hotter so it never
    // goes past 0 dBFS (faders reach +24 dB, so a loud mix would otherwise clip harshly).
    for (int c = 0; c < 2; ++c)
    {
        auto* d = masterBuffer.getWritePointer (c);
        for (int i = 0; i < numSamples; ++i)
        {
            const float ax = std::abs (d[i]);
            if (ax > 0.9f)
                d[i] = std::copysign (0.9f + 0.1f * std::tanh ((ax - 0.9f) / 0.1f), d[i]);
        }
    }

    // Audition player (tool windows): mixed in after the master, never in exports
    if (auditionOn.load() && auditionBuffer != nullptr && ! offlineActive.load())
    {
        const auto& ab = *auditionBuffer;
        const auto start = auditionPos.load();
        const int avail = (int) juce::jlimit<juce::int64> (0, numSamples, (juce::int64) ab.getNumSamples() - start);
        const float gain = auditionGain.load();
        for (int c = 0; c < 2 && avail > 0; ++c)
            masterBuffer.addFrom (c, 0, ab, juce::jmin (c, ab.getNumChannels() - 1), (int) start, avail, gain);
        auditionPos = start + numSamples;
        if (avail < numSamples)
            auditionOn = false;   // reached the end
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

//==============================================================================
// Automation

float AudioEngine::volumeDbToNormalised (float db)
{
    if (db <= -60.0f) return 0.0f;
    return juce::jlimit (0.0f, 1.0f, std::cbrt (juce::Decibels::decibelsToGain (db) / 15.85f));   // v^3 * 15.85 = gain, so 1.0 = +24 dB
}

float AudioEngine::normalisedToVolumeDb (float v)
{
    if (v <= 0.0f) return -60.0f;
    return juce::jlimit (-60.0f, 24.0f, juce::Decibels::gainToDecibels (15.85f * v * v * v, -60.0f));
}

static juce::AudioProcessorParameter* automatedParameter (const Automation& a)
{
    if (a.target == nullptr || a.kind != Automation::Kind::Param)
        return nullptr;
    if (a.slot < 0 || a.slot >= (int) a.target->inserts.size())
        return nullptr;
    auto* plugin = a.target->inserts[(size_t) a.slot]->plugin.get();
    if (plugin == nullptr)
        return nullptr;
    const auto& params = plugin->getParameters();
    return juce::isPositiveAndBelow (a.param, params.size()) ? params[a.param] : nullptr;
}

float AudioEngine::getControlValue (const Automation& a)
{
    if (a.target == nullptr) return 0.5f;
    switch (a.kind)
    {
        case Automation::Kind::Volume: return volumeDbToNormalised (a.target->volumeDb.load());
        case Automation::Kind::Pan:    return 0.5f + 0.5f * a.target->pan.load();
        case Automation::Kind::Param:  if (auto* p = automatedParameter (a)) return p->getValue(); break;
    }
    return 0.5f;
}

juce::String AudioEngine::describeTarget (const Automation& a)
{
    if (a.target == nullptr)
        return "(deleted channel)";
    const auto channel = a.target->name;
    switch (a.kind)
    {
        case Automation::Kind::Volume: return channel + " - Volume";
        case Automation::Kind::Pan:    return channel + " - Pan";
        case Automation::Kind::Param:
            if (auto* p = automatedParameter (a))
                return channel + " - " + a.target->inserts[(size_t) a.slot]->plugin->getName() + ": " + p->getName (40);
            return channel + " - slot " + juce::String (a.slot + 1) + " (empty)";
    }
    return channel;
}

Track* AudioEngine::addAutomationTrack (Track& target, Automation::Kind kind, int slot, int param)
{
    auto a = std::make_unique<Automation>();
    a->kind = kind;
    a->target = &target;
    a->slot = slot;
    a->param = param;
    const float now = getControlValue (*a);
    const double end = juce::jmax (8.0, (double) getSessionLengthSamples() / juce::jmax (1.0, sampleRate.load()));
    a->points = { { 0.0, now }, { end, now } };

    auto* t = addTrack ("Auto: " + describeTarget (*a));
    const juce::ScopedLock sl (lock);
    t->automation = std::move (a);
    return t;
}

void AudioEngine::applyAutomation (double seconds)
{
    // called on the audio thread with the engine lock held
    for (auto* t : tracks)
    {
        if (t->automation == nullptr || t->mute.load() || t->automation->target == nullptr || t->automation->points.empty())
            continue;
        const auto& a = *t->automation;
        const float v = a.valueAt (seconds);
        switch (a.kind)
        {
            case Automation::Kind::Volume: a.target->volumeDb = normalisedToVolumeDb (v); break;
            case Automation::Kind::Pan:    a.target->pan = juce::jlimit (-1.0f, 1.0f, v * 2.0f - 1.0f); break;
            case Automation::Kind::Param:
                if (auto* p = automatedParameter (a))
                    if (std::abs (p->getValue() - v) > 1.0e-4f)
                        p->setValue (v);
                break;
        }
    }
}

//==============================================================================
void AudioEngine::startAudition (std::shared_ptr<const juce::AudioBuffer<float>> buffer, juce::int64 fromSample)
{
    std::shared_ptr<const juce::AudioBuffer<float>> old;
    {
        const juce::ScopedLock sl (lock);
        old = std::exchange (auditionBuffer, std::move (buffer));
        auditionPos = juce::jmax<juce::int64> (0, fromSample);
        auditionOn = auditionBuffer != nullptr;
    }
}

void AudioEngine::swapAuditionBuffer (std::shared_ptr<const juce::AudioBuffer<float>> buffer)
{
    std::shared_ptr<const juce::AudioBuffer<float>> old;
    const juce::ScopedLock sl (lock);
    old = std::exchange (auditionBuffer, std::move (buffer));
}

void AudioEngine::stopAudition()
{
    auditionOn = false;
}

//==============================================================================
void AudioEngine::applyPunchTake (Track& track, const std::shared_ptr<AudioClip>& take)
{
    const auto in = punchIn.load(), out = punchOut.load();
    if (out <= in)
        return;
    if (take->startSample.load() < in)  trimClipStart (*take, in);
    if (take->getEnd() > out)           trimClipEnd (*take, out);

    for (auto& c : getClips (track))
    {
        if (c == take || c->getEnd() <= in || c->startSample.load() >= out)
            continue;
        auto piece = c;
        if (piece->startSample.load() < in)
            if (auto right = splitClip (track, piece, in))
                piece = right;
        if (piece->getEnd() > out)
            splitClip (track, piece, out);
        removeClip (track, piece.get());
    }
}

//==============================================================================
juce::Result AudioEngine::renderChannel (Track& t, bool postFader, juce::AudioBuffer<float>& result,
                                         std::function<bool (float)> onProgress, double tailSeconds)
{
    if (recording)
        return juce::Result::fail ("Stop recording first.");
    if (t.isMaster || t.isAutomation() || ! isValidChannel (&t))
        return juce::Result::fail ("Pick an audio track.");

    const auto length = getSessionLengthSamples();
    if (length <= 0)
        return juce::Result::fail ("There's nothing to render yet.");

    stop();
    const double sr = getSampleRate();
    const auto total = length + (juce::int64) (sr * juce::jmax (0.0, tailSeconds));
    if (total > (juce::int64) std::numeric_limits<int>::max())
        return juce::Result::fail ("The song is too long.");

    int lat = 0;
    std::vector<Track*> lanes;
    {
        const juce::ScopedLock sl (lock);
        lat = channelLatency (t, false);
        for (auto* other : tracks)
            if (other->playThrough == &t && ! other->mute.load())
                lanes.push_back (other);
    }

    result.setSize (2, (int) total);
    result.clear();
    const float savedVol = t.volumeDb.load(), savedPan = t.pan.load();
    if (! postFader) { t.volumeDb = 0.0f; t.pan = 0.0f; }

    auto setNonRealtime = [&] (bool offline)
    {
        const juce::ScopedLock sl (lock);
        for (auto& s : t.inserts)
            if (s->plugin != nullptr)
                s->plugin->setNonRealtime (offline);
    };
    setNonRealtime (true);
    offlineActive = true;

    const int block = juce::jmax (64, blockSize.load());
    bool cancelled = false;
    for (juce::int64 done = 0; done < total + lat; done += block)
    {
        const int n = (int) juce::jmin<juce::int64> (block, total + lat - done);
        {
            const juce::ScopedLock sl (lock);
            ensureScratch (n);
            for (int c = 0; c < kMaxChannels; ++c)
                trackBuffer.clear (c, 0, n);
            applyAutomation ((double) done / juce::jmax (1.0, sr));
            if (! t.clips.empty())
                renderClips (t, done, n);
            for (auto* lane : lanes)
                if (isValidChannel (lane) && ! lane->clips.empty())
                    renderClips (*lane, done, n);
            if (! postFader)
                t.smoothedGainL = t.smoothedGainR = 1.0f;
            processChannel (t, n, false);

            const auto dst = done - lat;
            const auto from = juce::jmax<juce::int64> (0, -dst);
            const auto count = juce::jmin<juce::int64> (n - from, total - (dst + from));
            if (count > 0)
                for (int c = 0; c < 2; ++c)
                    result.copyFrom (c, (int) (dst + from), trackBuffer, c, (int) from, (int) count);
        }
        if (onProgress && ! onProgress ((float) ((double) (done + n) / (double) (total + lat))))
        {
            cancelled = true;
            break;
        }
    }

    offlineActive = false;
    setNonRealtime (false);
    if (! postFader) { t.volumeDb = savedVol; t.pan = savedPan; }
    return cancelled ? juce::Result::fail ("Cancelled.") : juce::Result::ok();
}

//==============================================================================
void AudioEngine::setReference (std::shared_ptr<const juce::AudioBuffer<float>> buffer, double integratedLufs)
{
    {
        const juce::ScopedLock sl (lock);
        std::swap (referenceBuffer, buffer);
    }
    referenceLufs = referenceBuffer != nullptr ? integratedLufs : -100.0;
    if (referenceBuffer == nullptr)
        referenceOn = false;
}

float AudioEngine::getReferenceGainDb() const
{
    if (! referenceMatch.load() || ! hasReference())
        return 0.0f;
    double mix = masterIntegratedLufs.load();
    if (mix <= -70.0) mix = masterShortTermLufs.load();
    if (mix <= -70.0)
        return 0.0f;   // nothing measured yet: play it as it is
    return (float) juce::jlimit (-24.0, 12.0, mix - referenceLufs.load());
}

//==============================================================================
double AudioEngine::getRetroSeconds() const
{
    const juce::ScopedLock sl (lock);
    return juce::jmin ((double) retroWritten, (double) retroRing.getNumSamples()) / juce::jmax (1.0, sampleRate.load());
}

std::pair<Track*, std::shared_ptr<AudioClip>> AudioEngine::captureRetro()
{
    juce::AudioBuffer<float> copy;
    Track* track = nullptr;
    juce::int64 endTimeline = -1;
    bool stereo = false;
    {
        const juce::ScopedLock sl (lock);
        track = retroTrack;
        if (track == nullptr || ! isValidChannel (track) || recording)
            return {};
        const int len = retroRing.getNumSamples();
        const int n = (int) juce::jmin<juce::int64> (retroWritten, len);
        if (n < (int) (sampleRate.load() * 0.25))
            return {};
        copy.setSize (2, n);
        const int start = (retroPos - n + len) % len;
        const int firstPart = juce::jmin (n, len - start);
        for (int c = 0; c < 2; ++c)
        {
            copy.copyFrom (c, 0, retroRing, c, start, firstPart);
            if (n > firstPart)
                copy.copyFrom (c, firstPart, retroRing, c, 0, n - firstPart);
        }
        endTimeline = retroTimelineEnd;
        stereo = track->stereoInput.load();
        retroWritten = 0;   // the same performance isn't captured twice
    }

    // keep only the part where something was sung (with 50 ms of air around it)
    const int n = copy.getNumSamples();
    int first = n, last = -1;
    for (int i = 0; i < n; ++i)
        if (std::abs (copy.getSample (0, i)) > 0.002f || std::abs (copy.getSample (1, i)) > 0.002f) { first = juce::jmin (first, i); last = i; }
    if (last < 0)
        return {};
    const int margin = (int) (sampleRate.load() * 0.05);
    first = juce::jmax (0, first - margin);
    last = juce::jmin (n - 1, last + margin);
    const int keep = last - first + 1;

    getTakesFolder().createDirectory();
    auto file = getTakesFolder().getNonexistentChildFile (juce::File::createLegalFileName (track->name) + "_retro_"
                                                          + juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S"), ".wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (file.createOutputStream());
        if (os == nullptr)
            return {};
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sampleRate.load(), stereo ? 2u : 1u, 24, {}, 0));
        if (w == nullptr)
            return {};
        os.release();
        const float* chans[2] = { copy.getReadPointer (0, first), copy.getReadPointer (1, first) };
        w->writeFromFloatArrays (chans, stereo ? 2 : 1, keep);
    }

    // Placed where it was sung if the transport was rolling (with the same latency compensation as a take)
    juce::int64 start = position.load();
    if (endTimeline >= 0)
    {
        juce::int64 latency = maxLatency.load();
        if (auto* dev = deviceManager.getCurrentAudioDevice())
            latency += dev->getInputLatencyInSamples() + dev->getOutputLatencyInSamples();
        start = endTimeline - n + first - latency;
    }
    auto clip = loadClip (file, juce::jmax<juce::int64> (0, start));
    if (clip == nullptr)
        return {};
    clip->name = track->name + " (retro)";
    return { track, clip };
}

//==============================================================================
AudioEngine::Mp3Gapless AudioEngine::readMp3Gapless (const juce::File& file)
{
    Mp3Gapless g;
    juce::MemoryBlock mb;
    {
        juce::FileInputStream in (file);
        if (! in.openedOk())
            return g;
        mb.setSize ((size_t) juce::jmin<juce::int64> (in.getTotalLength(), 512 * 1024));
        in.read (mb.getData(), (int) mb.getSize());
    }
    const auto* b = (const juce::uint8*) mb.getData();
    const size_t n = mb.getSize();
    size_t p = 0;
    if (n > 10 && b[0] == 'I' && b[1] == 'D' && b[2] == '3')
        p = 10 + (size_t) (((b[6] & 127) << 21) | ((b[7] & 127) << 14) | ((b[8] & 127) << 7) | (b[9] & 127));
    while (p + 4 < n && ! (b[p] == 0xff && (b[p + 1] & 0xe0) == 0xe0))
        ++p;
    if (p + 200 >= n)
        return g;
    const bool mpeg1 = ((b[p + 1] >> 3) & 3) == 3;
    const bool mono = ((b[p + 3] >> 6) & 3) == 3;
    const size_t side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    size_t t = p + 4 + side;
    auto isTag = [&] (size_t at) { return at + 4 < n && ((b[at] == 'X' && b[at + 1] == 'i' && b[at + 2] == 'n' && b[at + 3] == 'g')
                                                        || (b[at] == 'I' && b[at + 1] == 'n' && b[at + 2] == 'f' && b[at + 3] == 'o')); };
    if (! isTag (t))
        return g;
    auto be32 = [&] (size_t at) { return ((juce::uint32) b[at] << 24) | ((juce::uint32) b[at + 1] << 16) | ((juce::uint32) b[at + 2] << 8) | b[at + 3]; };
    const auto flags = be32 (t + 4);
    size_t q = t + 8;
    if (flags & 1) { g.frames = be32 (q); q += 4; }
    if (flags & 2) q += 4;
    if (flags & 4) q += 100;
    if (flags & 8) q += 4;
    // LAME-style extension (LAME, Lavc/Lavf, ...): encoder delay and padding, 12 bits each, 21 bytes in
    if (q + 24 < n && std::isalpha (b[q]) && std::isalpha (b[q + 1]) && std::isalpha (b[q + 2]) && std::isalpha (b[q + 3]))
    {
        g.encoderDelay = (b[q + 21] << 4) | (b[q + 22] >> 4);
        g.padding = ((b[q + 22] & 15) << 8) | b[q + 23];
        g.found = g.frames > 0 && g.encoderDelay < 3000 && g.padding < 3000;
    }
    const int spf = mpeg1 ? 1152 : 576;
    if (g.found)
    {
        g.trimStart = g.encoderDelay + 529 + kMp3InfoFrameSamples * spf / 1152;
        g.validLength = g.frames * spf - g.encoderDelay - g.padding;
    }
    return g;
}


ClipSettings AudioEngine::renderSettingsFor (const AudioClip& clip) const
{
    auto s = clip.settings;
    if (clip.lossless && losslessUntouched.load())
        s.declick = 0;   // lossless: no automatic fades at the file's own edges
    return s;
}
