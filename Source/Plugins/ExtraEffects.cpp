#include "Builtins.h"
#include "../Loudness.h"

// AMETHYST 2026 effects, second wave:
//   Vocal Rider (Pro), Denoiser (Ultra), Harmonizer (Ultra), Stereo Imager (Pro), Transient Shaper (Pro),
//   Rhythm Gate (Pro), Auto Filter (Pro) and the Loudness Meter (free for everyone).

namespace
{
    inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }
    inline float gainToDb (float g)  { return g > 1.0e-6f ? 20.0f * std::log10 (g) : -120.0f; }
    inline float onePole (double ms, double sr) { return (float) std::exp (-1.0 / juce::jmax (1.0, ms * 0.001 * sr)); }

    // Position for tempo-synced effects: (ppq at block start, bpm, playing).
    struct HostTime { double ppq = 0.0, bpm = 120.0; bool playing = false; };
    HostTime hostTime (juce::AudioProcessor& p)
    {
        HostTime t;
        if (auto* ph = p.getPlayHead())
            if (auto pos = ph->getPosition())
            {
                if (auto b = pos->getBpm()) t.bpm = juce::jmax (20.0, *b);
                if (auto q = pos->getPpqPosition()) t.ppq = *q;
                t.playing = pos->getIsPlaying();
            }
        return t;
    }

    //==========================================================================
    // Small history graph used by several visuals: values pushed by a timer, drawn as a line.
    class HistoryGraph : public juce::Component, private juce::Timer
    {
    public:
        HistoryGraph (std::function<float()> source, float lo, float hi, juce::String unitLabel, std::vector<float> guides = {})
            : read (std::move (source)), minV (lo), maxV (hi), unit (std::move (unitLabel)), lines (std::move (guides))
        {
            hist.assign (240, lo);
            startTimerHz (30);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (juce::Colours::black.withAlpha (0.25f));
            g.fillRoundedRectangle (r, 8.0f);
            auto yOf = [&] (float v) { return r.getBottom() - (juce::jlimit (minV, maxV, v) - minV) / (maxV - minV) * r.getHeight(); };
            g.setFont (juce::FontOptions (10.0f));
            for (auto v : lines)
            {
                g.setColour (Theme::grid.withAlpha (0.6f));
                g.drawHorizontalLine ((int) yOf (v), r.getX(), r.getRight());
                g.setColour (Theme::textDim);
                g.drawText (juce::String (v, 0) + unit, juce::Rectangle<float> (r.getX() + 4, yOf (v) - 12, 60, 12), juce::Justification::left);
            }
            juce::Path p;
            for (size_t i = 0; i < hist.size(); ++i)
            {
                const float x = r.getX() + r.getWidth() * (float) i / (float) (hist.size() - 1);
                if (i == 0) p.startNewSubPath (x, yOf (hist[i])); else p.lineTo (x, yOf (hist[i]));
            }
            g.setColour (Theme::accent);
            g.strokePath (p, juce::PathStrokeType (1.8f));
        }

    private:
        void timerCallback() override
        {
            hist.erase (hist.begin());
            hist.push_back (read());
            repaint();
        }
        std::function<float()> read;
        float minV, maxV;
        juce::String unit;
        std::vector<float> lines;
        std::vector<float> hist;
    };

    //==========================================================================
    // VOCAL RIDER: rides the vocal level towards a target like an engineer on the fader, before any compressor.
    class RiderPlugin : public AmethystPlugin
    {
    public:
        RiderPlugin() : AmethystPlugin ("amethyst.rider", "Vocal Rider", "Dynamics", "Automatic vocal level riding", makeLayout())
        {
            sections = { { "Ride", { "target", "range", "speed" }, 0, 62 }, { "Detect", { "sens", "out" }, 0, 54 } };
            presets = {
                { "Default", {} },
                { "Gentle Leveling", { { "range", 4 }, { "speed", 600 } } },
                { "Pop Vocal", { { "target", -16 }, { "range", 8 }, { "speed", 250 } } },
                { "Rap Upfront", { { "target", -14 }, { "range", 10 }, { "speed", 120 } } },
                { "Podcast Voice", { { "target", -20 }, { "range", 12 }, { "speed", 400 }, { "sens", -45 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::num ("target", "Target", -36.0f, -6.0f, -18.0f, "dB", 1));
            l.add (AmParams::num ("range", "Range", 1.0f, 18.0f, 6.0f, "dB", 1));
            l.add (AmParams::num ("speed", "Speed", 30.0f, 2000.0f, 300.0f, "ms", 0, 300.0f));
            l.add (AmParams::num ("sens", "Ignore below", -70.0f, -20.0f, -50.0f, "dB", 0));
            l.add (AmParams::num ("out", "Output", -12.0f, 12.0f, 0.0f, "dB", 1));
            return l;
        }

        void prepareToPlay (double sr, int) override { sampleRate = sr; env = 0.0f; gainDb = 0.0f; }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals nd;
            const int n = buffer.getNumSamples(), nCh = juce::jmin (2, buffer.getNumChannels());
            const float target = get ("target"), range = get ("range"), sens = get ("sens"), out = get ("out");
            const float rmsC = onePole (40.0, sampleRate), rideC = onePole (get ("speed"), sampleRate);
            for (int i = 0; i < n; ++i)
            {
                float x2 = 0.0f;
                for (int c = 0; c < nCh; ++c) { const float v = buffer.getSample (c, i); x2 += v * v; }
                env = rmsC * env + (1.0f - rmsC) * (x2 / (float) nCh);
                const float level = 10.0f * std::log10 (env + 1.0e-12f);
                if (level > sens)   // between phrases the rider holds still instead of pumping the noise up
                {
                    const float want = juce::jlimit (-range, range, target - level);
                    gainDb = rideC * gainDb + (1.0f - rideC) * want;
                }
                const float g = dbToGain (gainDb + out);
                for (int c = 0; c < nCh; ++c)
                    buffer.setSample (c, i, buffer.getSample (c, i) * g);
            }
            rideNow.store (gainDb);
        }

        std::unique_ptr<juce::Component> createVisual() override
        {
            return std::make_unique<HistoryGraph> ([this] { return rideNow.load(); }, -18.0f, 18.0f, " dB", std::vector<float> { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f });
        }
        int getVisualHeight() const override { return 120; }

    private:
        float env = 0.0f, gainDb = 0.0f;
        std::atomic<float> rideNow { 0.0f };
    };

    //==========================================================================
    // DENOISER (Ultra): STFT spectral noise reduction. Learns the noise from a pause (Learn switch), or keeps
    // following the noise floor on its own (Adaptive). 1024-point frames at 75% overlap; reports its latency.
    class DenoisePlugin : public AmethystPlugin
    {
    public:
        static constexpr int kOrder = 10, kN = 1 << kOrder, kHop = kN / 4, kBins = kN / 2 + 1;

        DenoisePlugin() : AmethystPlugin ("amethyst.denoise", "Denoiser", "Repair", "Removes hiss, hum and room noise", makeLayout()), fft (kOrder)
        {
            sections = { { "Noise", { "learn", "adaptive" }, 0, 54 }, { "Reduction", { "amount", "sens", "smooth", "tone" }, 0, 58 } };
            presets = {
                { "Default", {} },
                { "Light Hiss", { { "amount", 9 }, { "sens", 3 } } },
                { "Room Tone", { { "amount", 16 }, { "sens", 6 }, { "smooth", 60 } } },
                { "Heavy Cleanup", { { "amount", 28 }, { "sens", 9 }, { "smooth", 70 } } },
                { "Keep Air", { { "amount", 14 }, { "tone", -60 } } },
            };
            for (int i = 0; i < kN; ++i)
                window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) kN));
            setLatencySamples (kN);
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::toggle ("learn", "Learn noise", false));
            l.add (AmParams::toggle ("adaptive", "Adaptive", true));
            l.add (AmParams::num ("amount", "Reduction", 0.0f, 40.0f, 12.0f, "dB", 0));
            l.add (AmParams::num ("sens", "Threshold", -6.0f, 18.0f, 4.0f, "dB", 1));
            l.add (AmParams::num ("smooth", "Smoothing", 0.0f, 100.0f, 45.0f, "%", 0));
            l.add (AmParams::num ("tone", "Tilt", -100.0f, 100.0f, 0.0f, "%", 0));   // + = clean highs harder, - = lows
            return l;
        }

        void prepareToPlay (double sr, int) override
        {
            sampleRate = sr;
            for (auto& ch : chans) ch = {};
            if (! hasProfile)
                profile.fill (0.0f);
            learnFrames = 0;
            setLatencySamples (kN);
        }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals nd;
            const int n = buffer.getNumSamples(), nCh = juce::jmin (2, buffer.getNumChannels());
            const bool learn = getBool ("learn");
            if (learn && ! wasLearning) { profile.fill (0.0f); learnFrames = 0; }
            if (! learn && wasLearning && learnFrames > 0) hasProfile = true;
            wasLearning = learn;

            for (int c = 0; c < nCh; ++c)
            {
                auto& st = chans[(size_t) c];
                float* d = buffer.getWritePointer (c);
                for (int i = 0; i < n; ++i)
                {
                    st.in[(size_t) st.fill] = d[i];
                    d[i] = st.out[(size_t) st.fill];
                    st.out[(size_t) st.fill] = 0.0f;
                    if (++st.fill == kHop)
                    {
                        st.fill = 0;
                        frame (st, c, learn);
                    }
                }
            }
        }

        // the learned profile is saved with the preset / project
        void getStateInformation (juce::MemoryBlock& mb) override
        {
            if (hasProfile)
            {
                juce::MemoryBlock p (profile.data(), sizeof (float) * profile.size());
                apvts.state.setProperty ("noiseProfile", p.toBase64Encoding(), nullptr);
            }
            AmethystPlugin::getStateInformation (mb);
        }
        void setStateInformation (const void* data, int size) override
        {
            AmethystPlugin::setStateInformation (data, size);
            juce::MemoryBlock p;
            if (p.fromBase64Encoding (apvts.state.getProperty ("noiseProfile").toString()) && p.getSize() == sizeof (float) * profile.size())
            {
                std::memcpy (profile.data(), p.getData(), p.getSize());
                hasProfile = true;
            }
        }

    private:
        struct Channel
        {
            std::array<float, kN> ring {};         // last kN input samples (circular by hops)
            std::array<float, kHop> in {}, out {};
            std::array<float, kN> acc {};          // overlap-add accumulator
            std::array<float, kBins> gains {}, adaptiveFloor {}, smoothed {};
            int fill = 0, ringPos = 0;
            bool floorInit = false;
        };

        void frame (Channel& st, int channel, bool learn)
        {
            // shift the new hop into the analysis ring
            std::copy (st.in.begin(), st.in.end(), st.ring.begin() + st.ringPos * kHop);
            st.ringPos = (st.ringPos + 1) % (kN / kHop);
            for (int i = 0; i < kN; ++i)
                work[(size_t) i] = st.ring[(size_t) ((st.ringPos * kHop + i) % kN)] * window[(size_t) i];
            std::fill (work.begin() + kN, work.end(), 0.0f);
            fft.performRealOnlyForwardTransform (work.data(), true);

            const float amount = dbToGain (-get ("amount"));
            const float thr = dbToGain (get ("sens"));
            const float smooth = juce::jlimit (0.0f, 0.97f, get ("smooth") * 0.0095f);
            const float tilt = get ("tone") * 0.01f;
            const bool adaptive = getBool ("adaptive") || ! hasProfile;

            for (int k = 0; k < kBins; ++k)
            {
                const float re = work[(size_t) (2 * k)], im = work[(size_t) (2 * k + 1)];
                const float mag = std::sqrt (re * re + im * im);
                if (learn && channel == 0)
                    profile[(size_t) k] = (profile[(size_t) k] * (float) learnFrames + mag) / (float) (learnFrames + 1);

                // minimum statistics on a smoothed spectrum: follows the quietest level of each bin,
                // drops quickly, creeps up about 1.5 dB per second, then corrects the minimum's bias
                float& ps = st.smoothed[(size_t) k];
                float& fl = st.adaptiveFloor[(size_t) k];
                if (! st.floorInit) { ps = mag; fl = mag; }
                ps = 0.85f * ps + 0.15f * mag;
                fl = ps < fl ? 0.6f * fl + 0.4f * ps : fl * 1.0009f;
                const float adaptiveNoise = fl * 1.9f;

                const float noise = adaptive ? (hasProfile ? juce::jmax (adaptiveNoise, profile[(size_t) k]) : adaptiveNoise) : profile[(size_t) k];
                const float pos = (float) k / (float) kBins;
                const float t = thr * (1.0f + tilt * (pos - 0.3f));
                const float nn = noise * t;
                float g = mag > 1.0e-9f ? juce::jlimit (0.0f, 1.0f, (mag * mag - nn * nn) / (mag * mag)) : 0.0f;
                g = amount + (1.0f - amount) * g;   // never below the reduction floor (keeps it natural)
                float& sg = st.gains[(size_t) k];
                sg = g > sg ? 0.4f * sg + 0.6f * g : smooth * sg + (1.0f - smooth) * g;   // fast open, smooth close
            }
            st.floorInit = true;
            if (learn && channel == 0) ++learnFrames;

            // light smoothing across frequency against "musical noise"
            for (int k = 0; k < kBins; ++k)
            {
                const float a = st.gains[(size_t) juce::jmax (0, k - 1)], b = st.gains[(size_t) k], c = st.gains[(size_t) juce::jmin (kBins - 1, k + 1)];
                const float g = 0.25f * a + 0.5f * b + 0.25f * c;
                work[(size_t) (2 * k)] *= g;
                work[(size_t) (2 * k + 1)] *= g;
            }

            fft.performRealOnlyInverseTransform (work.data());
            constexpr float olaScale = 0.5f;   // sqrt-Hann analysis * synthesis = Hann, which sums to 2 at 75% overlap
            for (int i = 0; i < kN; ++i)
                st.acc[(size_t) i] += work[(size_t) i] * window[(size_t) i] * olaScale;
            for (int i = 0; i < kHop; ++i)
                st.out[(size_t) i] = st.acc[(size_t) i];
            std::copy (st.acc.begin() + kHop, st.acc.end(), st.acc.begin());
            std::fill (st.acc.end() - kHop, st.acc.end(), 0.0f);
        }

        juce::dsp::FFT fft;
        std::array<float, kN> window {};
        std::array<float, 2 * kN> work {};
        std::array<Channel, 2> chans;
        std::array<float, kBins> profile {};
        int learnFrames = 0;
        bool hasProfile = false, wasLearning = false;
    };

    //==========================================================================
    // Pitch shifting voice: two crossfading read heads on a short delay line (clean for harmonies).
    class ShiftVoice
    {
    public:
        void prepare (double sr)
        {
            size = 1 << 14;
            buf.assign ((size_t) size, 0.0f);
            w = 0;
            window = juce::jmax (256.0, sr * 0.045);
            phase = 0.0;
        }
        float process (float x, double ratio)
        {
            buf[(size_t) w] = x;
            phase += (1.0 - ratio) / window;
            phase -= std::floor (phase);
            float y = 0.0f;
            for (int k = 0; k < 2; ++k)
            {
                double ph = phase + 0.5 * k;
                ph -= std::floor (ph);
                const double delay = 8.0 + ph * window;
                double rp = (double) w - delay;
                while (rp < 0.0) rp += size;
                const int i0 = (int) rp;
                const float f = (float) (rp - i0);
                const float s = buf[(size_t) (i0 & (size - 1))] * (1.0f - f) + buf[(size_t) ((i0 + 1) & (size - 1))] * f;
                const float gw = (float) std::sin (juce::MathConstants<double>::pi * ph);
                y += s * gw * gw;
            }
            w = (w + 1) & (size - 1);
            return y;
        }
    private:
        std::vector<float> buf;
        int size = 0, w = 0;
        double window = 2048.0, phase = 0.0;
    };

    //==========================================================================
    // HARMONIZER (Ultra): up to two extra voices that follow the singer in key (scale degrees),
    // or fixed intervals in semitones. Pitch detection by normalised autocorrelation every ~20 ms.
    class HarmonyPlugin : public AmethystPlugin
    {
    public:
        HarmonyPlugin() : AmethystPlugin ("amethyst.harmony", "Harmonizer", "Pitch", "Instant in-key harmonies", makeLayout())
        {
            sections = { { "Key", { "key", "scale", "mode" }, 0, 54 },
                         { "Voices", { "v1", "v2", "level", "spread" }, 1, 58 },
                         { "Feel", { "human", "dry" }, 1, 54 } };
            presets = {
                { "Default", {} },
                { "Third Above", { { "v1", 7 }, { "v2", 0 } } },
                { "Pop Stack (3rd + 5th)", { { "v1", 7 }, { "v2", 9 } } },
                { "Octave Sandwich", { { "v1", 1 }, { "v2", 11 }, { "level", 70 } } },
                { "Dark Fifth Below", { { "v1", 3 }, { "v2", 0 }, { "level", 75 } } },
                { "Wide Choir", { { "v1", 5 }, { "v2", 7 }, { "spread", 100 }, { "human", 70 } } },
            };
        }

        static juce::StringArray intervals()
        {
            return { "Off", "Octave down", "6th down", "5th down", "4th down", "3rd down", "Double (unison)",
                     "3rd up", "4th up", "5th up", "6th up", "Octave up" };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::choice ("key", "Key", { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0));
            l.add (AmParams::choice ("scale", "Scale", { "Major", "Minor" }, 0));
            l.add (AmParams::choice ("mode", "Follow", { "Scale", "Fixed" }, 0));
            l.add (AmParams::choice ("v1", "Voice 1", intervals(), 7));
            l.add (AmParams::choice ("v2", "Voice 2", intervals(), 0));
            l.add (AmParams::num ("level", "Voices", 0.0f, 100.0f, 60.0f, "%", 0));
            l.add (AmParams::num ("spread", "Spread", 0.0f, 100.0f, 60.0f, "%", 0));
            l.add (AmParams::num ("human", "Humanize", 0.0f, 100.0f, 35.0f, "%", 0));
            l.add (AmParams::num ("dry", "Lead", 0.0f, 100.0f, 100.0f, "%", 0));
            return l;
        }

        void prepareToPlay (double sr, int) override
        {
            sampleRate = sr;
            for (auto& v : voices) v.prepare (sr);
            decim = juce::jmax (1, (int) std::lround (sr / 12000.0));
            fsd = sr / decim;
            ana.assign (1024, 0.0f);
            anaPos = 0;
            decimCount = 0;
            hopCount = 0;
            note = -1.0f;
            for (auto& r : ratio) r = 1.0;
            for (auto& r : targetRatio) r = 1.0;
            lfo = 0.0;
            for (auto& d : humanDelay) d.assign (4096, 0.0f);
            hdPos = 0;
        }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals nd;
            const int n = buffer.getNumSamples(), nCh = juce::jmin (2, buffer.getNumChannels());
            const float level = get ("level") * 0.01f, dry = get ("dry") * 0.01f;
            const float spread = get ("spread") * 0.01f, human = get ("human") * 0.01f;
            const int v[2] = { getInt ("v1"), getInt ("v2") };
            const float glide = onePole (25.0, sampleRate);

            for (int i = 0; i < n; ++i)
            {
                const float l = buffer.getSample (0, i), r = nCh > 1 ? buffer.getSample (1, i) : l;
                const float mono = 0.5f * (l + r);
                analyse (mono, v);

                lfo += 2.0 * juce::MathConstants<double>::pi * 0.23 / sampleRate;
                float outL = l * dry, outR = r * dry;
                for (int k = 0; k < 2; ++k)
                {
                    if (v[k] == 0) continue;
                    ratio[k] = glide * ratio[k] + (1.0 - glide) * targetRatio[k];
                    // humanize: a few cents of slow drift and 10-25 ms of lag per voice
                    const double drift = std::pow (2.0, human * 0.12 * std::sin (lfo * (1.0 + 0.37 * k) + k) / 12.0);
                    float s = voices[(size_t) k].process (mono, ratio[k] * drift);
                    auto& hd = humanDelay[(size_t) k];
                    hd[(size_t) hdPos] = s;
                    const int lag = (int) (human * (0.010 + 0.015 * k) * sampleRate);
                    s = hd[(size_t) ((hdPos - lag + 4096) & 4095)];
                    const float pan = (k == 0 ? -1.0f : 1.0f) * spread * (v[1] == 0 || v[0] == 0 ? 0.0f : 1.0f);
                    const float a = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
                    outL += s * level * std::cos (a) * 1.2f;
                    outR += s * level * std::sin (a) * 1.2f;
                }
                hdPos = (hdPos + 1) & 4095;
                buffer.setSample (0, i, outL);
                if (nCh > 1) buffer.setSample (1, i, outR);
            }
        }

    private:
        void analyse (float x, const int* v)
        {
            if (++decimCount < decim) return;
            decimCount = 0;
            ana[(size_t) anaPos] = x;
            anaPos = (anaPos + 1) % (int) ana.size();
            if (++hopCount < 256) return;
            hopCount = 0;

            // normalised autocorrelation over 70..1000 Hz
            const int N = (int) ana.size();
            const int minLag = (int) (fsd / 1000.0), maxLag = juce::jmin (N / 2, (int) (fsd / 70.0));
            float energy = 0.0f;
            for (int i = 0; i < N; ++i) { const float s = ana[(size_t) ((anaPos + i) % N)]; energy += s * s; }
            if (energy < 1.0e-4f) return;   // silence: keep the last intervals
            float best = 0.0f;
            int bestLag = -1;
            for (int lag = minLag; lag <= maxLag; ++lag)
            {
                float acc = 0.0f, e1 = 0.0f, e2 = 0.0f;
                for (int i = 0; i + lag < N; i += 2)
                {
                    const float a = ana[(size_t) ((anaPos + i) % N)], b = ana[(size_t) ((anaPos + i + lag) % N)];
                    acc += a * b; e1 += a * a; e2 += b * b;
                }
                const float c = acc / std::sqrt (e1 * e2 + 1.0e-12f);
                if (c > best) { best = c; bestLag = lag; }
            }
            if (bestLag <= 0 || best < 0.75f) return;   // unvoiced: hold
            const double f = fsd / bestLag;
            note = (float) (69.0 + 12.0 * std::log2 (f / 440.0));
            for (int k = 0; k < 2; ++k)
                targetRatio[k] = std::pow (2.0, semitonesFor (v[k], note) / 12.0);
        }

        double semitonesFor (int interval, float sungNote) const
        {
            static const int degrees[] = { 0, -7, -5, -4, -3, -2, 0, 2, 3, 4, 5, 7 };
            static const int fixedSemis[] = { 0, -12, -9, -7, -5, -4, 0, 4, 5, 7, 9, 12 };
            if (interval <= 0 || interval > 11) return 0.0;
            if (getInt ("mode") == 1) return fixedSemis[interval];
            static const int major[7] = { 0, 2, 4, 5, 7, 9, 11 }, minor[7] = { 0, 2, 3, 5, 7, 8, 10 };
            const int* sc = getInt ("scale") == 1 ? minor : major;
            const int key = getInt ("key");
            const int sung = (int) std::lround (sungNote);
            // nearest scale degree to the sung note
            int bestDeg = 0, bestOct = 0, bestDist = 99;
            for (int oct = -1; oct <= 11; ++oct)
                for (int d = 0; d < 7; ++d)
                {
                    const int nn = key + sc[d] + 12 * oct;
                    if (std::abs (nn - sung) < bestDist) { bestDist = std::abs (nn - sung); bestDeg = d; bestOct = oct; }
                }
            const int target = bestDeg + degrees[interval];
            const int oct = bestOct + (int) std::floor (target / 7.0);
            const int deg = ((target % 7) + 7) % 7;
            const int targetNote = key + sc[deg] + 12 * oct;
            return (double) (targetNote - (key + sc[bestDeg] + 12 * bestOct));
        }

        std::array<ShiftVoice, 2> voices;
        std::array<double, 2> ratio {}, targetRatio {};
        std::array<std::vector<float>, 2> humanDelay;
        int hdPos = 0;
        std::vector<float> ana;
        int anaPos = 0, decim = 4, decimCount = 0, hopCount = 0;
        double fsd = 12000.0, lfo = 0.0;
        float note = -1.0f;
    };

    //==========================================================================
    // STEREO IMAGER: width, mono low end below a frequency, balance; correlation + vectorscope display.
    class ImagerPlugin : public AmethystPlugin
    {
    public:
        ImagerPlugin() : AmethystPlugin ("amethyst.imager", "Stereo Imager", "Stereo", "Width, mono bass and balance", makeLayout())
        {
            sections = { { "Image", { "width", "monobass", "monofreq", "balance" }, 0, 60 } };
            presets = {
                { "Default", {} },
                { "Wide Hook", { { "width", 150 } } },
                { "Mono Bass 120", { { "monobass", 1 }, { "monofreq", 120 } } },
                { "Master Safe", { { "width", 115 }, { "monobass", 1 }, { "monofreq", 100 } } },
                { "Mono Check", { { "width", 0 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::num ("width", "Width", 0.0f, 200.0f, 100.0f, "%", 0));
            l.add (AmParams::toggle ("monobass", "Mono bass", false));
            l.add (AmParams::num ("monofreq", "Below", 40.0f, 400.0f, 120.0f, "Hz", 0, 120.0f));
            l.add (AmParams::num ("balance", "Balance", -100.0f, 100.0f, 0.0f, {}, 0));
            return l;
        }

        void prepareToPlay (double sr, int) override { sampleRate = sr; sideHp.reset(); lastF = -1.0f; }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals nd;
            if (buffer.getNumChannels() < 2) return;
            const int n = buffer.getNumSamples();
            const float width = get ("width") * 0.01f, bal = get ("balance") * 0.01f;
            const bool mono = getBool ("monobass");
            const float f = get ("monofreq");
            if (f != lastF) { lastF = f; sideHp.setCoefficients (juce::IIRCoefficients::makeHighPass (sampleRate, f, 0.707)); }
            const float gl = juce::jmin (1.0f, 1.0f - bal), gr = juce::jmin (1.0f, 1.0f + bal);
            float* L = buffer.getWritePointer (0);
            float* R = buffer.getWritePointer (1);
            double lr = 0, ll = 0, rr = 0;
            for (int i = 0; i < n; ++i)
            {
                const float m = 0.5f * (L[i] + R[i]);
                float s = 0.5f * (L[i] - R[i]) * width;
                if (mono) s = sideHp.processSingleSampleRaw (s);
                L[i] = (m + s) * gl;
                R[i] = (m - s) * gr;
                lr += L[i] * R[i]; ll += L[i] * L[i]; rr += R[i] * R[i];
                if ((i & 7) == 0)
                {
                    const int k = scopePos.load();
                    scopeX[(size_t) k].store ((L[i] - R[i]) * 0.7071f);
                    scopeY[(size_t) k].store ((L[i] + R[i]) * 0.7071f);
                    scopePos.store ((k + 1) % (int) scopeX.size());
                }
            }
            if (ll > 1.0e-9 && rr > 1.0e-9)
                correlation.store ((float) (0.8 * correlation.load() + 0.2 * (lr / std::sqrt (ll * rr))));
        }

        std::unique_ptr<juce::Component> createVisual() override;
        int getVisualHeight() const override { return 150; }

        std::array<std::atomic<float>, 384> scopeX {}, scopeY {};
        std::atomic<int> scopePos { 0 };
        std::atomic<float> correlation { 1.0f };

    private:
        juce::IIRFilter sideHp;
        float lastF = -1.0f;
    };

    class ImagerVisual : public juce::Component, private juce::Timer
    {
    public:
        explicit ImagerVisual (ImagerPlugin& p) : plugin (p) { startTimerHz (30); }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (2.0f);
            g.setColour (juce::Colours::black.withAlpha (0.25f));
            g.fillRoundedRectangle (r, 8.0f);
            auto scope = r.removeFromLeft (r.getHeight()).reduced (8.0f);
            g.setColour (Theme::grid);
            g.drawLine (scope.getCentreX(), scope.getY(), scope.getCentreX(), scope.getBottom());
            g.drawLine (scope.getX(), scope.getCentreY(), scope.getRight(), scope.getCentreY());
            g.setColour (Theme::accent.withAlpha (0.8f));
            for (size_t i = 0; i < plugin.scopeX.size(); ++i)
            {
                const float x = scope.getCentreX() + juce::jlimit (-1.0f, 1.0f, plugin.scopeX[i].load()) * scope.getWidth() * 0.5f;
                const float y = scope.getCentreY() - juce::jlimit (-1.0f, 1.0f, plugin.scopeY[i].load()) * scope.getHeight() * 0.5f;
                g.fillRect (x, y, 1.6f, 1.6f);
            }
            // correlation bar: -1 (out of phase) .. +1 (mono)
            auto bar = r.reduced (16.0f, 0.0f).withSizeKeepingCentre (r.getWidth() - 32.0f, 14.0f);
            g.setColour (Theme::panelLight);
            g.fillRoundedRectangle (bar, 7.0f);
            const float c = plugin.correlation.load();
            const float cx = bar.getX() + (c + 1.0f) * 0.5f * bar.getWidth();
            g.setColour (c < 0.0f ? Theme::record : Theme::accent);
            g.fillEllipse (cx - 7.0f, bar.getY(), 14.0f, 14.0f);
            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("-1", bar.translated (0, 16).withWidth (30), juce::Justification::left);
            g.drawText ("+1", bar.translated (0, 16).withTrimmedLeft (bar.getWidth() - 30), juce::Justification::right);
            g.drawText ("Correlation " + juce::String (c, 2), bar.translated (0, -22), juce::Justification::centred);
        }
    private:
        void timerCallback() override { repaint(); }
        ImagerPlugin& plugin;
    };
    std::unique_ptr<juce::Component> ImagerPlugin::createVisual() { return std::make_unique<ImagerVisual> (*this); }

    //==========================================================================
    // TRANSIENT SHAPER: more or less attack and sustain, independent of level.
    class TransientPlugin : public AmethystPlugin
    {
    public:
        TransientPlugin() : AmethystPlugin ("amethyst.transient", "Transient Shaper", "Dynamics", "Punch and sustain control", makeLayout())
        {
            sections = { { "Shape", { "attack", "sustain", "out" }, 0, 62 } };
            presets = {
                { "Default", {} },
                { "Punchier Consonants", { { "attack", 40 } } },
                { "Tame Plosives", { { "attack", -45 } } },
                { "Dry Up the Room", { { "sustain", -50 } } },
                { "Fatter Tail", { { "sustain", 45 } } },
            };
        }
        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::num ("attack", "Attack", -100.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::num ("sustain", "Sustain", -100.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::num ("out", "Output", -12.0f, 12.0f, 0.0f, "dB", 1));
            return l;
        }
        void prepareToPlay (double sr, int) override { sampleRate = sr; fA = sA = fR = sR = 0.0f; }
        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals nd;
            const int n = buffer.getNumSamples(), nCh = juce::jmin (2, buffer.getNumChannels());
            const float att = get ("attack") * 0.01f, sus = get ("sustain") * 0.01f, out = get ("out");
            const float fastA = onePole (0.5, sampleRate), slowA = onePole (25.0, sampleRate);
            const float rel = onePole (60.0, sampleRate), fastR = onePole (20.0, sampleRate), slowR = onePole (250.0, sampleRate);
            for (int i = 0; i < n; ++i)
            {
                float x = 0.0f;
                for (int c = 0; c < nCh; ++c) x = juce::jmax (x, std::abs (buffer.getSample (c, i)));
                auto follow = [x] (float& e, float a, float r) { e = x > e ? a * e + (1 - a) * x : r * e + (1 - r) * x; };
                follow (fA, fastA, rel);
                follow (sA, slowA, rel);
                follow (fR, fastA, fastR);
                follow (sR, fastA, slowR);
                const float tr = juce::jmax (0.0f, gainToDb (fA + 1e-9f) - gainToDb (sA + 1e-9f));   // > 0 on hits
                const float tail = juce::jmax (0.0f, gainToDb (sR + 1e-9f) - gainToDb (fR + 1e-9f)); // > 0 in decays
                const float gDb = juce::jlimit (-18.0f, 18.0f, att * tr * 1.5f + sus * tail * 1.2f) + out;
                const float g = dbToGain (gDb);
                for (int c = 0; c < nCh; ++c)
                    buffer.setSample (c, i, softCeiling (buffer.getSample (c, i) * g, 1.4f));
            }
        }
    private:
        float fA = 0, sA = 0, fR = 0, sR = 0;
    };

    //==========================================================================
    // RHYTHM GATE: tempo-synced 16-step volume patterns (trance gate / stutter), with smoothing and mix.
    struct GatePattern { const char* name; const char* steps; };
    const GatePattern kPatterns[] = {
        { "Straight 16ths", "x.x.x.x.x.x.x.x." }, { "Offbeat pump", ".xxx.xxx.xxx.xxx" }, { "Trance 1", "xx.xx.x.xx.xx.x." },
        { "Trance 2", "x.xxx.x.x.xxx.x." }, { "Stutter end", "xxxxxxxxxxxxx.x." }, { "Half time", "xxxx....xxxx...." },
        { "Triplet feel", "x..x..x..x..x..x" }, { "Chop", "xx..xx..x.x.xx.." },
    };
    juce::StringArray patternNames() { juce::StringArray s; for (auto& p : kPatterns) s.add (p.name); return s; }

    class GatePlugin : public AmethystPlugin
    {
    public:
        GatePlugin() : AmethystPlugin ("amethyst.gate", "Rhythm Gate", "Creative", "Tempo-synced trance gate and stutter", makeLayout())
        {
            sections = { { "Pattern", { "pattern", "rate" }, 0, 54 }, { "Shape", { "depth", "smooth", "mix" }, 0, 58 } };
            presets = { { "Default", {} } };
            for (int i = 1; i < (int) std::size (kPatterns); ++i)
                presets.push_back ({ kPatterns[i].name, { { "pattern", (double) i } } });
        }
        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::choice ("pattern", "Pattern", patternNames(), 0));
            l.add (AmParams::choice ("rate", "Step", { "1/8", "1/16", "1/32" }, 1));
            l.add (AmParams::num ("depth", "Depth", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("smooth", "Smooth", 0.5f, 40.0f, 4.0f, "ms", 1, 6.0f));
            l.add (AmParams::num ("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
            return l;
        }
        void prepareToPlay (double sr, int) override { sampleRate = sr; gain = 1.0f; freePpq = 0.0; }
        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals nd;
            const int n = buffer.getNumSamples(), nCh = juce::jmin (2, buffer.getNumChannels());
            const auto t = hostTime (*this);
            double ppq = t.playing ? t.ppq : freePpq;
            const double ppqPerSample = t.bpm / 60.0 / sampleRate;
            const double stepBeats = getInt ("rate") == 0 ? 0.5 : getInt ("rate") == 1 ? 0.25 : 0.125;
            const char* steps = kPatterns[juce::jlimit (0, (int) std::size (kPatterns) - 1, getInt ("pattern"))].steps;
            const float depth = get ("depth") * 0.01f, mix = get ("mix") * 0.01f;
            const float sm = onePole (get ("smooth"), sampleRate);
            for (int i = 0; i < n; ++i)
            {
                const auto step = (juce::int64) std::floor (ppq / stepBeats);
                const bool on = steps[(size_t) (((step % 16) + 16) % 16)] == 'x';
                gain = sm * gain + (1.0f - sm) * (on ? 1.0f : 1.0f - depth);
                const float g = 1.0f - mix + mix * gain;
                for (int c = 0; c < nCh; ++c)
                    buffer.setSample (c, i, buffer.getSample (c, i) * g);
                ppq += ppqPerSample;
                if (i == 0) currentStep.store ((int) (((step % 16) + 16) % 16));
            }
            freePpq = ppq;
        }
        std::unique_ptr<juce::Component> createVisual() override;
        int getVisualHeight() const override { return 64; }
        std::atomic<int> currentStep { 0 };
    private:
        float gain = 1.0f;
        double freePpq = 0.0;
    };

    class GateVisual : public juce::Component, private juce::Timer
    {
    public:
        explicit GateVisual (GatePlugin& p) : plugin (p) { startTimerHz (30); }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (4.0f);
            const char* steps = kPatterns[juce::jlimit (0, (int) std::size (kPatterns) - 1, plugin.getInt ("pattern"))].steps;
            const float w = r.getWidth() / 16.0f;
            for (int i = 0; i < 16; ++i)
            {
                auto cell = juce::Rectangle<float> (r.getX() + i * w, r.getY(), w, r.getHeight()).reduced (2.0f);
                const bool on = steps[i] == 'x';
                g.setColour (on ? Theme::accent.withAlpha (i == plugin.currentStep.load() ? 1.0f : 0.55f) : Theme::panelLight);
                g.fillRoundedRectangle (cell, 4.0f);
                if (i == plugin.currentStep.load())
                {
                    g.setColour (Theme::text);
                    g.drawRoundedRectangle (cell, 4.0f, 1.5f);
                }
            }
        }
    private:
        void timerCallback() override { repaint(); }
        GatePlugin& plugin;
    };
    std::unique_ptr<juce::Component> GatePlugin::createVisual() { return std::make_unique<GateVisual> (*this); }

    //==========================================================================
    // AUTO FILTER: resonant low/high/band-pass swept by a (tempo-synced) LFO and an envelope follower.
    class AutoFilterPlugin : public AmethystPlugin
    {
    public:
        AutoFilterPlugin() : AmethystPlugin ("amethyst.autofilter", "Auto Filter", "Creative", "LFO and envelope filter sweeps", makeLayout())
        {
            sections = { { "Filter", { "type", "cutoff", "reso" }, 0, 58 },
                         { "Motion", { "sync", "rate", "hz", "depth", "shape", "env" }, 1, 54 },
                         { "Out", { "mix" }, 1, 54 } };
            presets = {
                { "Default", {} },
                { "Slow Sweep", { { "rate", 6 }, { "depth", 3 } } },
                { "Wah Follow", { { "type", 2 }, { "depth", 0 }, { "env", 70 }, { "reso", 60 } } },
                { "Telephone Wobble", { { "type", 2 }, { "cutoff", 1200 }, { "depth", 0.8 }, { "reso", 30 } } },
                { "Rising Highpass", { { "type", 1 }, { "cutoff", 120 }, { "rate", 8 }, { "shape", 2 }, { "depth", 4 } } },
            };
        }
        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::choice ("type", "Type", { "Low pass", "High pass", "Band pass" }, 0));
            l.add (AmParams::num ("cutoff", "Cutoff", 40.0f, 18000.0f, 1500.0f, "Hz", 0, 1200.0f));
            l.add (AmParams::num ("reso", "Resonance", 0.0f, 100.0f, 25.0f, "%", 0));
            l.add (AmParams::toggle ("sync", "Sync", true));
            l.add (AmParams::choice ("rate", "Rate", { "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/4 T", "1/8 T" }, 4));
            l.add (AmParams::num ("hz", "Free rate", 0.05f, 12.0f, 1.0f, "Hz", 2, 1.0f));
            l.add (AmParams::num ("depth", "Depth", 0.0f, 5.0f, 2.0f, "oct", 1));
            l.add (AmParams::choice ("shape", "Shape", { "Sine", "Triangle", "Saw up", "Square" }, 0));
            l.add (AmParams::num ("env", "Envelope", -100.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::num ("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
            return l;
        }
        void prepareToPlay (double sr, int bs) override
        {
            sampleRate = sr;
            juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, bs), 2 };
            svf.prepare (spec);
            svf.reset();
            env = 0.0f;
            freePhase = 0.0;
        }
        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals nd;
            const int n = buffer.getNumSamples(), nCh = juce::jmin (2, buffer.getNumChannels());
            static const double beats[] = { 16, 8, 4, 2, 1, 0.5, 0.25, 2.0 / 3.0, 1.0 / 3.0 };
            const auto t = hostTime (*this);
            const bool sync = getBool ("sync");
            const double cycleBeats = beats[juce::jlimit (0, 8, getInt ("rate"))];
            const double hz = sync ? t.bpm / 60.0 / cycleBeats : get ("hz");
            double phase = sync && t.playing ? std::fmod (t.ppq / cycleBeats, 1.0) : freePhase;
            const double inc = hz / sampleRate;
            const int type = getInt ("type"), shapeType = getInt ("shape");
            svf.setType (type == 0 ? juce::dsp::StateVariableTPTFilterType::lowpass
                       : type == 1 ? juce::dsp::StateVariableTPTFilterType::highpass : juce::dsp::StateVariableTPTFilterType::bandpass);
            const float resonance = 0.5f + get ("reso") * 0.08f;
            svf.setResonance (resonance);
            const float resoComp = 1.0f / std::sqrt (juce::jmax (1.0f, resonance));   // keep the peak from jumping out
            const float base = get ("cutoff"), depth = get ("depth"), envAmt = get ("env") * 0.01f, mix = get ("mix") * 0.01f;
            const float envC = onePole (30.0, sampleRate);

            dry.makeCopyOf (buffer, true);
            for (int i = 0; i < n; i += 16)
            {
                const int k = juce::jmin (16, n - i);
                float peak = 0.0f;
                for (int c = 0; c < nCh; ++c) peak = juce::jmax (peak, buffer.getMagnitude (c, i, k));
                env = envC * env + (1.0f - envC) * peak;
                double lfoV = 0.0;
                switch (shapeType)
                {
                    case 0: lfoV = std::sin (juce::MathConstants<double>::twoPi * phase); break;
                    case 1: lfoV = 1.0 - 4.0 * std::abs (phase - 0.5); break;
                    case 2: lfoV = 2.0 * phase - 1.0; break;
                    default: lfoV = phase < 0.5 ? 1.0 : -1.0; break;
                }
                const double oct = lfoV * depth * 0.5 + envAmt * 4.0 * juce::jmin (1.0f, env * 3.0f);
                svf.setCutoffFrequency ((float) juce::jlimit (30.0, sampleRate * 0.45, base * std::pow (2.0, oct)));
                juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) nCh, (size_t) i, (size_t) k);
                juce::dsp::ProcessContextReplacing<float> ctx (block);
                svf.process (ctx);
                phase += inc * k;
                phase -= std::floor (phase);
            }
            freePhase = phase;
            for (int c = 0; c < nCh; ++c)
            {
                buffer.applyGain (c, 0, n, mix * resoComp);
                buffer.addFrom (c, 0, dry, c, 0, n, 1.0f - mix);
            }
        }
    private:
        juce::dsp::StateVariableTPTFilter<float> svf;
        juce::AudioBuffer<float> dry;
        float env = 0.0f;
        double freePhase = 0.0;
    };

    //==========================================================================
    // LOUDNESS METER (free): LUFS momentary / short-term / integrated, true peak and a target line.
    class LoudnessPlugin : public AmethystPlugin
    {
    public:
        LoudnessPlugin() : AmethystPlugin ("amethyst.loudness", "Loudness Meter", "Analysis", "LUFS and true-peak metering", makeLayout())
        {
            sections = { { "Target", { "target" }, 0, 54 } };
            presets = { { "Default", {} }, { "Apple Music (-16)", { { "target", 1 } } }, { "Club (-9)", { { "target", 2 } } }, { "Broadcast (-23)", { { "target", 3 } } } };
        }
        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::choice ("target", "Target", { "Spotify / YouTube -14", "Apple Music -16", "Club -9", "Broadcast -23" }, 0));
            return l;
        }
        float targetLufs() const { static const float t[] = { -14.0f, -16.0f, -9.0f, -23.0f }; return t[juce::jlimit (0, 3, getInt ("target"))]; }

        void prepareToPlay (double sr, int) override { sampleRate = sr; meter.prepare (sr); }
        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            if (resetRequested.exchange (false))
                meter.reset();
            const int n = buffer.getNumSamples();
            meter.process (buffer.getReadPointer (0), buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : nullptr, n);
            if (++publishCount >= 4)
            {
                publishCount = 0;
                momentary.store ((float) meter.momentary());
                shortTerm.store ((float) meter.shortTerm());
                integrated.store ((float) meter.integrated());
                truePeak.store (meter.truePeakDb());
            }
        }
        std::unique_ptr<juce::Component> createVisual() override;
        int getVisualHeight() const override { return 190; }
        int getMinimumEditorWidth() const override { return 460; }

        std::atomic<float> momentary { -100.0f }, shortTerm { -100.0f }, integrated { -100.0f }, truePeak { -100.0f };
        std::atomic<bool> resetRequested { false };
    private:
        Loudness::Meter meter;
        int publishCount = 0;
    };

    class LoudnessVisual : public juce::Component, private juce::Timer
    {
    public:
        explicit LoudnessVisual (LoudnessPlugin& p) : plugin (p),
            graph ([&p] { return p.shortTerm.load(); }, -40.0f, 0.0f, " LUFS", { -30.0f, -23.0f, -14.0f, -9.0f })
        {
            addAndMakeVisible (graph);
            resetBtn.onClick = [this] { plugin.resetRequested = true; };
            addAndMakeVisible (resetBtn);
            startTimerHz (10);
        }
        void resized() override
        {
            auto r = getLocalBounds();
            auto top = r.removeFromTop (70);
            resetBtn.setBounds (top.removeFromRight (70).withSizeKeepingCentre (62, 24));
            graph.setBounds (r);
        }
        void paint (juce::Graphics& g) override
        {
            auto top = getLocalBounds().removeFromTop (70).withTrimmedRight (74).toFloat();
            const float target = plugin.targetLufs();
            auto cell = [&] (int i, const juce::String& label, float v, bool tp)
            {
                auto c = top.withWidth (top.getWidth() / 4.0f).translated (top.getWidth() / 4.0f * i, 0).reduced (3.0f);
                g.setColour (juce::Colours::black.withAlpha (0.25f));
                g.fillRoundedRectangle (c, 8.0f);
                g.setColour (Theme::textDim);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (label, c.removeFromTop (20), juce::Justification::centred);
                const bool hot = tp ? v > -1.0f : (i == 2 && v > target + 1.0f);
                g.setColour (hot ? Theme::record : (i == 2 && std::abs (v - target) <= 1.0f ? Theme::accent : Theme::text));
                g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
                g.drawText (v <= -99.0f ? juce::String ("--") : juce::String (v, 1), c, juce::Justification::centred);
            };
            cell (0, "Momentary", plugin.momentary.load(), false);
            cell (1, "Short-term", plugin.shortTerm.load(), false);
            cell (2, "Integrated (target " + juce::String (target, 0) + ")", plugin.integrated.load(), false);
            cell (3, "True peak dBTP", plugin.truePeak.load(), true);
        }
    private:
        void timerCallback() override { repaint (getLocalBounds().removeFromTop (70)); }
        LoudnessPlugin& plugin;
        HistoryGraph graph;
        juce::TextButton resetBtn { "Reset" };
    };
    std::unique_ptr<juce::Component> LoudnessPlugin::createVisual() { return std::make_unique<LoudnessVisual> (*this); }
}

std::unique_ptr<AmethystPlugin> Builtins::makeRider()      { return std::make_unique<RiderPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeDenoise()    { return std::make_unique<DenoisePlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeHarmony()    { return std::make_unique<HarmonyPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeImager()     { return std::make_unique<ImagerPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeTransient()  { return std::make_unique<TransientPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeGate()       { return std::make_unique<GatePlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeAutoFilter() { return std::make_unique<AutoFilterPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeLoudness()   { return std::make_unique<LoudnessPlugin>(); }
