#include <JuceHeader.h>
#include "MainComponent.h"
#include "License.h"
#include "GlassLookAndFeel.h"
#include "Plugins/Builtins.h"
#include "Account.h"
#include "EditHistory.h"
#include "Exporter.h"
#include "FlpImport.h"
#include "NightcoreMaker.h"
#include "ClipFx.h"
#include "WelcomeScreen.h"
#include "AvatarMedia.h"

void* amethystIpcConnect();
bool amethystIpcWrite (void*, const void*, std::size_t);
int amethystIpcRead (void*, void*, std::size_t);
void amethystIpcClose (void*);
bool amethystProtect (const void* data, std::size_t size, std::vector<unsigned char>& out, bool encrypt);
double amethystTuneTest (double inputHz, int key, int scale, double retuneMs);

int runSecondWaveSelfTests();   // SelfTest2.cpp

class VocalDAWApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return kAppTitle; }
    const juce::String getApplicationVersion() override { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override
    {
        // headless test modes may run next to an open AMETHYST window
        const auto args = juce::JUCEApplicationBase::getCommandLineParameters();
        return args.contains ("--selftest") || args.contains ("--flp-") || args.contains ("--fst-test") || args.contains ("--account-test")
            || args.contains ("--window-test") || args.contains ("--key-test") || args.contains ("--ui-test") || args.contains ("--mp3-test") || args.contains ("--license-verify") || args.contains ("--license-status") || args.contains ("--discord-test") || args.contains ("--preview") || args.contains ("--snapshot");
    }

    // Headless check of the clip effect renderer:  VocalDAW.exe --selftest
    static int runSelfTest()
    {
        std::setvbuf (stdout, nullptr, _IONBF, 0);   // a crash still shows the last line
        const double sr = 48000.0;
        juce::AudioBuffer<float> src (2, (int) sr * 4);
        juce::Random rnd (1);
        for (int i = 0; i < src.getNumSamples(); ++i)
        {
            const float v = 0.4f * std::sin (0.03f * (float) i) + 0.05f * (rnd.nextFloat() - 0.5f);
            src.setSample (0, i, v);
            src.setSample (1, i, v * 0.8f);
        }

        std::vector<ClipSettings> cases (6);
        cases[1].timeMode = 1; cases[1].speed = 0.6f; cases[1].pitchSemis = 5.0f;
        cases[2].timeMode = 1; cases[2].speed = 1.9f; cases[2].fineCents = -40.0f; cases[2].reverse = true;
        cases[3].pogo = -1.0f; cases[3].reverb = 1.0f; cases[3].reverbType = 1; cases[3].delay = 0.8f;
        cases[4].removeDC = cases[4].normalize = cases[4].invert = cases[4].stereoFade = cases[4].swapStereo = cases[4].softClip = true;
        cases[4].startPct = 30.0f; cases[4].lengthPct = 40.0f; cases[4].fadeInMs = 300.0f; cases[4].fadeOutMs = 900.0f; cases[4].trimDb = -30.0f;
        cases[4].boostDb = 12.0f; cases[4].eqTilt = 0.8f; cases[4].ringMix = 0.5f; cases[4].cutHz = 800.0f; cases[4].resonance = 0.9f;
        cases[5].pogo = 1.0f; cases[5].pan = -0.7f; cases[5].gainDb = -6.0f; cases[5].lengthPct = 0.01f;

        juce::AudioBuffer<float> mono (1, 3000);
        mono.clear();

        for (size_t i = 0; i < cases.size(); ++i)
        {
            for (auto* in : { &src, &mono })
            {
                auto out = ClipFx::render (*in, cases[i], sr, 120.0);
                if (out.getNumSamples() <= 0)
                    { std::printf ("case %d: empty output\n", (int) i); return 1; }
                for (int c = 0; c < out.getNumChannels(); ++c)
                    for (int s = 0; s < out.getNumSamples(); ++s)
                        if (! std::isfinite (out.getSample (c, s)))
                            { std::printf ("case %d: non-finite sample\n", (int) i); return 1; }
                std::printf ("case %d (%s): %d -> %d samples, %d ch\n", (int) i, in == &src ? "stereo" : "mono",
                             in->getNumSamples(), out.getNumSamples(), out.getNumChannels());
            }
        }

        ClipSettings s; s.boostDb = 3.0f; s.reverse = true; s.timeMode = 1;
        juce::XmlElement x ("CLIP");
        s.writeTo (x);
        ClipSettings back; back.readFrom (x);
        if (back != s) { std::printf ("settings XML round-trip failed\n"); return 1; }

        // Clip editing: split, trim and overlapping-take playback, checked sample-exactly through the engine.
        {
            AudioEngine eng;
            auto* t = eng.addTrack ("T");
            auto makeClip = [] (float value, juce::int64 start, int len)
            {
                auto b = std::make_shared<juce::AudioBuffer<float>> (1, len);
                for (int i = 0; i < len; ++i)
                    b->setSample (0, i, value >= 0.0f ? value : (float) i / (float) len);
                auto c = std::make_shared<AudioClip>();
                c->source = b; c->buffer = b; c->startSample = start;
                return c;
            };
            juce::AudioBuffer<float> out (2, 512);
            auto renderAt = [&] (juce::int64 pos) -> float
            {
                eng.setTransportPosition (pos);
                eng.play();
                float* outs[2] = { out.getWritePointer (0), out.getWritePointer (1) };
                eng.audioDeviceIOCallbackWithContext (nullptr, 0, outs, 2, 512, juce::AudioIODeviceCallbackContext {});
                eng.stop();
                return out.getSample (0, 100); // sample at pos + 100
            };
            int fails = 0;
            auto expect = [&fails] (bool ok, const char* what) { std::printf ("  [%s] %s\n", ok ? "ok" : "FAIL", what); if (! ok) ++fails; };

            auto ramp = makeClip (-1.0f, 0, 48000);
            eng.addClip (*t, ramp);
            auto right = eng.splitClip (*t, ramp, 20000);
            expect (right != nullptr && ramp->getLength() == 20000 && right->getOffset() == 20000 && right->getLength() == 28000,
                    "split makes two clips that share the audio");
            expect (std::abs (renderAt (30000) - 30100.0f / 48000.0f) < 1.0e-4f, "right half plays the original audio at the same time");

            eng.trimClipStart (*right, 22000);
            expect (right->startSample.load() == 22000 && right->getOffset() == 22000 && right->getEnd() == 48000, "trim start keeps the end fixed");
            eng.trimClipEnd (*ramp, 10000);
            expect (ramp->getLength() == 10000, "trim end shortens the clip");
            expect (renderAt (15000) == 0.0f, "trimmed gap is silent");
            expect (std::abs (renderAt (30000) - 30100.0f / 48000.0f) < 1.0e-4f, "audio still lines up after trimming");

            eng.trimClipStart (*right, -100000);
            expect (right->startSample.load() == 0 && right->getOffset() == 0, "trim can't reveal audio before the file starts");
            eng.trimClipStart (*right, 22000);

            eng.addClip (*t, makeClip (0.25f, 29000, 5000));
            t->layerTakes = true;
            expect (std::abs (renderAt (30000) - (30100.0f / 48000.0f + 0.25f)) < 1.0e-4f, "layer mode: overlapping takes are both heard");
            t->layerTakes = false;
            expect (std::abs (renderAt (30000) - 0.25f) < 1.0e-4f, "newest-on-top mode: newest take wins");

            // undo / redo
            {
                EditHistory history (eng);
                const auto before = t->clips.size();
                auto victim = t->clips.front();
                const auto victimLen = victim->getLength();

                history.checkpoint ("Delete clip");
                eng.removeClip (*t, victim.get());
                expect (t->clips.size() == before - 1, "delete removes the clip");
                expect (history.undo() && t->clips.size() == before && t->clips.front() == victim && victim->getLength() == victimLen,
                        "undo brings a deleted clip back with its trim");
                expect (history.redo() && t->clips.size() == before - 1, "redo deletes it again");
                history.undo();

                history.checkpoint ("Move clip");
                const auto oldStart = right->startSample.load();
                right->startSample = oldStart + 5000;
                eng.trimClipEnd (*right, right->getEnd() - 3000);
                expect (history.undo() && right->startSample.load() == oldStart && right->getEnd() == 48000,
                        "undo restores position and trim together");

                history.checkpoint ("Click");
                history.discardIfUnchanged();
                expect (history.nextUndoName() != "Click", "a click with no change isn't an undo step");
            }

            // where new takes go
            {
                auto* t2 = eng.addTrack ("T2");
                auto* t3 = eng.addTrack ("T3");
                auto take = makeClip (0.1f, 25000, 4000);   // overlaps clips on T
                expect (eng.findTrackForTake (t, *take) == t2, "overlapping take goes to the next free track");
                t2->armed = true;
                expect (eng.findTrackForTake (t, *take) == t3, "armed tracks are skipped");
                eng.addClip (*t3, makeClip (0.1f, 26000, 1000));
                expect (eng.findTrackForTake (t, *take) == nullptr, "no free track = make a new one");
                auto lonely = makeClip (0.1f, 200000, 4000);
                expect (eng.findTrackForTake (t, *lonely) == t, "a take with nothing underneath stays put");

                // a take lane plays through its source channel: same fader, so same level
                auto* lane = eng.addTrack ("Lane");
                t->volumeDb = -6.0f;
                lane->volumeDb = 0.0f;
                eng.setClips (*t, {});
                eng.setClips (*t2, {}); eng.setClips (*t3, {});
                eng.addClip (*lane, makeClip (0.5f, 0, 48000));
                eng.setPlayThrough (*lane, t);
                renderAt (1000);                               // let the fader smoothing settle
                const float throughSource = renderAt (1000);
                expect (std::abs (throughSource - 0.5f * juce::Decibels::decibelsToGain (-6.0f)) < 2.0e-3f,
                        "take lane uses the source track's fader (no volume jump)");
                eng.setPlayThrough (*lane, nullptr);
                renderAt (1000);
                const float ownChannel = renderAt (1000);
                expect (std::abs (ownChannel - 0.5f) < 2.0e-3f, "back on its own channel it uses its own fader");
                t->volumeDb = 0.0f;
            }

            if (fails > 0) { std::printf ("clip editing test FAILED\n"); return 1; }
        }

        // AMETHYST TUNE: an out-of-tune tone must come out on the right note.
        {
            struct Case { double in; int key, scale; double expect; const char* what; };
            const Case tuneCases[] = {
                { 431.0, 9, 0, 440.0, "flat A (431 Hz) is pulled up to 440 Hz" },
                { 451.0, 9, 0, 440.0, "sharp A (451 Hz) is pulled down to 440 Hz" },
                { 455.0, 0, 1, 440.0, "in C major, a note between A and A# goes to A (A# isn't in the key)" },
                { 255.0, 0, 0, 261.63, "flat middle C is pulled up to 261.6 Hz" },
            };
            int tuneFails = 0;
            for (auto& c : tuneCases)
            {
                const double got = amethystTuneTest (c.in, c.key, c.scale, 0.0);
                const double cents = got > 0.0 ? 1200.0 * std::log2 (got / c.expect) : 9999.0;
                const bool ok = std::abs (cents) < 15.0;
                std::printf ("  [%s] %s  (got %.1f Hz, %+.1f cents)\n", ok ? "ok" : "FAIL", c.what, got, cents);
                if (! ok) ++tuneFails;
            }
            const double untouched = amethystTuneTest (440.0, 9, 0, 0.0);
            const bool cleanOk = std::abs (1200.0 * std::log2 (untouched / 440.0)) < 5.0;
            std::printf ("  [%s] an in-tune note passes through unchanged (got %.1f Hz)\n", cleanOk ? "ok" : "FAIL", untouched);
            if (! cleanOk) ++tuneFails;
            if (tuneFails > 0) { std::printf ("tune test FAILED\n"); return 1; }
        }

        // Export: the WHOLE song must come out (not just the start), in every format.
        {
            AudioEngine eng;
            auto* t = eng.addTrack ("T");
            const double esr = eng.getSampleRate();
            const int len = (int) (esr * 60.0); // a 60-second "song"
            auto b = std::make_shared<juce::AudioBuffer<float>> (2, len);
            for (int i = 0; i < len; ++i)
            {
                const float v = i >= len - 2000 ? 0.9f : 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / esr);
                b->setSample (0, i, v);
                b->setSample (1, i, v);
            }
            auto clip = std::make_shared<AudioClip>();
            clip->source = b; clip->buffer = b;
            eng.addClip (*t, clip);

            int exportFails = 0;
            auto expect = [&exportFails] (bool ok, const juce::String& what) { std::printf ("  [%s] %s\n", ok ? "ok" : "FAIL", what.toRawUTF8()); if (! ok) ++exportFails; };

            juce::AudioBuffer<float> mix;
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            const auto r = eng.renderSong (mix, [] (float) { return true; });
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            expect (r.wasOk() && mix.getNumSamples() == len + (int) (esr * 3.0), "render covers the whole song plus a 3 s tail");
            expect (std::abs (mix.getSample (0, len - 1000) - 0.9f) < 0.02f, "the very end of the song is in the export");
            expect (ms < 60000.0, "a 60 s song renders faster than real time (" + juce::String (60000.0 / juce::jmax (1.0, ms), 0) + "x)");
            expect (! eng.isPlaying() && ! eng.isExporting(), "transport is stopped again afterwards");

            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("amethyst_export_test");
            dir.createDirectory();
            for (int f = 0; f <= 4; ++f)
            {
                const auto format = (Exporter::Format) f;
                const auto file = dir.getChildFile ("song" + Exporter::extension (format));
                if (! Exporter::isAvailable (format))
                {
                    std::printf ("  [skip] %s not available here\n", Exporter::displayName (format).toRawUTF8());
                    continue;
                }
                const auto wr = Exporter::write (mix, esr, format, file);
                double seconds = -1.0;
                if (std::unique_ptr<juce::AudioFormatReader> reader (eng.audioFormats.createReaderFor (file)); reader != nullptr)
                    seconds = reader->lengthInSamples / reader->sampleRate;
                const bool lengthOk = seconds < 0.0 ? file.getSize() > 100000 : std::abs (seconds - 63.0) < 1.0;
                expect (wr.wasOk() && lengthOk,
                        Exporter::displayName (format) + ": " + (wr.wasOk() ? juce::String (file.getSize() / 1024) + " KB, "
                            + (seconds < 0.0 ? juce::String ("(no reader to check length)") : juce::String (seconds, 2) + " s") : wr.getErrorMessage()));
            }
            dir.deleteRecursively();
            if (exportFails > 0) { std::printf ("export test FAILED\n"); return 1; }
        }

        // Built-in effects: every factory preset, stereo noise + a tone, must stay finite and not blow up.
        for (const auto& desc : Builtins::getDescriptions())
        {
            auto inst = Builtins::create (desc.fileOrIdentifier);
            auto* plug = dynamic_cast<AmethystPlugin*> (inst.get());
            if (plug == nullptr) { std::printf ("%s: failed to create\n", desc.name.toRawUTF8()); return 1; }

            for (int pr = 0; pr < plug->getNumPrograms(); ++pr)
            {
                plug->setCurrentProgram (pr);
                plug->setRateAndBufferSizeDetails (sr, 512);
                plug->prepareToPlay (sr, 512);
                juce::AudioBuffer<float> block (2, 512);
                juce::MidiBuffer midi;
                float peak = 0.0f;
                for (int b = 0; b < 400; ++b)   // ~4 seconds
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        const float v = 0.5f * std::sin (0.05f * (float) (b * 512 + i)) + 0.1f * (rnd.nextFloat() - 0.5f);
                        block.setSample (0, i, v);
                        block.setSample (1, i, v * 0.9f);
                    }
                    plug->processBlock (block, midi);
                    for (int c = 0; c < 2; ++c)
                        for (int i = 0; i < 512; ++i)
                        {
                            const float smp = block.getSample (c, i);
                            if (! std::isfinite (smp)) { std::printf ("%s / %s: non-finite\n", desc.name.toRawUTF8(), plug->getProgramName (pr).toRawUTF8()); return 1; }
                            peak = juce::jmax (peak, std::abs (smp));
                        }
                }
                if (peak > 8.0f) { std::printf ("%s / %s: runaway level %.2f\n", desc.name.toRawUTF8(), plug->getProgramName (pr).toRawUTF8(), peak); return 1; }
                std::printf ("  %-16s %-22s peak %.2f\n", desc.name.toRawUTF8(), plug->getProgramName (pr).toRawUTF8(), peak);
            }

            juce::MemoryBlock state;
            plug->getStateInformation (state);
            auto again = Builtins::create (desc.fileOrIdentifier);
            again->setStateInformation (state.getData(), (int) state.getSize());
            std::unique_ptr<juce::AudioProcessorEditor> ed (again->createEditor());
            if (ed == nullptr || ed->getWidth() < 100) { std::printf ("%s: editor problem\n", desc.name.toRawUTF8()); return 1; }
            std::printf ("%s: editor %d x %d\n", desc.name.toRawUTF8(), ed->getWidth(), ed->getHeight());
        }

        // ---- key finder: C major chord + scale should read as C major ----
        {
            const double ksr = 44100.0;
            juce::AudioBuffer<float> kb (1, (int) (ksr * 8));
            const double scale[] = { 261.63, 293.66, 329.63, 349.23, 392.0, 440.0, 493.88, 523.25 };
            for (int i = 0; i < kb.getNumSamples(); ++i)
            {
                const double t = i / ksr;
                const double note = scale[(int) (t * 2.0) % 8];
                const double v = 0.2 * (std::sin (2 * juce::MathConstants<double>::pi * 130.81 * t) + std::sin (2 * juce::MathConstants<double>::pi * 164.81 * t)
                                        + std::sin (2 * juce::MathConstants<double>::pi * 196.0 * t)) + 0.25 * std::sin (2 * juce::MathConstants<double>::pi * note * t);
                kb.setSample (0, i, (float) v);
            }
            const auto key = ClipFx::detectKey (kb, 0, kb.getNumSamples(), ksr);
            std::printf ("[%s] key finder: %s (confidence %.2f)\n", key.tonic == 0 && ! key.minor ? "ok" : "FAIL", key.name().toRawUTF8(), key.confidence);
            if (! (key.tonic == 0 && ! key.minor)) return 1;
        }

        // ---- nightcore render: 1.25x makes it 20% shorter ----
        {
            juce::AudioBuffer<float> nb (2, 48000 * 4);
            for (int i = 0; i < nb.getNumSamples(); ++i)
                nb.setSample (0, i, 0.3f * std::sin (0.05f * (float) i)), nb.setSample (1, i, 0.3f * std::sin (0.05f * (float) i));
            NightcoreMaker::Settings st;
            st.speed = 1.25; st.reverb = 0.0f; st.bassDb = 6.0f;
            const auto out = NightcoreMaker::render (nb, 48000.0, st);
            const double ratio = (double) out.getNumSamples() / nb.getNumSamples();
            const bool ok = std::abs (ratio - 0.8) < 0.01 && out.getMagnitude (0, out.getNumSamples()) <= 0.99f;
            std::printf ("[%s] nightcore render length ratio %.3f\n", ok ? "ok" : "FAIL", ratio);
            if (! ok) return 1;
        }

        // ---- tape stop: silent after "Play" goes off, back to the dry signal after it comes on ----
        {
            auto ts = Builtins::create ("amethyst.tapestop");
            auto* am = dynamic_cast<AmethystPlugin*> (ts.get());
            ts->setPlayConfigDetails (2, 2, 48000.0, 512);
            ts->prepareToPlay (48000.0, 512);
            am->setParam ("stop", 200.0f);
            am->setParam ("start", 100.0f);
            juce::MidiBuffer midi;
            juce::AudioBuffer<float> blk (2, 512);
            auto run = [&] (int blocks, double& lastRms, double& lastDiff)
            {
                static juce::int64 n = 0;
                for (int b = 0; b < blocks; ++b)
                {
                    juce::AudioBuffer<float> dry (2, 512);
                    for (int i = 0; i < 512; ++i, ++n)
                        for (int c = 0; c < 2; ++c)
                            dry.setSample (c, i, 0.5f * std::sin (0.03f * (float) n));
                    blk.makeCopyOf (dry);
                    ts->processBlock (blk, midi);
                    lastRms = blk.getRMSLevel (0, 0, 512);
                    double d = 0;
                    for (int i = 0; i < 512; ++i) d = juce::jmax (d, (double) std::abs (blk.getSample (0, i) - dry.getSample (0, i)));
                    lastDiff = d;
                }
            };
            double rms = 0, diff = 0;
            run (10, rms, diff);
            const bool passThrough = diff < 1.0e-5;
            am->setParam ("play", 0.0f);
            run (40, rms, diff);                 // ~0.4 s > 200 ms stop time
            const bool stopped = rms < 1.0e-4;
            am->setParam ("play", 1.0f);
            run (40, rms, diff);                 // spin up + crossfade
            const bool back = diff < 1.0e-5;
            std::printf ("[%s] tape stop: pass-through %d, stops %d, recovers %d\n", passThrough && stopped && back ? "ok" : "FAIL", passThrough, stopped, back);
            if (! (passThrough && stopped && back)) return 1;
        }

        // ---- upgraded effects ----
        {
            const double sr = 48000.0;
            juce::MidiBuffer midi;
            auto make = [sr] (const char* id)
            {
                auto p = Builtins::create (id);
                p->setPlayConfigDetails (2, 2, sr, 512);
                p->prepareToPlay (sr, 512);
                return p;
            };
            auto set = [] (juce::AudioPluginInstance& p, const char* id, float v) { dynamic_cast<AmethystPlugin&> (p).setParam (id, v); };
            int failed = 0;
            auto report = [&failed] (bool ok, const juce::String& what)
            {
                std::printf ("[%s] %s\n", ok ? "ok" : "FAIL", what.toRawUTF8());
                if (! ok) ++failed;
            };

            // Geode: measured decay time follows the "time" setting; stays stable at 20 s
            for (float rt : { 0.8f, 2.0f, 5.0f })
            {
                auto rv = make ("amethyst.reverb");
                set (*rv, "time", rt); set (*rv, "dry", 0); set (*rv, "er", 0); set (*rv, "wet", 100);
                set (*rv, "lowcut", 20); set (*rv, "highcut", 20000); set (*rv, "hidamp", 20000); set (*rv, "mod", 0); set (*rv, "predelay", 0);
                const int total = (int) (sr * (rt * 1.6 + 0.5));
                std::vector<float> env;
                double peakDb = -200;
                for (int pos = 0; pos < total; pos += 512)
                {
                    juce::AudioBuffer<float> b (2, 512);
                    b.clear();
                    if (pos == 0) { b.setSample (0, 0, 1.0f); b.setSample (1, 0, 1.0f); }
                    rv->processBlock (b, midi);
                    const double db = juce::Decibels::gainToDecibels ((double) b.getRMSLevel (0, 0, 512) + 1e-12);
                    env.push_back ((float) db);
                    peakDb = juce::jmax (peakDb, db);
                }
                // T30: time from -5 dB to -35 dB below the peak, times two
                double t5 = -1, t35 = -1;
                const size_t peakAt = (size_t) (std::max_element (env.begin(), env.end()) - env.begin());
                for (size_t k = peakAt; k < env.size(); ++k)
                {
                    const double t = (double) k * 512 / sr;
                    if (t5 < 0 && env[k] < peakDb - 5) t5 = t;
                    if (t35 < 0 && env[k] < peakDb - 35) t35 = t;
                }
                const double measured = t35 > 0 && t5 >= 0 ? (t35 - t5) * 2.0 : -1.0;
                report (measured > rt * 0.6 && measured < rt * 1.5, "Geode decay " + juce::String (rt, 1) + " s measured " + juce::String (measured, 2) + " s");
            }
            {
                auto rv = make ("amethyst.reverb");
                set (*rv, "time", 20.0f); set (*rv, "bass", 2.5f); set (*rv, "mod", 100); set (*rv, "wet", 100);
                juce::Random rnd (3);
                float peak = 0.0f;
                bool finite = true;
                for (int pos = 0; pos < (int) (sr * 20); pos += 512)
                {
                    juce::AudioBuffer<float> b (2, 512);
                    for (int i = 0; i < 512; ++i) b.setSample (0, i, 0.3f * (rnd.nextFloat() - 0.5f)), b.setSample (1, i, 0.3f * (rnd.nextFloat() - 0.5f));
                    rv->processBlock (b, midi);
                    peak = juce::jmax (peak, b.getMagnitude (0, 512));
                    finite = finite && std::isfinite (b.getSample (0, 511));
                }
                report (finite && peak < 4.0f, "Geode stable at 20 s decay (peak " + juce::String (peak, 2) + ")");
            }

            // Facet EQ: low cut at 1 kHz, steeper slopes cut a 250 Hz tone harder (2 octaves below)
            for (int slope = 0; slope < 3; ++slope)
            {
                auto eq = make ("amethyst.eq");
                set (*eq, "b1_type", 0); set (*eq, "b1_freq", 1000); set (*eq, "b1_q", 0.7071f); set (*eq, "b1_slope", (float) slope);
                double inRms = 0, outRms = 0;
                for (int blk = 0, n = 0; blk < 60; ++blk)
                {
                    juce::AudioBuffer<float> b (2, 512);
                    for (int i = 0; i < 512; ++i, ++n) { const float v = 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * 250.0f * (float) n / (float) sr); b.setSample (0, i, v); b.setSample (1, i, v); }
                    if (blk > 30) inRms += b.getRMSLevel (0, 0, 512);
                    eq->processBlock (b, midi);
                    if (blk > 30) outRms += b.getRMSLevel (0, 0, 512);
                }
                const double att = juce::Decibels::gainToDecibels (outRms / inRms);
                const double expect[] = { -24.0, -48.0, -96.0 };
                report (att < expect[slope] * 0.75 && att > expect[slope] * 1.35 - 10.0,
                        "Facet low cut " + juce::String (12 << slope) + " dB/oct: 2 octaves down = " + juce::String (att, 1) + " dB");
            }

            // Prism: sidechain ducks a steady tone when the other track is loud
            {
                auto dyn = make ("amethyst.dynamics");
                set (*dyn, "mode", 1); set (*dyn, "thresh", -30); set (*dyn, "ratio", 10); set (*dyn, "att", 1); set (*dyn, "rel", 50); set (*dyn, "ceil", 0);
                std::vector<float> loud (512, 0.8f), silent (512, 0.0f);
                bool scLoud = true;
                auto saved = AmethystPlugin::sidechainSource();
                AmethystPlugin::sidechainSource() = [&] (int track, int, const float*& l, const float*& r)
                {
                    if (track != 3) return false;
                    l = r = scLoud ? loud.data() : silent.data();
                    return true;
                };
                auto runTone = [&] (int blocks)
                {
                    float last = 0;
                    for (int blk = 0; blk < blocks; ++blk)
                    {
                        juce::AudioBuffer<float> b (2, 512);
                        for (int i = 0; i < 512; ++i) b.setSample (0, i, 0.05f), b.setSample (1, i, 0.05f);
                        dyn->processBlock (b, midi);
                        last = b.getRMSLevel (0, 0, 512);
                    }
                    return last;
                };
                set (*dyn, "sc_track", 0.0f);
                const float own = runTone (20);          // quiet tone below threshold: untouched
                set (*dyn, "sc_track", 3.0f);
                const float ducked = runTone (20);       // loud sidechain: pushed down
                scLoud = false;
                const float released = runTone (40);
                AmethystPlugin::sidechainSource() = saved;
                report (ducked < own * 0.3f && released > own * 0.9f,
                        "Prism sidechain: own " + juce::String (own, 3) + ", ducked " + juce::String (ducked, 3) + ", released " + juce::String (released, 3));
            }

            // Prism gate: no chatter on a level hovering around the threshold, no click when it opens
            {
                auto dyn = make ("amethyst.dynamics");
                set (*dyn, "ceil", 0); set (*dyn, "g_thr", -40); set (*dyn, "g_range", 40); set (*dyn, "g_hold", 60); set (*dyn, "g_rel", 150);
                const float thrLin = juce::Decibels::decibelsToGain (-40.0f);
                int flips = 0;
                bool wasOpen = false;
                juce::int64 n = 0;
                for (int blk = 0; blk < (int) (sr * 3 / 64); ++blk)
                {
                    juce::AudioBuffer<float> b (2, 64), dry (2, 64);
                    for (int i = 0; i < 64; ++i, ++n)
                    {
                        const double t = (double) n / sr;
                        const float a = thrLin * juce::Decibels::decibelsToGain ((float) (3.0 * std::sin (2.0 * juce::MathConstants<double>::pi * 8.0 * t)));
                        const float v = a * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 300.0 * t);
                        b.setSample (0, i, v); b.setSample (1, i, v);
                    }
                    dry.makeCopyOf (b);
                    dyn->processBlock (b, midi);
                    const float ratio = b.getRMSLevel (0, 0, 64) / juce::jmax (1.0e-9f, dry.getRMSLevel (0, 0, 64));
                    const bool open = ratio > 0.5f;
                    if (blk > 20 && open != wasOpen) ++flips;
                    wasOpen = open;
                }
                report (flips <= 1, "Prism gate: " + juce::String (flips) + " open/close flips in 3 s of a level wobbling +/-3 dB around the threshold");

                auto dyn2 = make ("amethyst.dynamics");
                set (*dyn2, "ceil", 0); set (*dyn2, "g_thr", -40); set (*dyn2, "g_range", 40);
                for (int k = 0; k < 100; ++k)   // ~1 s of silence: the gate fully closes
                {
                    juce::AudioBuffer<float> silent (2, 512);
                    silent.clear();
                    dyn2->processBlock (silent, midi);
                }
                juce::AudioBuffer<float> tone (2, 512);
                for (int i = 0; i < 512; ++i) { const float v = 0.3f * std::sin (0.06f * (float) i); tone.setSample (0, i, v); tone.setSample (1, i, v); }
                juce::AudioBuffer<float> dryTone (tone);
                dyn2->processBlock (tone, midi);
                // gain over the first 10 samples vs the end of the block
                float early = 0, late = 0;
                for (int i = 2; i < 12; ++i) early = juce::jmax (early, std::abs (tone.getSample (0, i)) / juce::jmax (1.0e-6f, std::abs (dryTone.getSample (0, i))));
                late = tone.getRMSLevel (0, 400, 100) / dryTone.getRMSLevel (0, 400, 100);
                report (early < 0.6f && late > 0.97f, "Prism gate opens smoothly (first samples x" + juce::String (early, 2) + ", then x" + juce::String (late, 2) + ")");
            }

            // Prism compressor: static curve and auto gain
            {
                for (int det = 0; det < 2; ++det)
                {
                    auto dyn = make ("amethyst.dynamics");
                    set (*dyn, "mode", 1); set (*dyn, "character", 0); set (*dyn, "detect", (float) det); set (*dyn, "thresh", -20); set (*dyn, "ratio", 4);
                    set (*dyn, "knee", 0); set (*dyn, "att", 1); set (*dyn, "rel", 50); set (*dyn, "ceil", 0);
                    const float inAmp = juce::Decibels::decibelsToGain (-10.0f);
                    float outPeak = 0;
                    for (int blk = 0, n = 0; blk < 200; ++blk)
                    {
                        juce::AudioBuffer<float> b (2, 512);
                        for (int i = 0; i < 512; ++i, ++n) { const float v = inAmp * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f * (float) n / (float) sr); b.setSample (0, i, v); b.setSample (1, i, v); }
                        dyn->processBlock (b, midi);
                        if (blk > 150) outPeak = juce::jmax (outPeak, b.getMagnitude (0, 0, 512));
                    }
                    const float outDb = juce::Decibels::gainToDecibels (outPeak);
                    report (std::abs (outDb - (-17.5f)) < 0.8f, juce::String (det == 0 ? "Peak" : "RMS") + " compressor: -10 dB in, 4:1 at -20 -> " + juce::String (outDb, 2) + " dB (expect -17.5)");
                }
            }

            // Crystal Echo: every style stays bounded at high feedback
            for (int style = 0; style < 4; ++style)
            {
                auto echo = make ("amethyst.echo");
                set (*echo, "style", (float) style); set (*echo, "fb", 92); set (*echo, "drive", 60); set (*echo, "mod", 60); set (*echo, "wet", 100);
                float peak = 0;
                bool finite = true;
                for (int pos = 0; pos < (int) (sr * 10); pos += 512)
                {
                    juce::AudioBuffer<float> b (2, 512);
                    b.clear();
                    if (pos < sr) for (int i = 0; i < 512; ++i) b.setSample (0, i, 0.5f * std::sin ((float) (pos + i) * 0.05f)), b.setSample (1, i, b.getSample (0, i));
                    echo->processBlock (b, midi);
                    peak = juce::jmax (peak, b.getMagnitude (0, 512));
                    finite = finite && std::isfinite (b.getSample (0, 511));
                }
                report (finite && peak < 6.0f, "Crystal Echo style " + juce::String (style) + " stable (peak " + juce::String (peak, 2) + ")");
            }

            // A/B compare restores each slot's settings
            {
                auto p = make ("amethyst.reverb");
                auto& am = dynamic_cast<AmethystPlugin&> (*p);
                am.setParam ("wet", 10.0f);
                am.selectAB (1);
                am.setParam ("wet", 80.0f);
                am.selectAB (0);
                const float a = am.get ("wet");
                am.selectAB (1);
                const float b = am.get ("wet");
                report (std::abs (a - 10.0f) < 0.5f && std::abs (b - 80.0f) < 0.5f, "A/B compare: A " + juce::String (a, 1) + ", B " + juce::String (b, 1));
            }
            if (failed > 0) return 1;
        }

        // ---- animated profile pictures: a 2-frame GIF (red 200 ms, blue 200 ms) ----
        {
            // tiny GIF writer: every pixel is sent as [clear][index] with 3-bit codes, which is valid LZW
            auto gifFrame = [] (juce::MemoryOutputStream& o, int colour)
            {
                o.writeByte (0x21); o.writeByte ((char) 0xf9); o.writeByte (4); o.writeByte (0); o.writeShort (20); o.writeByte (0); o.writeByte (0);
                o.writeByte (0x2c); o.writeShort (0); o.writeShort (0); o.writeShort (4); o.writeShort (4); o.writeByte (0);
                o.writeByte (2);   // min code size
                juce::MemoryBlock bits;
                juce::uint32 acc = 0; int n = 0;
                juce::MemoryOutputStream data;
                auto put = [&] (int code) { acc |= (juce::uint32) code << n; n += 3; while (n >= 8) { data.writeByte ((char) (acc & 0xff)); acc >>= 8; n -= 8; } };
                for (int i = 0; i < 16; ++i) { put (4); put (colour); }
                put (5);   // end
                if (n > 0) data.writeByte ((char) (acc & 0xff));
                o.writeByte ((char) data.getDataSize()); o.write (data.getData(), data.getDataSize()); o.writeByte (0);
            };
            juce::MemoryOutputStream gif;
            gif.write ("GIF89a", 6); gif.writeShort (4); gif.writeShort (4); gif.writeByte ((char) 0x81); gif.writeByte (0); gif.writeByte (0);
            const juce::uint8 table[12] = { 0, 0, 0, 255, 0, 0, 0, 0, 255, 255, 255, 255 };   // black, red, blue, white
            gif.write (table, 12);
            gifFrame (gif, 1);
            gifFrame (gif, 2);
            gif.writeByte (0x3b);

            std::vector<juce::Colour> seen;
            std::vector<int> delays;
            const bool decoded = AvatarMedia::decodeGif (gif.getData(), gif.getDataSize(), [&] (const juce::Image& canvas, int d)
            {
                seen.push_back (canvas.getPixelAt (2, 2));
                delays.push_back (d);
                return true;
            });
            const bool framesOk = decoded && seen.size() == 2 && seen[0] == juce::Colours::red && seen[1] == juce::Colour (0, 0, 255)
                                  && delays[0] == 200 && delays[1] == 200;

            auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("amethyst_test", ".gif");
            tmp.replaceWithData (gif.getData(), gif.getDataSize());
            juce::String err;
            const auto strip = AvatarMedia::load (tmp, err);
            tmp.deleteFile();
            juce::String mime;
            const auto bytes = AvatarMedia::encode (strip, mime);
            // 400 ms of animation on the 80 ms grid = 5 frames: 3 red, 2 blue
            const bool stripOk = AvatarMedia::isAnimated (strip) && AvatarMedia::frameCount (strip) == 5
                                 && strip.getPixelAt (48, 48).getRed() > 200 && strip.getPixelAt (4 * 96 + 48, 48).getBlue() > 200
                                 && mime == "image/jpeg" && bytes.getSize() > 0 && bytes.getSize() < 950 * 1024;
            std::printf ("[%s] animated GIF avatar: decoded %d frames, strip of %d frames, %d KB %s\n", framesOk && stripOk ? "ok" : "FAIL",
                         (int) seen.size(), AvatarMedia::frameCount (strip), (int) (bytes.getSize() / 1024), mime.toRawUTF8());
            if (! framesOk || ! stripOk) return 1;
        }

        // ---- count-in and punch-in ----
        {
            AudioEngine eng;
            const double esr = eng.getSampleRate();
            eng.bpm = 120.0;
            auto* t = eng.addTrack ("Vox");
            t->armed = true;
            std::vector<float> mic (512, 0.25f);
            const float* ins[1] = { mic.data() };
            juce::AudioBuffer<float> out (2, 512);
            juce::AudioIODeviceCallbackContext cbCtx {};
            auto runUntil = [&] (juce::int64 pos)
            {
                for (int guard = 0; eng.getTransportPosition() < pos && guard < 100000; ++guard)
                {
                    eng.audioDeviceIOCallbackWithContext (ins, 1, out.getArrayOfWritePointers(), 2, 512, cbCtx);
                    juce::Thread::sleep (3);   // ~4x real time, so the background take writer keeps up as it would live
                }
            };

            // count-in: 1 bar (2 s at 120 BPM) of lead-in that is heard but not recorded
            const auto bar = (juce::int64) (2.0 * esr);
            eng.setTransportPosition (0);
            eng.record (bar);
            const bool startedEarly = eng.getTransportPosition() == -bar;
            runUntil ((juce::int64) (4.0 * esr));
            eng.stop();
            auto takes = eng.consumeNewTakes();
            const bool countOk = startedEarly && takes.size() == 1 && takes[0].second->startSample.load() == 0
                                 && std::abs ((double) takes[0].second->getLength() - 4.0 * esr) < 1024;
            std::printf ("[%s] count-in: starts 1 bar early, take begins at the record point (%.2f s long)\n", countOk ? "ok" : "FAIL",
                         takes.empty() ? 0.0 : takes[0].second->getLength() / esr);

            // punch 1 s - 2 s over that 4 s take: old audio stays outside, new take fills the gap
            eng.punchIn = (juce::int64) esr;
            eng.punchOut = (juce::int64) (2.0 * esr);
            eng.punchEnabled = true;
            eng.setTransportPosition (0);
            eng.record (0);
            runUntil ((juce::int64) (2.5 * esr));
            const bool wasPunch = eng.isPunchRecording();
            eng.stop();
            for (auto& [tr, take] : eng.consumeNewTakes())
                eng.applyPunchTake (*tr, take);
            auto clips = eng.getClips (*t);
            std::sort (clips.begin(), clips.end(), [] (auto& a, auto& b) { return a->startSample.load() < b->startSample.load(); });
            juce::String layout;
            for (auto& c : clips)
                layout << juce::String (c->startSample.load() / esr, 2) << "-" << juce::String (c->getEnd() / esr, 2) << " ";
            const bool punchOk = wasPunch && clips.size() == 3
                                 && clips[0]->startSample.load() == 0 && clips[0]->getEnd() == (juce::int64) esr
                                 && clips[1]->startSample.load() == (juce::int64) esr && clips[1]->getEnd() == (juce::int64) (2.0 * esr)
                                 && clips[2]->startSample.load() == (juce::int64) (2.0 * esr) && std::abs ((double) clips[2]->getEnd() - 4.0 * esr) < 1024;
            std::printf ("[%s] punch-in replaces only 1-2 s: %s\n", punchOk ? "ok" : "FAIL", layout.toRawUTF8());
            for (auto& c : clips) c->file.deleteFile();   // test takes
            if (! countOk || ! punchOk) return 1;
        }

        // ---- input gain: +12 dB makes the monitored mic 4x louder ----
        {
            AudioEngine eng;
            auto* t = eng.addTrack ("Mic");
            t->monitor = true;
            std::vector<float> mic (512, 0.05f);
            const float* ins[1] = { mic.data() };
            juce::AudioBuffer<float> out (2, 512);
            juce::AudioIODeviceCallbackContext cbCtx {};
            eng.audioDeviceIOCallbackWithContext (ins, 1, out.getArrayOfWritePointers(), 2, 512, cbCtx);
            const float plain = out.getMagnitude (0, 0, 512);
            t->inputGainDb = 12.0f;
            eng.audioDeviceIOCallbackWithContext (ins, 1, out.getArrayOfWritePointers(), 2, 512, cbCtx);
            const float boosted = out.getMagnitude (0, 0, 512);
            const float ratio = plain > 0 ? boosted / plain : 0.0f;
            std::printf ("[%s] input gain +12 dB: monitor level x%.2f\n", std::abs (ratio - 3.98f) < 0.2f ? "ok" : "FAIL", ratio);
            if (std::abs (ratio - 3.98f) >= 0.2f) return 1;
        }

        // ---- automation: envelope values and the engine applying them ----
        {
            AudioEngine eng;
            auto* t = eng.addTrack ("T");
            auto* lane = eng.addAutomationTrack (*t, Automation::Kind::Pan);
            lane->automation->points = { { 0.0, 0.0f }, { 2.0, 1.0f } };
            const float mid = lane->automation->valueAt (1.0);
            juce::AudioBuffer<float> outBuf (2, 512);
            eng.setTransportPosition ((juce::int64) (1.5 * eng.getSampleRate()));
            eng.play();
            juce::AudioIODeviceCallbackContext cbCtx {};
            eng.audioDeviceIOCallbackWithContext (nullptr, 0, outBuf.getArrayOfWritePointers(), 2, 512, cbCtx);   // one block applies the automation
            eng.stop();
            const bool ok = std::abs (mid - 0.5f) < 1.0e-4f && t->pan.load() > 0.0f
                            && AudioEngine::volumeDbToNormalised (AudioEngine::normalisedToVolumeDb (0.63f)) > 0.62f;
            std::printf ("[%s] automation: mid %.2f, pan after playback %.2f\n", ok ? "ok" : "FAIL", mid, t->pan.load());
            if (! ok) return 1;
        }

        if (runSecondWaveSelfTests() > 0)
            return 1;

        std::printf ("selftest OK\n");
        return 0;
    }


    // FL Studio import checks.
    //   AMETHYST.exe --flp-test "a.flp" "b.flp"   full import of each file, prints the report
    //   AMETHYST.exe --flp-scan "folder"          reader only, over every .flp below the folder
    //   AMETHYST.exe --fst-test "preset.fst" "folder"   loads each FL preset onto a fresh channel, prints the report
    //   and every parameter that moved away from its default
    static int runFstTest (const juce::StringArray& args)
    {
        juce::Array<juce::File> files;
        for (int i = args.indexOf ("--fst-test") + 1; i < args.size(); ++i)
        {
            juce::File f (args[i].unquoted());
            if (f.isDirectory()) files.addArray (f.findChildFiles (juce::File::findFiles, true, "*.fst"));
            else if (f.existsAsFile()) files.add (f);
        }
        if (files.isEmpty()) { std::printf ("no .fst files given\n"); return 2; }

        AudioEngine eng;
        int failures = 0;
        for (auto& f : files)
        {
            eng.clearTracks();
            auto* t = eng.addTrack ("Vocal");
            FlpImport::Report report;
            const auto r = FlpImport::importPreset (eng, *t, f, report);
            std::printf ("==== %s\n", f.getFileName().toRawUTF8());
            if (r.failed()) { ++failures; std::printf ("[FAIL] %s\n", r.getErrorMessage().toRawUTF8()); continue; }
            std::printf ("%s", report.toText().toRawUTF8());
            std::printf ("  channel: vol %.1f dB, pan %.2f\n", t->volumeDb.load(), t->pan.load());
            for (size_t s = 0; s < t->inserts.size(); ++s)
            {
                auto* p = t->inserts[s] != nullptr ? t->inserts[s]->plugin.get() : nullptr;
                if (p == nullptr) continue;
                juce::String moved;
                for (auto* prm : p->getParameters())
                    if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                        if (std::abs (rp->getValue() - rp->getDefaultValue()) > 0.0005f)
                            moved << " " << rp->getParameterID() << "=" << rp->getCurrentValueAsText();
                std::printf ("  slot %d %s%s%s:%s\n", (int) s + 1, p->getName().toRawUTF8(), t->inserts[s]->bypassed ? " [bypassed]" : "",
                             t->inserts[s]->mix < 0.999f ? (" mix " + juce::String (juce::roundToInt (t->inserts[s]->mix * 100)) + "%").toRawUTF8() : "",
                             moved.isEmpty() ? " (defaults)" : moved.toRawUTF8());
            }
        }
        std::printf ("%d presets, %d failed\n", files.size(), failures);
        return failures == 0 ? 0 : 1;
    }

    static int runFlpTest (const juce::StringArray& args, bool scanOnly)
    {
        juce::Array<juce::File> files;
        for (int i = args.indexOf (scanOnly ? "--flp-scan" : "--flp-test") + 1; i < args.size(); ++i)
        {
            juce::File f (args[i].unquoted());
            if (f.isDirectory()) files.addArray (f.findChildFiles (juce::File::findFiles, true, "*.flp"));
            else if (f.existsAsFile()) files.add (f);
        }
        if (files.isEmpty()) { std::printf ("no .flp files given\n"); return 2; }

        int failures = 0;
        if (scanOnly)
        {
            std::map<juce::String, std::pair<int, int>> byVersion;   // major -> ok, total
            int clips = 0, inserts = 0, effects = 0, fixed = 0;
            for (auto& f : files)
            {
                Flp::Project p;
                const auto r = Flp::read (f, p);
                auto& v = byVersion[p.version.upToFirstOccurrenceOf (".", false, false).paddedLeft (' ', 3)];
                ++v.second;
                if (r.failed()) { ++failures; std::printf ("[FAIL] %s: %s\n", f.getFullPathName().toRawUTF8(), r.getErrorMessage().toRawUTF8()); continue; }
                ++v.first;
                fixed += p.used172Fix ? 1 : 0;
                for (auto& it : p.items) clips += it.channel >= 0 ? 1 : 0;
                inserts += (int) p.inserts.size();
                for (auto& ins : p.inserts) effects += (int) ins.plugins.size();
            }
            for (auto& [v, c] : byVersion) std::printf ("FL %s: %d/%d read\n", v.toRawUTF8(), c.first, c.second);
            std::printf ("%d files, %d failed, %d needed the FL 26 rule, %d audio clips, %d inserts, %d effects\n",
                         files.size(), failures, fixed, clips, inserts, effects);
            return failures == 0 ? 0 : 1;
        }

        AudioEngine eng;
        for (auto& f : files)
        {
            FlpImport::Report report;
            const auto r = FlpImport::importProject (eng, f, report);
            if (r.failed()) { ++failures; std::printf ("[FAIL] %s: %s\n", f.getFullPathName().toRawUTF8(), r.getErrorMessage().toRawUTF8()); continue; }
            std::printf ("==== %s\n%s", f.getFullPathName().toRawUTF8(), report.toText().toRawUTF8());
            for (auto* t : eng.getTracks())
                std::printf ("  track \"%s\" clips=%d fx=%d vol=%.1fdB pan=%.2f%s\n", t->name.toRawUTF8(), (int) t->clips.size(), t->countPlugins(),
                             t->volumeDb.load(), t->pan.load(), t->playThrough != nullptr ? (" -> plays through " + t->playThrough->name).toRawUTF8() : "");
            std::printf ("  master fx=%d\n", eng.getMaster().countPlugins());
            if (args.contains ("--params"))
            {
                auto dump = [] (Track& t)
                {
                    for (auto& slot : t.inserts)
                    {
                        if (slot->plugin == nullptr) continue;
                        juce::String line = "    [" + t.name + "] " + slot->plugin->getName() + ":";
                        if (slot->plugin->getPluginDescription().pluginFormatName == "VST3")
                        {
                            // Show a bit of what the plug-in reports back, to confirm FL's settings landed.
                            juce::MemoryBlock mb;
                            slot->plugin->getStateInformation (mb);
                            if (auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize()))
                                if (auto* comp = xml->getChildByName ("IComponent"))
                                {
                                    juce::MemoryBlock state;
                                    state.fromBase64Encoding (comp->getAllSubText());
                                    const auto text = state.toString();
                                    const int at = text.indexOf ("id=\"KeyP\"");
                                    line << " state " << (int) state.getSize() << " bytes" << (at >= 0 ? " " + text.substring (at, at + 40) : juce::String());
                                }
                        }
                        for (auto* p : slot->plugin->getParameters())
                            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                            {
                                if (line.length() > 2000) break;
                                line << " " << rp->getParameterID() << "=" << rp->getCurrentValueAsText();
                            }
                        std::printf ("%s\n", line.toRawUTF8());
                    }
                };
                dump (eng.getMaster());
                for (auto* t : eng.getTracks()) dump (*t);
            }
        }
        eng.clearTracks();
        return failures == 0 ? 0 : 1;
    }

    // Exercises the Supabase client against a server (normally a local mock):
    //   AMETHYST.exe --account-test http://127.0.0.1:8765 test-anon
    static int runAccountTest (const juce::StringArray& args)
    {
        const int i = args.indexOf ("--account-test");
        if (i < 0 || i + 2 >= args.size())
            return 2;

        SupabaseApi api (args[i + 1], args[i + 2]);
        int failures = 0;
        auto check = [&failures] (bool ok, const char* what, const juce::String& detail)
        {
            std::printf ("[%s] %s  %s\n", ok ? "ok" : "FAIL", what, detail.toRawUTF8());
            if (! ok) ++failures;
        };

        auto bad = api.signIn ("singer@example.com", "wrong-password");
        check (! bad.ok && bad.error.isNotEmpty(), "wrong password rejected", bad.error);

        auto good = api.signIn ("singer@example.com", "correct-horse");
        check (good.ok && good.accessToken.isNotEmpty() && good.userId.isNotEmpty(), "sign in", good.userId);

        auto ent = api.fetchEntitlement (good.accessToken, good.userId);
        check (ent.ok && ent.isPro, "pro entitlement read", ent.plan);

        auto freeUser = api.signIn ("free@example.com", "correct-horse");
        auto freeEnt = api.fetchEntitlement (freeUser.accessToken, freeUser.userId);
        check (freeEnt.ok && ! freeEnt.isPro, "free user is not pro", freeEnt.plan);

        auto expired = api.signIn ("lapsed@example.com", "correct-horse");
        auto expEnt = api.fetchEntitlement (expired.accessToken, expired.userId);
        check (expEnt.ok && ! expEnt.isPro, "expired pro_until is not pro", expEnt.proUntil.toISO8601 (true));

        auto ultraUser = api.signIn ("ultra@example.com", "correct-horse");
        auto ultraEnt = api.fetchEntitlement (ultraUser.accessToken, ultraUser.userId);
        check (ultraEnt.ok && ultraEnt.isUltra && ultraEnt.isPro, "ultra pro user gets Ultra (and Pro)", ultraEnt.plan);
        check (ent.ok && ! ent.isUltra, "pro user is not ultra", {});

        auto legacy = api.signIn ("legacy@example.com", "correct-horse");
        auto legacyEnt = api.fetchEntitlement (legacy.accessToken, legacy.userId);
        check (legacyEnt.ok && legacyEnt.isPro && ! legacyEnt.isUltra, "works before the Ultra migration is run", legacyEnt.error);

        auto noToken = api.fetchEntitlement ("not-a-token", good.userId);
        check (! noToken.ok, "entitlement needs a valid token", noToken.error);

        {
            juce::Image pic (juce::Image::ARGB, 16, 16, true);
            pic.clear (pic.getBounds(), juce::Colours::purple);
            juce::MemoryOutputStream pngOut;
            juce::PNGImageFormat().writeImageToStream (pic, pngOut);
            juce::MemoryBlock png (pngOut.getData(), pngOut.getDataSize());

            check (api.uploadAvatar (good.accessToken, good.userId, png).isEmpty(), "pro user uploads profile picture", {});
            auto got = api.downloadAvatar (good.accessToken, good.userId);
            check (got.ok && got.data == png, "profile picture downloads intact", juce::String ((int) got.data.getSize()) + " bytes");
            const auto freeErr = api.uploadAvatar (freeUser.accessToken, freeUser.userId, png);
            check (freeErr.containsIgnoreCase ("Pro"), "free user can't upload a picture", freeErr);
            check (api.deleteAvatar (good.accessToken, good.userId).isEmpty(), "profile picture deleted", {});
            auto gone = api.downloadAvatar (good.accessToken, good.userId);
            check (! gone.ok && gone.notFound, "deleted picture reports not found", gone.error);
        }

        auto refreshed = api.refresh (good.refreshToken);
        check (refreshed.ok && refreshed.accessToken != good.accessToken, "refresh token", {});

        auto signup = api.signUp ("new@example.com", "secret123");
        check (signup.ok && signup.needsConfirmation, "sign up needs email confirmation", signup.userId);

        check (api.sendPasswordReset ("singer@example.com").isEmpty(), "password reset", {});
        api.signOut (refreshed.accessToken);

        SupabaseApi offline ("http://127.0.0.1:9", "x");
        auto off = offline.signIn ("a@b.c", "x");
        check (! off.ok && off.networkError, "offline is reported as network error", off.error);

        SupabaseApi unconfigured ("", "");
        check (! unconfigured.isConfigured() && ! unconfigured.signIn ("a", "b").ok, "unconfigured build can't sign in", {});

        {
            const std::string secret = "rt-session-secret";
            std::vector<unsigned char> enc, dec;
            const bool encOk = amethystProtect (secret.data(), secret.size(), enc, true);
            const bool decOk = encOk && amethystProtect (enc.data(), enc.size(), dec, false);
            const bool hidden = encOk && std::search (enc.begin(), enc.end(), secret.begin(), secret.end()) == enc.end();
            check (decOk && std::string (dec.begin(), dec.end()) == secret && hidden, "secure session storage round-trip", {});
        }

        std::printf (failures == 0 ? "account test OK\n" : "account test FAILED (%d)\n", failures);
        return failures == 0 ? 0 : 1;
    }

    // Checks that tool windows really become owned by the main window. Windows stay hidden.
    static int runWindowTest()
    {
        juce::DocumentWindow mainWin ("main", juce::Colours::black, 0, true);
        juce::DocumentWindow toolWin ("tool", juce::Colours::black, 0, true);
        FloatAboveMain::mainWindow() = &mainWin;
        FloatAboveMain::attach (toolWin);
        FloatAboveMain::mainWindow() = nullptr;

        // attach() succeeded via OS ownership if it didn't fall back to always-on-top
        const bool owned = amethystSetWindowOwner (toolWin.getWindowHandle(), mainWin.getWindowHandle());
        std::printf ("[%s] tool window owned by main window\n", owned ? "ok" : "FAIL");
        std::printf ("[%s] no always-on-top fallback needed\n", ! toolWin.isAlwaysOnTop() ? "ok" : "FAIL");
        const bool ok = owned && ! toolWin.isAlwaysOnTop();
        std::printf (ok ? "window test OK\n" : "window test FAILED\n");
        return ok ? 0 : 1;
    }

    void initialise (const juce::String& commandLine) override
    {
        // Feeds key presses through the real OS window path (hidden window): Space must start / stop playback
        // even when no component has keyboard focus.
        if (commandLine.contains ("--key-test"))
        {
            juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
            MainComponent::isTestInstance() = true;
            auto win = std::make_unique<MainWindow> ("AMETHYST key test", false);
            auto* mc = dynamic_cast<MainComponent*> (win->getContentComponent());
            int code = 1;
            if (auto* peer = win->getPeer(); peer != nullptr && mc != nullptr)
            {
                if (auto* f = juce::Component::getCurrentlyFocusedComponent()) f->giveAwayKeyboardFocus();
                const bool before = mc->isTransportPlaying();
                peer->handleKeyPress (juce::KeyPress (juce::KeyPress::spaceKey));
                const bool after = mc->isTransportPlaying();
                peer->handleKeyPress (juce::KeyPress (juce::KeyPress::spaceKey));
                const bool again = mc->isTransportPlaying();
                std::printf ("[%s] Space with nothing focused: playing %d -> %d -> %d\n", (! before && after && ! again) ? "ok" : "FAIL", before, after, again);
                code = (! before && after && ! again) ? 0 : 1;

                // optional: picture of the top of the main window (toolbar + transport)
                const auto args = juce::StringArray::fromTokens (commandLine, true);
                if (const int i = args.indexOf ("--key-test"); i + 1 < args.size())
                {
                    auto img = mc->createComponentSnapshot (mc->getLocalBounds().withHeight (130));
                    juce::FileOutputStream fos (juce::File (args[i + 1].unquoted()));
                    if (fos.openedOk()) { fos.setPosition (0); fos.truncate(); juce::PNGImageFormat().writeImageToStream (img, fos); }
                }
            }
            win = nullptr;
            setApplicationReturnValue (code);
            quit();
            return;
        }

        // Dev: how JUCE decodes an MP3 (length, first audible sample) next to its gapless header: --mp3-test file.mp3 ...
        if (commandLine.contains ("--mp3-test"))
        {
            const auto args = juce::StringArray::fromTokens (commandLine, true);
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            for (int i = args.indexOf ("--mp3-test") + 1; i < args.size(); ++i)
            {
                juce::File f (args[i].unquoted());
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
                if (r == nullptr) { std::printf ("%s: unreadable\n", f.getFileName().toRawUTF8()); continue; }
                const int n = (int) juce::jmin<juce::int64> (r->lengthInSamples, (juce::int64) r->sampleRate * 5);
                juce::AudioBuffer<float> b ((int) r->numChannels, n);
                r->read (&b, 0, n, 0, true, true);
                int first = -1;
                for (int s = 0; s < n && first < 0; ++s)
                    for (int c = 0; c < b.getNumChannels(); ++c)
                        if (std::abs (b.getSample (c, s)) > 0.001f) { first = s; break; }
                const auto g = AudioEngine::readMp3Gapless (f);
                std::printf ("%s: format %s, length %lld, sr %.0f, first audible %d | header: %s frames %lld delay %d padding %d -> trim start %d, length %lld\n",
                             f.getFileName().substring (0, 40).toRawUTF8(), r->getFormatName().toRawUTF8(), (long long) r->lengthInSamples, r->sampleRate, first,
                             g.found ? "gapless" : "no gapless info", (long long) g.frames, g.encoderDelay, g.padding, g.trimStart, (long long) g.validLength);
            }
            setApplicationReturnValue (0);
            quit();
            return;
        }

        // Dev: what this PC's saved account has, license-wise (no tokens or email printed): --license-status
        if (commandLine.contains ("--license-status"))
        {
            juce::PropertiesFile::Options opts;
            opts.applicationName = "AMETHYST";
            opts.filenameSuffix = ".settings";
            opts.folderName = "AMETHYST";
            opts.osxLibrarySubFolder = "Application Support";
            juce::PropertiesFile settings (opts);
            Account account (settings, {}, {});   // offline: only reads what's saved
            const auto s = account.getLicenseSummary();
            std::printf ("signed in: %s\nplan now (offline): %s\nlicense: %s\n", account.isSignedIn() ? "yes" : "no",
                         account.getTier() >= 2 ? "Ultra Pro" : account.getTier() == 1 ? "Pro" : "Free", s.text.toRawUTF8());
            std::printf ("times: %s\n", account.getLicenseTimesForDebug().toRawUTF8());
            setApplicationReturnValue (0);
            quit();
            return;
        }

        // Dev: checks a license token against the built-in public key: --license-verify <token>
        if (commandLine.contains ("--license-verify"))
        {
            const auto args = juce::StringArray::fromTokens (commandLine, true);
            const auto info = License::verify (args[args.indexOf ("--license-verify") + 1].unquoted());
            std::printf ("%s: %s tier %d plan %s exp %lld\n", info.valid ? "valid" : "INVALID", info.valid ? info.sub.toRawUTF8() : info.error.toRawUTF8(),
                         info.tier, info.plan.toRawUTF8(), (long long) info.exp);
            setApplicationReturnValue (info.valid ? 0 : 1);
            quit();
            return;
        }

        // Drives the AMETHYST 2026 features through a hidden main window: --ui-test [snapshot folder]
        if (commandLine.contains ("--ui-test"))
        {
            juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
            MainComponent::isTestInstance() = true;
            auto win = std::make_unique<MainWindow> ("AMETHYST UI test", false);
            int code = 1;
            if (auto* mc = dynamic_cast<MainComponent*> (win->getContentComponent()))
            {
                const auto args = juce::StringArray::fromTokens (commandLine, true);
                const int i = args.indexOf ("--ui-test");
                const juce::File folder = i + 1 < args.size() ? juce::File (args[i + 1].unquoted()) : juce::File();
                mc->setSize (1500, 900);
                const int fails = mc->runUiSelfTest (folder);
                std::printf (fails == 0 ? "ui test OK\n" : "ui test: %d failed\n", fails);
                code = fails == 0 ? 0 : 1;
            }
            win = nullptr;
            setApplicationReturnValue (code);
            quit();
            return;
        }

        // Talks to the Discord app: handshake with the given id and print Discord's reply (no activity is set).
        if (commandLine.contains ("--discord-test"))
        {
            const auto args = juce::StringArray::fromTokens (commandLine, true);
            const auto id = args[args.indexOf ("--discord-test") + 1];
            int code = 1;
            if (void* pipe = amethystIpcConnect())
            {
                juce::MemoryOutputStream frame;
                const juce::String json = "{\"v\":1,\"client_id\":\"" + id + "\"}";
                frame.writeInt (0);
                frame.writeInt ((int) json.getNumBytesAsUTF8());
                frame.write (json.toRawUTF8(), json.getNumBytesAsUTF8());
                amethystIpcWrite (pipe, frame.getData(), frame.getDataSize());
                char buf[4096] = {};
                int n = 0;
                for (int k = 0; k < 40 && n <= 0; ++k) { juce::Thread::sleep (50); n = amethystIpcRead (pipe, buf, sizeof (buf) - 1); }
                if (n >= 8)
                {
                    int op = 0; std::memcpy (&op, buf, 4);
                    std::printf ("connected; Discord replied (opcode %d): %s\n", op, juce::String::fromUTF8 (buf + 8, n - 8).toRawUTF8());
                    code = 0;
                }
                else
                    std::printf ("connected, but no reply (n=%d)\n", n);
                amethystIpcClose (pipe);
            }
            else
                std::printf ("Discord is not running (no IPC pipe)\n");
            setApplicationReturnValue (code);
            quit();
            return;
        }

        if (commandLine.contains ("--window-test"))
        {
            setApplicationReturnValue (runWindowTest());
            quit();
            return;
        }

        if (commandLine.contains ("--account-test"))
        {
            setApplicationReturnValue (runAccountTest (juce::StringArray::fromTokens (commandLine, true)));
            quit();
            return;
        }

        if (commandLine.contains ("--fst-test"))
        {
            setApplicationReturnValue (runFstTest (juce::StringArray::fromTokens (commandLine, true)));
            quit();
            return;
        }

        if (commandLine.contains ("--flp-test") || commandLine.contains ("--flp-scan"))
        {
            setApplicationReturnValue (runFlpTest (juce::StringArray::fromTokens (commandLine, true), commandLine.contains ("--flp-scan")));
            quit();
            return;
        }

        if (commandLine.contains ("--selftest"))
        {
            setApplicationReturnValue (runSelfTest());
            quit();
            return;
        }

        // Dev helper: render an editor offscreen to a PNG (no window), e.g. --snapshot amethyst.eq out.png
        if (commandLine.contains ("--snapshot"))
        {
            juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
            const auto args = juce::StringArray::fromTokens (commandLine.fromFirstOccurrenceOf ("--snapshot", false, false).trim(), true);
            juce::File out (args[1].unquoted());
            std::unique_ptr<juce::Component> comp;
            std::unique_ptr<juce::AudioPluginInstance> plugin;
            std::unique_ptr<AudioEngine> eng;
            if (const int ti = args.indexOf ("--theme"); ti >= 0)
            {
                Theme::applyTheme (args[ti + 1].getIntValue());
                lookAndFeel.refreshColours();
            }
            if (args[0] == "welcome")
            {
                WelcomeScreen::Callbacks cb;
                juce::Array<juce::File> recent;
                for (auto& f : AudioEngine::getProjectsFolder().findChildFiles (juce::File::findFiles, false, "*.amethyst"))
                    recent.add (f);
                WelcomeScreen w (cb, recent, args[2].getIntValue(), args[2].getIntValue() >= 2 ? "Ultra Pro" : "Pro", true);
                w.setSize (1200, 720);
                juce::Image img (juce::Image::ARGB, 1200, 720, true);
                {
                    juce::Graphics g (img);
                    Glass::background (g, img.getBounds());
                    w.paintEntireComponent (g, true);
                }
                juce::FileOutputStream fos (out);
                int code = 1;
                if (fos.openedOk()) { fos.setPosition (0); fos.truncate(); code = juce::PNGImageFormat().writeImageToStream (img, fos) ? 0 : 1; }
                setApplicationReturnValue (code);
                quit();
                return;
            }
            if (args[0] == "icon")
            {
                const auto img = renderAppIcon (1024);
                juce::FileOutputStream fos (out);
                int code = 1;
                if (fos.openedOk()) { fos.setPosition (0); fos.truncate(); code = juce::PNGImageFormat().writeImageToStream (img, fos) ? 0 : 1; }
                setApplicationReturnValue (code);
                quit();
                return;
            }
            if (args[0] == "knobmenu")
            {
                // the knob right-click menu, drawn with the real menu look
                struct Row { int kind; const char* text; bool enabled; };   // 0 header, 1 item
                const Row rows[] = { { 0, "Stop time:  800 ms", true }, { 0, "Automation", true }, { 1, "Create automation clip", true },
                                     { 1, "Edit automation clip", false }, { 1, "Init song with this position", false }, { 1, "Delete automation clip", false },
                                     { 0, "Value", true }, { 1, "Reset", true }, { 1, "Copy value", true }, { 1, "Paste value", false },
                                     { 1, "Type in value...", true }, { 1, "Randomize", true } };
                const int w = 260, rowH = 24;
                juce::Image img (juce::Image::ARGB, w, rowH * (int) std::size (rows) + 8, true);
                {
                    juce::Graphics g (img);
                    lookAndFeel.drawPopupMenuBackground (g, img.getWidth(), img.getHeight());
                    int y = 4;
                    for (auto& r : rows)
                    {
                        const juce::Rectangle<int> area (0, y, w, rowH);
                        if (r.kind == 0) lookAndFeel.drawPopupMenuSectionHeader (g, area, r.text);
                        else lookAndFeel.drawPopupMenuItem (g, area, false, r.enabled, std::strcmp (r.text, "Create automation clip") == 0, false, false,
                                                            r.text, {}, nullptr, nullptr);
                        y += rowH;
                    }
                }
                juce::FileOutputStream fos (out);
                int code = 1;
                if (fos.openedOk()) { fos.setPosition (0); fos.truncate(); code = juce::PNGImageFormat().writeImageToStream (img, fos) ? 0 : 1; }
                setApplicationReturnValue (code);
                quit();
                return;
            }
            if (args[0] == "nightcore")
            {
                eng = std::make_unique<AudioEngine>();
                comp = std::make_unique<NightcoreMaker> (*eng, nullptr);
            }
            else if ((plugin = Builtins::create (args[0])) != nullptr)
            {
                plugin->prepareToPlay (48000.0, 512);
                comp.reset (plugin->createEditor());
            }
            int code = 1;
            if (comp != nullptr)
            {
                juce::Image img (juce::Image::ARGB, comp->getWidth(), comp->getHeight(), true);
                {
                    juce::Graphics g (img);
                    g.fillAll (Theme::windowBg);
                    comp->paintEntireComponent (g, true);
                }
                juce::FileOutputStream fos (out);
                if (fos.openedOk())
                {
                    fos.setPosition (0);
                    fos.truncate();
                    code = juce::PNGImageFormat().writeImageToStream (img, fos) ? 0 : 1;
                }
            }
            comp = nullptr;
            plugin = nullptr;
            eng = nullptr;
            setApplicationReturnValue (code);
            quit();
            return;
        }

        // Dev helper: show one built-in effect's editor on its own, e.g. --preview amethyst.eq
        if (commandLine.contains ("--preview"))
        {
            juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
            previewPlugin = Builtins::create (commandLine.fromFirstOccurrenceOf ("--preview", false, false).trim().upToFirstOccurrenceOf (" ", false, false));
            if (previewPlugin != nullptr)
            {
                previewPlugin->prepareToPlay (48000.0, 512);
                previewWindow = std::make_unique<juce::DocumentWindow> (previewPlugin->getName(), Theme::windowBg, juce::DocumentWindow::closeButton);
                previewWindow->setUsingNativeTitleBar (true);
                previewWindow->setContentOwned (previewPlugin->createEditor(), true);
                previewWindow->setTopLeftPosition (40, 40);
                previewWindow->setVisible (true);
                return;
            }
        }

        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
        openFromCommandLine (commandLine);   // double-clicked .amethyst / .flp
    }

    // A project file passed by Windows (double-click, "Open with") opens in this window.
    void openFromCommandLine (const juce::String& commandLine)
    {
        const auto arg = commandLine.trim().unquoted();
        const juce::File f (juce::File::isAbsolutePath (arg) ? arg : juce::String());
        if (mainWindow == nullptr || ! f.existsAsFile())
            return;
        if (! (f.hasFileExtension ("amethyst") || f.hasFileExtension ("vdaw") || f.hasFileExtension ("flp")))
            return;
        if (auto* mc = dynamic_cast<MainComponent*> (mainWindow->getContentComponent()))
            mc->openFileFromSystem (f);
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (mainWindow == nullptr) return;
        mainWindow->setMinimised (false);
        mainWindow->toFront (true);
        openFromCommandLine (commandLine);
    }

    void shutdown() override
    {
        previewWindow = nullptr;
        previewPlugin = nullptr;
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (const juce::String& name, bool show = true)
            : juce::DocumentWindow (name, Theme::windowBg, juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (1200, 520, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            if (show) setVisible (true);
            else      addToDesktop (getDesktopWindowStyleFlags());   // real OS window, never shown (tests)
            FloatAboveMain::mainWindow() = this; // plug-in / clip windows float above this one
        }

        ~MainWindow() override
        {
            FloatAboveMain::mainWindow() = nullptr;
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

        // With no component focused, Windows delivers keys to the window itself: pass them to the app's shortcuts.
        bool keyPressed (const juce::KeyPress& key) override
        {
            if (auto* mc = dynamic_cast<MainComponent*> (getContentComponent()))
                return mc->keyPressed (key);
            return juce::DocumentWindow::keyPressed (key);
        }
    };

private:
    GlassLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<juce::AudioPluginInstance> previewPlugin;
    std::unique_ptr<juce::DocumentWindow> previewWindow;
};

START_JUCE_APPLICATION (VocalDAWApplication)
