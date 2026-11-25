#include <JuceHeader.h>
#include "AudioEngine.h"
#include "Loudness.h"
#include "Plugins/Builtins.h"
#include "ClipFx.h"
#include "License.h"
#include "LicensePublicKey.h"
#include "LicenseTestVectors.h"

// Self-tests for the AMETHYST 2026 second wave (effects, engine features). Called from --selftest.
// Each test prints [ok] / [FAIL]; returns the number of failures.

namespace
{
    int failures = 0;
    void report (bool ok, const juce::String& what)
    {
        std::printf ("[%s] %s\n", ok ? "ok" : "FAIL", what.toRawUTF8());
        if (! ok) ++failures;
    }

    std::unique_ptr<AmethystPlugin> make (const char* id, double sr = 48000.0, int bs = 512)
    {
        auto p = std::unique_ptr<AmethystPlugin> (dynamic_cast<AmethystPlugin*> (Builtins::create (id).release()));
        p->setPlayConfigDetails (2, 2, sr, bs);
        p->prepareToPlay (sr, bs);
        return p;
    }

    // Runs `p` over `in` in 512-sample blocks.
    juce::AudioBuffer<float> run (AmethystPlugin& p, const juce::AudioBuffer<float>& in)
    {
        juce::AudioBuffer<float> out (in);
        juce::MidiBuffer midi;
        for (int i = 0; i < out.getNumSamples(); i += 512)
        {
            const int n = juce::jmin (512, out.getNumSamples() - i);
            juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, i, n);
            p.processBlock (view, midi);
        }
        return out;
    }

    juce::AudioBuffer<float> sine (double freq, float amp, double seconds, double sr = 48000.0)
    {
        juce::AudioBuffer<float> b (2, (int) (seconds * sr));
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            const float v = amp * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / sr);
            b.setSample (0, i, v);
            b.setSample (1, i, v);
        }
        return b;
    }

    // Strongest frequency between lo and hi Hz in the last `seconds` of channel 0 (FFT + parabolic peak).
    double peakFrequency (const juce::AudioBuffer<float>& b, double lo, double hi, double sr = 48000.0)
    {
        constexpr int order = 15, N = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> data (2 * N, 0.0f);
        const int start = juce::jmax (0, b.getNumSamples() - N);
        for (int i = 0; i < N && start + i < b.getNumSamples(); ++i)
            data[(size_t) i] = b.getSample (0, start + i) * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N));
        fft.performFrequencyOnlyForwardTransform (data.data());
        int best = 1;
        for (int k = (int) (lo * N / sr); k < (int) (hi * N / sr); ++k)
            if (data[(size_t) k] > data[(size_t) best] || best < (int) (lo * N / sr)) best = k;
        const float a = data[(size_t) best - 1], m = data[(size_t) best], c = data[(size_t) best + 1];
        const double delta = 0.5 * (a - c) / (a - 2 * m + c + 1e-12);
        return (best + delta) * sr / N;
    }

    // Band energy (dB) of channel 0 between lo and hi Hz over the last 0.5 s.
    double bandDb (const juce::AudioBuffer<float>& b, double lo, double hi, double sr = 48000.0)
    {
        constexpr int order = 14, N = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> data (2 * N, 0.0f);
        const int start = juce::jmax (0, b.getNumSamples() - N);
        for (int i = 0; i < N; ++i)
            data[(size_t) i] = b.getSample (0, start + i) * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N));
        fft.performFrequencyOnlyForwardTransform (data.data());
        double e = 0.0;
        for (int k = (int) (lo * N / sr); k <= (int) (hi * N / sr); ++k) e += (double) data[(size_t) k] * data[(size_t) k];
        return 10.0 * std::log10 (e + 1e-20);
    }

    void testEffects()
    {
        const double sr = 48000.0;

        // Loudness: EBU Tech 3341 - a 1 kHz sine at -23 dBFS in both channels reads -23.0 LUFS
        {
            auto tone = sine (1000.0, juce::Decibels::decibelsToGain (-23.0f), 20.0);
            const auto r = Loudness::analyse (tone, sr);
            report (std::abs (r.integrated + 23.0) < 0.3 && std::abs (r.truePeakDb + 23.0f) < 0.5f,
                    "loudness: EBU reference tone reads " + juce::String (r.integrated, 2) + " LUFS, true peak " + juce::String (r.truePeakDb, 2) + " dBTP");
            auto quiet = sine (1000.0, juce::Decibels::decibelsToGain (-33.0f), 10.0);
            const double g = Loudness::normalise (quiet, sr, -14.0);
            const auto after = Loudness::analyse (quiet, sr);
            report (std::abs (after.integrated + 14.0) < 0.6 && after.truePeakDb <= -0.9f,
                    "loudness: normalise to -14 LUFS gives " + juce::String (after.integrated, 2) + " LUFS (gain " + juce::String (g, 1) + " dB, peak " + juce::String (after.truePeakDb, 2) + ")");
        }

        // Harmonizer: A3 (220 Hz) in C major, "3rd up" = C4 (261.6 Hz)
        {
            auto h = make ("amethyst.harmony");
            h->setParam ("key", 0); h->setParam ("scale", 0); h->setParam ("mode", 0);
            h->setParam ("v1", 7); h->setParam ("v2", 0); h->setParam ("dry", 0); h->setParam ("human", 0); h->setParam ("level", 100);
            auto out = run (*h, sine (220.0, 0.3f, 3.0));
            const double f = peakFrequency (out, 150.0, 600.0);
            report (std::abs (f - 261.63) < 4.0, "harmonizer: A3 in C major, 3rd up -> " + juce::String (f, 1) + " Hz (expect 261.6)");
            h->setParam ("key", 9); h->setParam ("scale", 1);   // A minor: 3rd up from A = C as well; 5th up = E4 329.6
            h->setParam ("v1", 9);
            out = run (*h, sine (220.0, 0.3f, 3.0));
            const double f5 = peakFrequency (out, 150.0, 600.0);
            report (std::abs (f5 - 329.63) < 5.0, "harmonizer: A3 in A minor, 5th up -> " + juce::String (f5, 1) + " Hz (expect 329.6)");
        }

        // Vocal Rider: a quiet steady vocal (-32 dB RMS) is pulled up by the full range (6 dB) towards -18
        {
            auto r = make ("amethyst.rider");
            const float amp = juce::Decibels::decibelsToGain (-32.0f) * std::sqrt (2.0f);
            auto in = sine (300.0, amp, 4.0);
            auto out = run (*r, in);
            const float gain = juce::Decibels::gainToDecibels (out.getRMSLevel (0, out.getNumSamples() - 4800, 4800) / in.getRMSLevel (0, in.getNumSamples() - 4800, 4800));
            report (std::abs (gain - 6.0f) < 0.8f, "vocal rider: quiet vocal raised by " + juce::String (gain, 1) + " dB (range 6)");
        }

        // Denoiser, Learn mode: learn 1 s of hiss, then clean tone + hiss; the hiss drops, the tone stays
        {
            auto d = make ("amethyst.denoise");
            d->setParam ("amount", 20);
            juce::Random rnd (7);
            auto addHiss = [&rnd] (juce::AudioBuffer<float>& b)
            {
                for (int i = 0; i < b.getNumSamples(); ++i)
                    for (int c = 0; c < 2; ++c)
                        b.setSample (c, i, b.getSample (c, i) + 0.003f * (rnd.nextFloat() * 2.0f - 1.0f));
            };
            juce::AudioBuffer<float> hiss (2, 48000);
            hiss.clear();
            addHiss (hiss);
            d->setParam ("learn", 1);
            run (*d, hiss);
            d->setParam ("learn", 0);
            d->setParam ("adaptive", 0);
            auto in = sine (500.0, 0.2f, 3.0);
            addHiss (in);
            auto out = run (*d, in);
            const double noiseDrop = bandDb (in, 4000, 12000) - bandDb (out, 4000, 12000);
            const double toneChange = bandDb (out, 450, 550) - bandDb (in, 450, 550);
            report (noiseDrop > 12.0 && std::abs (toneChange) < 1.0,
                    "denoiser (learned): hiss down " + juce::String (noiseDrop, 1) + " dB, tone changed " + juce::String (toneChange, 2) + " dB, latency " + juce::String (d->getLatencySamples()));

            // Adaptive mode on hiss alone: settles and cuts it without being told
            auto a = make ("amethyst.denoise");
            a->setParam ("amount", 20);
            juce::AudioBuffer<float> longHiss (2, 48000 * 4);
            longHiss.clear();
            addHiss (longHiss);
            auto aout = run (*a, longHiss);
            const double drop = bandDb (longHiss, 2000, 12000) - bandDb (aout, 2000, 12000);
            report (drop > 8.0, "denoiser (adaptive): hiss down " + juce::String (drop, 1) + " dB after 4 s");
        }

        // Stereo Imager: width 0 makes it mono; mono bass removes the low side signal only
        {
            auto im = make ("amethyst.imager");
            im->setParam ("width", 0);
            juce::AudioBuffer<float> in (2, 48000);
            for (int i = 0; i < in.getNumSamples(); ++i) { in.setSample (0, i, 0.3f * std::sin (0.05f * i)); in.setSample (1, i, 0.3f * std::sin (0.071f * i)); }
            auto out = run (*im, in);
            float diff = 0.0f;
            for (int i = 0; i < out.getNumSamples(); ++i) diff = juce::jmax (diff, std::abs (out.getSample (0, i) - out.getSample (1, i)));
            report (diff < 1.0e-4f, "stereo imager: width 0 is mono (max L-R " + juce::String (diff, 6) + ")");
        }

        // Rhythm Gate: "Straight 16ths" at depth 100% silences every other step
        {
            auto g = make ("amethyst.gate");
            g->setParam ("smooth", 0.5f);
            auto out = run (*g, sine (1000.0, 0.3f, 1.0));
            // at 120 BPM (no playhead) a 16th is 6000 samples: step 0 on, step 1 off
            const float on = out.getRMSLevel (0, 1000, 4000), off = out.getRMSLevel (0, 7000, 4000);
            report (on > 0.15f && off < 0.01f, "rhythm gate: on step " + juce::String (on, 3) + ", off step " + juce::String (off, 4));
        }

        // Transient Shaper: +attack makes a plucked tone's first 5 ms louder relative to its tail
        {
            auto t = make ("amethyst.transient");
            t->setParam ("attack", 100);
            juce::AudioBuffer<float> in (2, 48000);
            for (int i = 0; i < in.getNumSamples(); ++i)
            {
                const float env = (i % 12000) < 12000 ? std::exp (-(float) (i % 12000) / 2400.0f) : 0.0f;
                const float v = 0.4f * env * std::sin (0.08f * i);
                in.setSample (0, i, v); in.setSample (1, i, v);
            }
            auto out = run (*t, in);
            const float before = in.getRMSLevel (0, 12000, 240) / in.getRMSLevel (0, 18000, 2400);
            const float after = out.getRMSLevel (0, 12000, 240) / out.getRMSLevel (0, 18000, 2400);
            report (after > before * 1.4f, "transient shaper: attack/tail ratio x" + juce::String (after / before, 2));
        }
    }
}

namespace
{
    std::shared_ptr<AudioClip> makeClip (juce::AudioBuffer<float> audio, juce::int64 start)
    {
        auto c = std::make_shared<AudioClip>();
        auto b = std::make_shared<juce::AudioBuffer<float>> (std::move (audio));
        c->source = b;
        c->buffer = b;
        c->startSample = start;
        c->name = "test";
        return c;
    }

    juce::AudioBuffer<float> impulse (int length, int at, float amp)
    {
        juce::AudioBuffer<float> b (2, length);
        b.clear();
        b.setSample (0, at, amp);
        b.setSample (1, at, amp);
        return b;
    }

    std::unique_ptr<juce::AudioPluginInstance> makeIn (AudioEngine& eng, const char* id)
    {
        for (auto& d : Builtins::getDescriptions())
            if (d.fileOrIdentifier == id)
            {
                juce::String err;
                return eng.createPlugin (d, err);
            }
        return nullptr;
    }

    // Runs the engine for `blocks` callbacks of `bs` samples, optionally with a mono input signal.
    juce::AudioBuffer<float> runEngine (AudioEngine& eng, int blocks, int bs = 4096, const float* input = nullptr)
    {
        juce::AudioBuffer<float> out (2, blocks * bs);
        juce::AudioIODeviceCallbackContext ctx {};
        for (int b = 0; b < blocks; ++b)
        {
            float* outs[2] = { out.getWritePointer (0, b * bs), out.getWritePointer (1, b * bs) };
            const float* ins[1] = { input != nullptr ? input + b * bs : nullptr };
            eng.audioDeviceIOCallbackWithContext (ins, input != nullptr ? 1 : 0, outs, 2, bs, ctx);
        }
        return out;
    }

    void testEngine()
    {
        // Plug-in delay compensation: the same impulse on two tracks, one through a 1024-sample-latency
        // plug-in (Denoiser set to 0 dB). In the export both land on the same sample.
        {
            AudioEngine eng;
            auto* a = eng.addTrack ("A");
            auto* b = eng.addTrack ("B");
            eng.addClip (*a, makeClip (impulse (9600, 2400, 0.3f), 0));
            eng.addClip (*b, makeClip (impulse (9600, 2400, 0.3f), 0));
            auto dn = makeIn (eng, "amethyst.denoise");
            dynamic_cast<AmethystPlugin*> (dn.get())->setParam ("amount", 0.0f);
            eng.insertPlugin (*a, 0, std::move (dn)).reset();
            juce::AudioBuffer<float> out;
            const auto r = eng.renderSong (out, nullptr);
            int peakAt = -1;
            float peak = 0.0f;
            for (int i = 0; i < out.getNumSamples(); ++i)
                if (std::abs (out.getSample (0, i)) > peak) { peak = std::abs (out.getSample (0, i)); peakAt = i; }
            report (r.wasOk() && std::abs (peakAt - 2400) <= 1 && peak > 0.5f,
                    "delay compensation: both impulses at sample " + juce::String (peakAt) + " (expect 2400), combined peak " + juce::String (peak, 2)
                    + ", latency " + juce::String (eng.getMaxChannelLatency()));
        }

        // Loop: an impulse at 1000 with the loop at 0..4800 repeats every 4800 samples, sample-accurately
        {
            AudioEngine eng;
            auto* t = eng.addTrack ("T");
            eng.addClip (*t, makeClip (impulse (2000, 200, 0.5f), 1000));   // clip edges fade in, so not on sample 0
            eng.loopStart = 0;
            eng.loopEnd = 4800;
            eng.loopEnabled = true;
            eng.play();
            auto out = runEngine (eng, 4, 4096);
            eng.stop();
            juce::StringArray hits;
            bool ok = true;
            for (int k = 0; k < 3; ++k)
            {
                const int at = 1200 + 4800 * k;
                const bool hit = std::abs (out.getSample (0, at)) > 0.3f;
                hits.add (juce::String (at) + (hit ? " yes" : " no"));
                ok = ok && hit;
            }
            report (ok, "loop: impulse repeats at " + hits.joinIntoString (", "));
        }

        // Monitor mode mono: a left-only signal comes out of both sides at half level; exports unaffected
        {
            AudioEngine eng;
            auto* t = eng.addTrack ("T");
            juce::AudioBuffer<float> left (2, 48000);
            left.clear();
            for (int i = 0; i < 48000; ++i) left.setSample (0, i, 0.4f * std::sin (0.05f * i));
            eng.addClip (*t, makeClip (left, 0));
            eng.monitorMode = 1;
            eng.play();
            auto out = runEngine (eng, 2);
            eng.stop();
            const float l = out.getMagnitude (0, 0, 8192), r = out.getMagnitude (1, 0, 8192);
            juce::AudioBuffer<float> exported;
            eng.renderSong (exported, nullptr);
            report (std::abs (l - 0.2f) < 0.02f && std::abs (r - 0.2f) < 0.02f && exported.getMagnitude (1, 0, 48000) < 1.0e-4f,
                    "monitor mono: L " + juce::String (l, 3) + " R " + juce::String (r, 3) + ", export right side " + juce::String (exported.getMagnitude (1, 0, 48000), 5));
        }

        // Channel render: pre-fader ignores the fader, post-fader includes it
        {
            AudioEngine eng;
            auto* t = eng.addTrack ("T");
            eng.addClip (*t, makeClip (sine (440.0, 0.5f, 1.0), 0));
            t->volumeDb = -6.0f;
            juce::AudioBuffer<float> pre, post;
            eng.renderChannel (*t, false, pre, nullptr, 0.5);
            eng.renderChannel (*t, true, post, nullptr, 0.5);
            const float a = pre.getMagnitude (0, 4800, 24000), b = post.getMagnitude (0, 4800, 24000);
            report (std::abs (a - 0.5f) < 0.02f && std::abs (b - 0.25f) < 0.02f && std::abs (t->volumeDb.load() + 6.0f) < 0.01f,
                    "channel render: pre-fader " + juce::String (a, 3) + ", post-fader " + juce::String (b, 3) + " (fader -6 dB kept)");
        }

        // Reference A/B: with the reference on, the output is the reference at the same position
        {
            AudioEngine eng;
            auto* t = eng.addTrack ("T");
            eng.addClip (*t, makeClip (sine (440.0, 0.5f, 1.0), 0));
            auto ref = std::make_shared<juce::AudioBuffer<float>> (sine (1000.0, 0.1f, 1.0));
            eng.setReference (ref, -20.0);
            eng.referenceMatch = false;
            eng.referenceOn = true;
            eng.play();
            auto out = runEngine (eng, 2);
            eng.stop();
            float err = 0.0f;
            for (int i = 0; i < 8192; ++i) err = juce::jmax (err, std::abs (out.getSample (0, i) - ref->getSample (0, i)));
            report (err < 1.0e-4f, "reference A/B: output matches the reference (max error " + juce::String (err, 6) + ")");
        }

        // Retrospective record: 1.5 s sung into an armed track while stopped becomes a take afterwards
        {
            AudioEngine eng;
            auto* t = eng.addTrack ("Vox");
            t->armed = true;
            std::vector<float> voice (4096 * 24, 0.0f);
            for (int i = 4096 * 4; i < 4096 * 4 + 72000; ++i) voice[(size_t) i] = 0.3f * std::sin (0.04f * i);
            runEngine (eng, 24, 4096, voice.data());
            const double buffered = eng.getRetroSeconds();
            auto [track, clip] = eng.captureRetro();
            const double secs = clip != nullptr ? clip->getLength() / 48000.0 : 0.0;
            report (track == t && clip != nullptr && std::abs (secs - 1.6) < 0.1,
                    "retro record: " + juce::String (buffered, 1) + " s buffered, captured " + juce::String (secs, 2) + " s take");
            if (clip != nullptr)
                clip->file.deleteFile();   // keep the user's Takes folder clean
        }

        // Metronome: each sound clicks, volume 0 is silent
        {
            juce::StringArray levels;
            bool ok = true;
            for (int sound = 0; sound < 3; ++sound)
            {
                AudioEngine eng;
                eng.addTrack ("T");
                eng.metronomeOn = true;
                eng.metronomeSound = sound;
                eng.play();
                const float lvl = runEngine (eng, 2).getMagnitude (0, 0, 8192);
                levels.add (juce::String (lvl, 2));
                ok = ok && lvl > 0.05f;
            }
            AudioEngine eng;
            eng.addTrack ("T");
            eng.metronomeOn = true;
            eng.metronomeVolume = 0.0f;
            eng.play();
            const float silent = runEngine (eng, 2).getMagnitude (0, 0, 8192);
            report (ok && silent < 1.0e-6f, "metronome: beep / wood / hat peaks " + levels.joinIntoString (" / ") + ", volume 0 -> " + juce::String (silent, 6));
        }

        // Low-latency monitoring: while recording, an armed + monitored track skips plug-ins that add latency,
        // so the singer hears themselves straight away (here: 1024 samples sooner than through the Denoiser)
        for (const bool lowLat : { true, false })
        {
            AudioEngine eng;
            eng.lowLatencyMonitoring = lowLat;
            auto* t = eng.addTrack ("Vox");
            t->armed = true;
            t->monitor = true;
            auto dn = makeIn (eng, "amethyst.denoise");
            dynamic_cast<AmethystPlugin*> (dn.get())->setParam ("amount", 0.0f);
            eng.insertPlugin (*t, 0, std::move (dn)).reset();
            std::vector<float> in (4096 * 4, 0.0f);
            in[1000] = 0.5f;
            const bool started = eng.record (0);
            auto out = runEngine (eng, 4, 4096, in.data());
            eng.stop();
            for (auto& [tr, clip] : eng.consumeNewTakes())
                clip->file.deleteFile();   // keep the user's Takes folder clean
            int at = -1;
            float best = 0.0f;
            for (int i = 0; i < out.getNumSamples(); ++i)
                if (std::abs (out.getSample (0, i)) > best) { best = std::abs (out.getSample (0, i)); at = i; }
            const int expect = lowLat ? 1000 : 2024;
            report (started && std::abs (at - expect) <= 1, juce::String (lowLat ? "low-latency monitoring on" : "low-latency monitoring off")
                    + ": singer hears themselves at sample " + juce::String (at) + " (expect " + juce::String (expect) + ")");
        }

        // Channel low cut and polarity
        {
            AudioEngine eng;
            auto* t = eng.addTrack ("T");
            eng.addClip (*t, makeClip (sine (50.0, 0.5f, 2.0), 0));
            t->lowCutHz = 300.0f;
            eng.play();
            const float cut = runEngine (eng, 12).getMagnitude (0, 4096 * 8, 4096 * 4);
            eng.stop();
            t->lowCutHz = 0.0f;
            t->phaseInvert = true;
            eng.setTransportPosition (0);
            eng.play();
            auto inv = runEngine (eng, 4);
            eng.stop();
            const float s = inv.getSample (0, 4096 * 2 + 100), expect = -0.5f * std::sin (juce::MathConstants<float>::twoPi * 50.0f * (4096 * 2 + 100) / 48000.0f);
            report (juce::Decibels::gainToDecibels (cut / 0.5f) < -24.0f && std::abs (s - expect) < 0.02f,
                    "channel strip: low cut 300 Hz takes 50 Hz down " + juce::String (juce::Decibels::gainToDecibels (cut / 0.5f), 1)
                    + " dB; polarity flip " + juce::String (s, 3) + " vs " + juce::String (expect, 3));
        }
    }
}

namespace
{
    // A drum loop: kick on 1 and 3, snare on 2 and 4, hats on 8ths (plus a bassline-ish hum and noise)
    juce::AudioBuffer<float> drumLoop (double bpm, double seconds, double sr = 48000.0, double swingNoise = 0.0)
    {
        juce::AudioBuffer<float> b (2, (int) (seconds * sr));
        b.clear();
        juce::Random rnd (42);
        const double beat = 60.0 / bpm;
        auto hit = [&] (double t, int kind)
        {
            const int s0 = (int) (t * sr);
            for (int i = 0; i < (int) (sr * 0.25) && s0 + i < b.getNumSamples(); ++i)
            {
                const double x = i / sr;
                float v = 0.0f;
                if (kind == 0) v = 0.9f * (float) (std::exp (-x * 18.0) * std::sin (juce::MathConstants<double>::twoPi * (50.0 + 90.0 * std::exp (-x * 30.0)) * x));
                if (kind == 1) v = 0.5f * (float) std::exp (-x * 25.0) * (rnd.nextFloat() * 2.0f - 1.0f);
                if (kind == 2) v = 0.18f * (float) std::exp (-x * 90.0) * (rnd.nextFloat() * 2.0f - 1.0f);
                for (int c = 0; c < 2; ++c) b.addSample (c, s0 + i, v);
            }
        };
        for (int k = 0; beat * 0.5 * k < seconds - 0.3; ++k)
        {
            const double t = juce::jmax (0.0, beat * 0.5 * k + (swingNoise > 0.0 ? (rnd.nextDouble() - 0.5) * swingNoise : 0.0));
            if (k % 2 == 0) hit (t, (k / 2) % 2 == 0 ? 0 : 1);
            hit (juce::jmax (0.0, t), 2);
        }
        for (int i = 0; i < b.getNumSamples(); ++i)
            for (int c = 0; c < 2; ++c)
                b.addSample (c, i, 0.05f * (float) std::sin (0.0098 * i) + 0.003f * (rnd.nextFloat() - 0.5f));
        return b;
    }

    void testWave3()
    {
        // Tempo finder: exact on whole and in-between tempos, including slow and fast ones, and with timing wobble
        {
            juce::StringArray res;
            bool ok = true;
            for (double bpm : { 75.0, 87.5, 93.0, 120.0, 128.0, 140.0, 150.0, 174.0 })
            {
                auto loop = drumLoop (bpm, 24.0);
                const double got = ClipFx::detectTempo (loop, 0, loop.getNumSamples(), 48000.0);
                res.add (juce::String (bpm, 1) + "->" + juce::String (got, 2));
                // exact, or exactly half / double (genuinely ambiguous patterns; the dialog offers both)
                ok = ok && (std::abs (got - bpm) < 0.06 || std::abs (got - 2.0 * bpm) < 0.12 || std::abs (got - 0.5 * bpm) < 0.03);
            }
            auto human = drumLoop (96.0, 24.0, 48000.0, 0.012);   // +/-6 ms of human timing
            const double gotHuman = ClipFx::detectTempo (human, 0, human.getNumSamples(), 48000.0);
            res.add ("96 (loose)->" + juce::String (gotHuman, 2));
            ok = ok && std::abs (gotHuman - 96.0) < 0.15;
            report (ok, "tempo finder: " + res.joinIntoString (", "));
        }

        // Lossless clips play their first sample untouched; lossy ones get the tiny anti-click fade
        {
            for (const bool lossless : { true, false })
            {
                AudioEngine eng;
                auto* t = eng.addTrack ("T");
                juce::AudioBuffer<float> hitBuf (2, 4800);
                hitBuf.clear();
                for (int i = 0; i < 4800; ++i) for (int c = 0; c < 2; ++c) hitBuf.setSample (c, i, 0.5f * std::exp (-i / 400.0f));
                auto clip = makeClip (hitBuf, 1000);
                clip->lossless = lossless;
                eng.addClip (*t, clip);
                eng.play();
                auto out = runEngine (eng, 1, 4096);
                eng.stop();
                const float first = out.getSample (0, 1000);
                report (lossless ? std::abs (first - 0.5f) < 1.0e-4f : first < 0.05f,
                        juce::String (lossless ? "lossless (WAV) clip" : "lossy (MP3) clip") + ": first sample plays at " + juce::String (first, 4)
                        + (lossless ? " (untouched)" : " (faded in)"));
            }
        }
    }
}

namespace
{
    void testLicense()
    {
        namespace V = LicenseTestVectors;
        using License::State;
        const auto good = License::verify (V::valid, V::modulusHex);
        report (good.valid && good.tier == 2 && good.sub == "test-user" && good.exp == V::iat + 14 * V::day,
                "license: a server-signed license verifies (tier " + juce::String (good.tier) + ", " + good.sub + ")");
        const auto forged = License::verify (V::forged, V::modulusHex);
        const auto flipped = License::verify (V::badSig, V::modulusHex);
        report (! forged.valid && ! flipped.valid, "license: an edited payload (" + forged.error + ") and a 1-bit signature change (" + flipped.error + ") are rejected");
        const auto otherKey = License::verify (V::valid);   // the real public key: test licenses must not pass
        report (! otherKey.valid, "license: a license signed with another key is rejected (" + otherKey.error + ")");
        report (! License::verify ("garbage").valid && ! License::verify ("abc.def").valid, "license: junk tokens are rejected");

        const auto now = V::iat + 2 * V::day;
        auto st = [] (State s) { return juce::String ((int) s); };
        const auto active = License::evaluate (good, now, now, "test-user");
        report (active.state == State::Active && active.tier == 2, "license: active 2 days after issue (offline) -> tier " + juce::String (active.tier));
        const auto expired = License::evaluate (good, V::iat + 15 * V::day, V::iat + 15 * V::day, "test-user");
        report (expired.state == State::Expired && expired.tier == 0, "license: after 14 days offline it asks to go online (state " + st (expired.state) + ")");
        const auto rolled = License::evaluate (good, V::iat + 1 * V::day, V::iat + 5 * V::day, "test-user");
        report (rolled.state == State::ClockRolledBack && rolled.tier == 0, "license: clock set back 4 days is caught (state " + st (rolled.state) + ")");
        const auto beforeIssue = License::evaluate (good, V::iat - 3 * V::day, 0, "test-user");
        report (beforeIssue.state == State::ClockRolledBack, "license: a clock earlier than the server's issue time is caught");
        const auto drift = License::evaluate (good, now - 5 * 60 * 1000, now, "test-user");
        report (drift.state == State::Active, "license: 5 minutes of normal clock drift is fine");
        const auto wrong = License::evaluate (good, now, now, "another-user");
        report (wrong.state == State::WrongAccount && wrong.tier == 0, "license: another account's license grants nothing");
        const auto lapsedInfo = License::verify (V::lapsed, V::modulusHex);
        const auto lapsed = License::evaluate (lapsedInfo, V::iat + 4 * V::day, V::iat + 4 * V::day, "test-user");
        report (lapsedInfo.valid && lapsed.state == State::Lapsed, "license: a subscription that ended (pro_until) stops on time, even offline");
        const auto freeInfo = License::verify (V::free, V::modulusHex);
        report (freeInfo.valid && License::evaluate (freeInfo, now, now, "test-user").tier == 0, "license: a Free license verifies and grants Free");
        report (juce::String (LicensePublicKey::modulusHex).length() == 512, "license: the app has a 2048-bit public key built in");
    }
}

int runSecondWaveSelfTests()
{
    failures = 0;
    testEffects();
    testEngine();
    testWave3();
    testLicense();
    return failures;
}
