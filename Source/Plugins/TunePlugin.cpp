#include "Builtins.h"

// AMETHYST TUNE - real-time vocal pitch correction (Ultra Pro).
//
// Detection: YIN on a decimated (~12 kHz) mono copy of the input, every ~5 ms.
// Correction: the detected pitch is pulled towards the nearest allowed note; Retune speed
// smooths how fast, Humanize lets small natural wobbles (vibrato) through.
// Shifting: a single varying-delay read head (clean when no correction is needed), spliced
// pitch-synchronously with a short crossfade whenever it drifts too far.

namespace
{
    const juce::StringArray kNoteNames { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const juce::StringArray kScales { "Chromatic", "Major", "Minor", "Harmonic minor", "Major pentatonic", "Minor pentatonic", "Blues", "Custom" };
    constexpr int kCustomScale = 7;
    const juce::StringArray kInputTypes { "Soprano", "Alto / Tenor", "Low male", "Instrument" };

    int scaleIntervals (int scale)
    {
        auto bits = [] (std::initializer_list<int> notes) { int m = 0; for (int n : notes) m |= 1 << n; return m; };
        switch (scale)
        {
            case 1:  return bits ({ 0, 2, 4, 5, 7, 9, 11 });
            case 2:  return bits ({ 0, 2, 3, 5, 7, 8, 10 });
            case 3:  return bits ({ 0, 2, 3, 5, 7, 8, 11 });
            case 4:  return bits ({ 0, 2, 4, 7, 9 });
            case 5:  return bits ({ 0, 3, 5, 7, 10 });
            case 6:  return bits ({ 0, 3, 5, 6, 7, 10 });
            default: return 0xFFF;
        }
    }

    int rotateMask (int mask, int key) { return ((mask << key) | (mask >> (12 - key))) & 0xFFF; }

    //==========================================================================
    class TunePlugin : public AmethystPlugin
    {
    public:
        TunePlugin() : AmethystPlugin ("amethyst.tune", "AMETHYST TUNE", "Pitch", "Real-time vocal pitch correction", makeLayout())
        {
            presets = {
                { "Default", {} },
                { "Hard Tune",       { { "retune", 0 }, { "humanize", 0 } } },
                { "Pop Polish",      { { "retune", 20 }, { "humanize", 30 }, { "scale", 1 } } },
                { "Natural",         { { "retune", 70 }, { "humanize", 55 } } },
                { "Subtle Fix",      { { "retune", 140 }, { "humanize", 75 } } },
                { "Trap Lead",       { { "retune", 5 }, { "humanize", 10 }, { "scale", 2 } } },
                { "Choir Smooth",    { { "retune", 200 }, { "humanize", 80 }, { "mix", 85 } } },
                { "Robot",           { { "retune", 0 }, { "humanize", 0 }, { "scale", 4 }, { "throat", 1 } } },
                { "Low Voice",       { { "retune", 30 }, { "humanize", 25 }, { "input", 2 } } },
                { "High Voice",      { { "retune", 30 }, { "humanize", 25 }, { "input", 0 } } },
            };
            for (auto& s : scoreHist)
                s = 0.0f;
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::choice ("key", "Key", kNoteNames, 0));
            l.add (AmParams::choice ("scale", "Scale", kScales, 0));
            l.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { "mask", 1 }, "Notes", 0, 4095, 4095));
            l.add (AmParams::num ("retune", "Retune speed", 0.0f, 400.0f, 25.0f, "ms", 0, 60.0f));
            l.add (AmParams::num ("humanize", "Humanize", 0.0f, 100.0f, 20.0f, "%", 0));
            l.add (AmParams::num ("tracking", "Tracking", 0.0f, 100.0f, 50.0f, "%", 0));
            l.add (AmParams::toggle ("lock", "Scale lock", false));
            l.add (AmParams::choice ("formant", "Formant", { "Auto", "Off" }, 0));
            l.add (AmParams::toggle ("throat", "Throat", false));
            l.add (AmParams::choice ("input", "Input type", kInputTypes, 1));
            l.add (AmParams::num ("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::toggle ("bypass", "Bypass", false));
            l.add (AmParams::num ("ref", "Reference", 430.0f, 450.0f, 440.0f, "Hz", 1));
            return l;
        }

        int effectiveMask() const
        {
            const int scale = getInt ("scale");
            if (scale == kCustomScale)
            {
                const int m = getInt ("mask") & 0xFFF;
                return m == 0 ? 0xFFF : m;
            }
            return rotateMask (scaleIntervals (scale), getInt ("key"));
        }

        //======================================================================
        void prepareToPlay (double sr, int) override
        {
            sampleRate = sr;
            decim = juce::jmax (1, juce::roundToInt (sr / 12000.0));
            fsd = sr / decim;
            antiAlias.setCoefficients (juce::IIRCoefficients::makeLowPass (sr, fsd * 0.42));
            antiAlias.reset();
            ana.assign (4096, 0.0f);
            anaW = 0; decimCount = 0; hopCount = 0;

            for (auto& r : rings) r.assign ((size_t) (sr * 0.1) + 8, 0.0f);
            ringW = 0;
            baseDelay = sr * 0.025;
            minDelay = sr * 0.006;
            maxDelay = sr * 0.044;
            headA = headB = baseDelay;
            fading = false;
            fadeLen = juce::jmax (32, (int) (sr * 0.006));

            corr = 0.0; currentTarget = -1; periodSamples = 0.0;
            for (auto* f : { &formantL, &formantR, &throatLowL, &throatLowR, &throatHighL, &throatHighR })
                f->reset();
            lastFormantDb = 999.0f;
            lastThroat = -1;
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            if (getBool ("bypass"))
                return;

            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

            // parameters for this block
            const int mask = effectiveMask();
            const double retuneSec = get ("retune") / 1000.0;
            const float humanize = get ("humanize") * 0.01f;
            const float threshold = 0.05f + 0.25f * get ("tracking") * 0.01f;
            const double ref = get ("ref");
            const float mix = get ("mix") * 0.01f;
            static constexpr float ranges[4][2] = { { 160.0f, 1300.0f }, { 85.0f, 900.0f }, { 55.0f, 420.0f }, { 45.0f, 1500.0f } };
            const auto& range = ranges[juce::jlimit (0, 3, getInt ("input"))];
            const double hopSec = (double) kHop / fsd;
            const double coef = retuneSec <= 0.0005 ? 1.0 : 1.0 - std::exp (-hopSec / retuneSec);
            const double releaseCoef = 1.0 - std::exp (-hopSec / 0.03);

            updateTone();

            float peak = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float xl = L[i], xr = R != nullptr ? R[i] : xl;
                const float mono = 0.5f * (xl + xr);
                peak = juce::jmax (peak, std::abs (mono));

                // --- decimate into the analysis buffer and run detection every hop ---
                const float filtered = antiAlias.processSingleSampleRaw (mono);
                if (++decimCount >= decim)
                {
                    decimCount = 0;
                    ana[(size_t) anaW] = filtered;
                    anaW = (anaW + 1) & 4095;
                    if (++hopCount >= kHop)
                    {
                        hopCount = 0;
                        const float midi = detect (range[0], range[1], threshold, ref);
                        updateCorrection (midi, mask, humanize, coef, releaseCoef, hopSec);
                    }
                }

                // --- shift ---
                rings[0][(size_t) ringW] = xl;
                rings[1][(size_t) ringW] = xr;
                const double ratio = std::pow (2.0, corrSmoothed / 12.0);
                corrSmoothed += (corr - corrSmoothed) * 0.002; // de-zipper between hops

                headA += 1.0 - ratio;
                if (fading)
                    headB += 1.0 - ratio;

                if (! fading && (headA < minDelay || headA > maxDelay))
                {
                    // splice by a whole number of pitch periods so the join is in phase
                    const double period = periodSamples > 1.0 ? periodSamples : sampleRate * 0.01;
                    const double k = std::round ((baseDelay - headA) / period);
                    headB = headA + k * period;
                    if (headB < minDelay || headB > maxDelay)
                        headB = baseDelay;
                    fading = true;
                    fadePos = 0;
                }

                float wl = read (0, headA), wr = read (1, headA);
                if (fading)
                {
                    const float t = (float) fadePos / (float) fadeLen;
                    const float ga = std::cos (t * juce::MathConstants<float>::halfPi), gb = std::sin (t * juce::MathConstants<float>::halfPi);
                    wl = wl * ga + read (0, headB) * gb;
                    wr = wr * ga + read (1, headB) * gb;
                    if (++fadePos >= fadeLen)
                    {
                        headA = headB;
                        fading = false;
                    }
                }

                // tone shaping on the corrected voice
                wl = throatHighL.processSingleSampleRaw (throatLowL.processSingleSampleRaw (formantL.processSingleSampleRaw (wl)));
                wr = throatHighR.processSingleSampleRaw (throatLowR.processSingleSampleRaw (formantR.processSingleSampleRaw (wr)));

                // dry is read at the same latency so the mix never combs
                const float dl = read (0, baseDelay), dr = read (1, baseDelay);
                L[i] = dl + mix * (wl - dl);
                if (R != nullptr)
                    R[i] = dr + mix * (wr - dr);

                ringW = (ringW + 1) % (int) rings[0].size();
            }

            inputLevel.store (juce::jmax (peak, inputLevel.load() * 0.9f));
            correctionCents.store ((float) (corr * 100.0));
        }

        // UI-facing state ---------------------------------------------------------
        std::atomic<float> detectedMidi { -1.0f }, targetMidi { -1.0f }, correctionCents { 0.0f }, inputLevel { 0.0f };
        std::array<std::atomic<float>, 12> scoreHist;
        std::atomic<bool> learning { false };
        std::atomic<float> learnedSeconds { 0.0f };

        void copyScope (std::vector<float>& out) const
        {
            out.resize (256);
            int r = anaW;
            for (int i = 255; i >= 0; --i)
            {
                r = (r - 2 + 4096) & 4095;
                out[(size_t) i] = ana[(size_t) r];
            }
        }

        juce::AudioProcessorEditor* createEditor() override;
        double getTailLengthSeconds() const override { return 0.05; }

    private:
        static constexpr int kHop = 64; // decimated samples between detections (~5 ms)

        float read (int ch, double delay) const
        {
            const auto& ring = rings[(size_t) ch];
            const int len = (int) ring.size();
            double pos = ringW - delay;
            while (pos < 0) pos += len;
            const int i0 = (int) pos;
            const float f = (float) (pos - i0);
            const float a = ring[(size_t) i0], b = ring[(size_t) ((i0 + 1) % len)];
            return a + (b - a) * f;
        }

        // YIN pitch detection on the decimated buffer. Returns MIDI note (float) or -1.
        float detect (float minHz, float maxHz, float threshold, double ref)
        {
            const int maxLag = juce::jmin (900, (int) (fsd / minHz));
            const int minLag = juce::jmax (2, (int) (fsd / maxHz));
            const int window = maxLag;
            const int total = window + maxLag + 2;
            if (total >= 4096)
                return -1.0f;

            // copy the most recent samples, oldest first
            frame.resize ((size_t) total);
            int r = (anaW - total + 4096) & 4095;
            double energy = 0.0;
            for (int i = 0; i < total; ++i)
            {
                frame[(size_t) i] = ana[(size_t) r];
                energy += frame[(size_t) i] * frame[(size_t) i];
                r = (r + 1) & 4095;
            }
            if (energy / total < 1.0e-6) // too quiet to be singing
            {
                detectedMidi.store (-1.0f);
                return -1.0f;
            }

            diff.assign ((size_t) maxLag + 2, 0.0f);
            for (int tau = 1; tau <= maxLag; ++tau)
            {
                double s = 0.0;
                for (int j = 0; j < window; ++j)
                {
                    const float d = frame[(size_t) j] - frame[(size_t) (j + tau)];
                    s += d * d;
                }
                diff[(size_t) tau] = (float) s;
            }

            // cumulative mean normalised difference
            double running = 0.0;
            diff[0] = 1.0f;
            for (int tau = 1; tau <= maxLag; ++tau)
            {
                running += diff[(size_t) tau];
                diff[(size_t) tau] = running > 0.0 ? (float) (diff[(size_t) tau] * tau / running) : 1.0f;
            }

            int best = -1;
            for (int tau = minLag; tau <= maxLag; ++tau)
            {
                if (diff[(size_t) tau] < threshold)
                {
                    while (tau + 1 <= maxLag && diff[(size_t) tau + 1] < diff[(size_t) tau])
                        ++tau;
                    best = tau;
                    break;
                }
            }
            if (best < 0)
            {
                detectedMidi.store (-1.0f);
                return -1.0f;
            }

            // parabolic interpolation for sub-sample accuracy
            double refined = best;
            if (best > 1 && best < maxLag)
            {
                const double a = diff[(size_t) best - 1], b = diff[(size_t) best], c = diff[(size_t) best + 1];
                const double denom = a - 2.0 * b + c;
                if (std::abs (denom) > 1.0e-9)
                    refined = best + 0.5 * (a - c) / denom;
            }

            const double hz = fsd / refined;
            periodSamples = sampleRate / hz;
            const float midi = (float) (69.0 + 12.0 * std::log2 (hz / ref));
            detectedMidi.store (midi);
            return midi;
        }

        void updateCorrection (float midi, int mask, float humanize, double coef, double releaseCoef, double hopSec)
        {
            if (midi < 0.0f)
            {
                corr += (0.0 - corr) * releaseCoef; // unvoiced: let go
                targetMidi.store (-1.0f);
                currentTarget = -1;
                return;
            }

            // nearest allowed note, with a little hysteresis so it doesn't flicker between two
            int bestNote = -1;
            float bestDist = 99.0f;
            for (int n = (int) std::floor (midi) - 6; n <= (int) std::ceil (midi) + 6; ++n)
            {
                if ((mask >> (((n % 12) + 12) % 12) & 1) == 0)
                    continue;
                const float d = std::abs ((float) n - midi);
                if (d < bestDist) { bestDist = d; bestNote = n; }
            }
            if (bestNote < 0)
                return;
            if (currentTarget >= 0 && currentTarget != bestNote && ((mask >> (currentTarget % 12)) & 1)
                && std::abs ((float) currentTarget - midi) < bestDist + 0.15f)
                bestNote = currentTarget;
            currentTarget = bestNote;
            targetMidi.store ((float) bestNote);

            double desired = bestNote - midi;
            // Humanize: let small wobbles (vibrato, natural drift) through
            const double closeness = juce::jlimit (0.0, 1.0, 1.0 - std::abs (desired) / 0.5);
            desired *= 1.0 - humanize * 0.8 * closeness;
            corr += (desired - corr) * coef;

            if (learning.load())
            {
                const int pc = ((bestNote % 12) + 12) % 12;
                scoreHist[(size_t) pc].store (scoreHist[(size_t) pc].load() + (float) hopSec);
                learnedSeconds.store (learnedSeconds.load() + (float) hopSec);
            }
        }

        void updateTone()
        {
            // Formant "Auto": shifting up brightens the voice, shifting down darkens it - counter that a little.
            const float formantDb = getInt ("formant") == 0 ? (float) juce::jlimit (-6.0, 6.0, -corr * 0.8) : 0.0f;
            if (std::abs (formantDb - lastFormantDb) > 0.05f)
            {
                lastFormantDb = formantDb;
                const auto c = juce::IIRCoefficients::makeHighShelf (sampleRate, 2500.0, 0.7, juce::Decibels::decibelsToGain (formantDb));
                formantL.setCoefficients (c);
                formantR.setCoefficients (c);
            }
            const int throat = getBool ("throat") ? 1 : 0;
            if (throat != lastThroat)
            {
                lastThroat = throat;
                const auto lo = juce::IIRCoefficients::makePeakFilter (sampleRate, 220.0, 1.0, juce::Decibels::decibelsToGain (throat ? 3.0f : 0.0f));
                const auto hi = juce::IIRCoefficients::makeHighShelf (sampleRate, 4000.0, 0.7, juce::Decibels::decibelsToGain (throat ? -2.5f : 0.0f));
                throatLowL.setCoefficients (lo);  throatLowR.setCoefficients (lo);
                throatHighL.setCoefficients (hi); throatHighR.setCoefficients (hi);
            }
        }

        int decim = 4, decimCount = 0, hopCount = 0, anaW = 0;
        double fsd = 12000.0;
        juce::IIRFilter antiAlias;
        std::vector<float> ana, frame, diff;

        std::array<std::vector<float>, 2> rings;
        int ringW = 0;
        double baseDelay = 1200, minDelay = 300, maxDelay = 2100, headA = 1200, headB = 1200;
        bool fading = false;
        int fadePos = 0, fadeLen = 256;

        double corr = 0.0, corrSmoothed = 0.0, periodSamples = 0.0;
        int currentTarget = -1;

        juce::IIRFilter formantL, formantR, throatLowL, throatLowR, throatHighL, throatHighR;
        float lastFormantDb = 999.0f;
        int lastThroat = -1;

        friend class TuneEditor;
    };

    //==========================================================================
    // Pill-shaped on/off switch.
    class ToggleSwitch : public juce::Button
    {
    public:
        ToggleSwitch() : juce::Button ({}) { setClickingTogglesState (true); }
        void paintButton (juce::Graphics& g, bool over, bool) override
        {
            auto r = getLocalBounds().toFloat().withSizeKeepingCentre (52.0f, 26.0f);
            const bool on = getToggleState();
            if (on)
            {
                g.setColour (Theme::accent.withAlpha (0.3f));
                g.fillRoundedRectangle (r.expanded (2.0f), 15.0f);
                g.setGradientFill (juce::ColourGradient (Theme::accent2, r.getX(), r.getY(), Theme::accent, r.getRight(), r.getBottom(), false));
            }
            else
                g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.fillRoundedRectangle (r, 13.0f);
            g.setColour (juce::Colours::white.withAlpha (over ? 0.3f : 0.14f));
            g.drawRoundedRectangle (r.reduced (0.5f), 13.0f, 1.0f);
            const auto knob = juce::Rectangle<float> (20.0f, 20.0f).withCentre ({ on ? r.getRight() - 13.0f : r.getX() + 13.0f, r.getCentreY() });
            g.setGradientFill (juce::ColourGradient (juce::Colours::white, knob.getX(), knob.getY(), juce::Colour (0xffb9b2c9), knob.getX(), knob.getBottom(), false));
            g.fillEllipse (knob);
        }
    };

    // Simple vector-icon button for the header.
    class HeaderIcon : public juce::Button
    {
    public:
        enum class Kind { Undo, Redo, Gear, Power, Left, Right, Down };
        explicit HeaderIcon (Kind k) : juce::Button ({}), kind (k) {}

        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (3.0f);
            if (over || down)
            {
                g.setColour (juce::Colours::white.withAlpha (down ? 0.05f : 0.09f));
                g.fillRoundedRectangle (r, 8.0f);
            }
            const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.55f;
            auto ic = juce::Rectangle<float> (s, s).withCentre (r.getCentre());
            juce::Colour col = Theme::text.withAlpha (isEnabled() ? 0.9f : 0.3f);
            if (kind == Kind::Power && getToggleState())
            {
                g.setColour (Theme::accent.withAlpha (0.3f));
                g.fillEllipse (ic.expanded (6.0f));
                col = Theme::accent2;
            }
            g.setColour (col);
            juce::PathStrokeType stroke (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
            juce::Path p;
            switch (kind)
            {
                case Kind::Undo:
                case Kind::Redo:
                {
                    const bool redo = kind == Kind::Redo;
                    p.addCentredArc (ic.getCentreX(), ic.getCentreY() + s * 0.1f, s * 0.4f, s * 0.32f, 0.0f,
                                     redo ? -1.9f : 1.9f, redo ? 1.2f : -1.2f, true);
                    g.strokePath (p, stroke);
                    const auto tip = juce::Point<float> (redo ? ic.getRight() - s * 0.05f : ic.getX() + s * 0.05f, ic.getCentreY() - s * 0.05f);
                    juce::Path head;
                    head.addTriangle (tip.x, tip.y - s * 0.28f, tip.x, tip.y + s * 0.12f, tip.x + (redo ? -s * 0.3f : s * 0.3f), tip.y - s * 0.1f);
                    g.fillPath (head);
                    break;
                }
                case Kind::Gear:
                {
                    for (int i = 0; i < 8; ++i)
                    {
                        const float a = juce::MathConstants<float>::twoPi * (float) i / 8.0f;
                        g.drawLine ({ ic.getCentre().getPointOnCircumference (s * 0.32f, a), ic.getCentre().getPointOnCircumference (s * 0.5f, a) }, 2.4f);
                    }
                    g.drawEllipse (ic.reduced (s * 0.2f), 2.0f);
                    break;
                }
                case Kind::Power:
                    p.addCentredArc (ic.getCentreX(), ic.getCentreY(), s * 0.45f, s * 0.45f, 0.0f, 0.6f, juce::MathConstants<float>::twoPi - 0.6f, true);
                    g.strokePath (p, stroke);
                    g.drawLine (ic.getCentreX(), ic.getY() - s * 0.05f, ic.getCentreX(), ic.getCentreY(), 2.2f);
                    break;
                case Kind::Left:
                case Kind::Right:
                case Kind::Down:
                {
                    const float h = s * 0.35f;
                    if (kind == Kind::Down)  { p.startNewSubPath (ic.getCentreX() - h, ic.getCentreY() - h * 0.5f); p.lineTo (ic.getCentreX(), ic.getCentreY() + h * 0.5f); p.lineTo (ic.getCentreX() + h, ic.getCentreY() - h * 0.5f); }
                    if (kind == Kind::Left)  { p.startNewSubPath (ic.getCentreX() + h * 0.5f, ic.getCentreY() - h); p.lineTo (ic.getCentreX() - h * 0.5f, ic.getCentreY()); p.lineTo (ic.getCentreX() + h * 0.5f, ic.getCentreY() + h); }
                    if (kind == Kind::Right) { p.startNewSubPath (ic.getCentreX() - h * 0.5f, ic.getCentreY() - h); p.lineTo (ic.getCentreX() + h * 0.5f, ic.getCentreY()); p.lineTo (ic.getCentreX() - h * 0.5f, ic.getCentreY() + h); }
                    g.strokePath (p, stroke);
                    break;
                }
            }
        }

    private:
        Kind kind;
    };

    //==========================================================================
    class TuneEditor : public juce::AudioProcessorEditor, private juce::Timer
    {
    public:
        explicit TuneEditor (TunePlugin& p) : juce::AudioProcessorEditor (p), plugin (p)
        {
            // header
            for (auto* b : { (juce::Button*) &prevBtn, (juce::Button*) &menuBtn, (juce::Button*) &undoBtn, (juce::Button*) &redoBtn,
                             (juce::Button*) &gearBtn, (juce::Button*) &powerBtn })
                addAndMakeVisible (b);
            prevBtn.onClick = [this] { stepPreset (-1); };
            menuBtn.onClick = [this] { showPresetMenu(); };
            presetLabel.setJustificationType (juce::Justification::centred);
            presetLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            presetLabel.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (presetLabel);
            undoBtn.setTooltip ("Undo");  undoBtn.onClick = [this] { plugin.undoManager.undo(); };
            redoBtn.setTooltip ("Redo");  redoBtn.onClick = [this] { plugin.undoManager.redo(); };
            gearBtn.setTooltip ("Settings"); gearBtn.onClick = [this] { showSettingsMenu(); };
            powerBtn.setTooltip ("On / bypass");
            powerBtn.setClickingTogglesState (true);
            powerBtn.onClick = [this] { plugin.setParam ("bypass", powerBtn.getToggleState() ? 0.0f : 1.0f); };

            // key column
            for (int i = 0; i < 12; ++i)
            {
                auto* b = keyBtns.add (new juce::TextButton (kNoteNames[i]));
                b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff5b3fe0));
                b->onClick = [this, i] { plugin.setParam ("key", (float) i); };
                addAndMakeVisible (b);
            }
            learnBtn.setClickingTogglesState (true);
            learnBtn.setTooltip ("Sing for a few seconds and AMETHYST TUNE picks the key and scale for you");
            learnBtn.onClick = [this] { toggleLearn(); };
            addAndMakeVisible (learnBtn);

            // knobs
            for (auto* k : { &retuneKnob, &humanizeKnob })
            {
                k->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
                k->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                k->setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
                addAndMakeVisible (k);
            }
            retuneAtt = std::make_unique<SA> (plugin.apvts, "retune", retuneKnob);
            humanizeAtt = std::make_unique<SA> (plugin.apvts, "humanize", humanizeKnob);
            retuneKnob.setDoubleClickReturnValue (true, 25.0);
            humanizeKnob.setDoubleClickReturnValue (true, 20.0);

            // control bar
            scaleBox.addItemList (kScales, 1);
            formantBox.addItemList ({ "Auto", "Off" }, 1);
            inputBox.addItemList (kInputTypes, 1);
            for (auto* c : { &scaleBox, &formantBox, &inputBox })
                addAndMakeVisible (c);
            scaleAtt = std::make_unique<CA> (plugin.apvts, "scale", scaleBox);
            formantAtt = std::make_unique<CA> (plugin.apvts, "formant", formantBox);
            inputAtt = std::make_unique<CA> (plugin.apvts, "input", inputBox);

            addAndMakeVisible (lockSwitch);
            addAndMakeVisible (throatSwitch);
            lockAtt = std::make_unique<BA> (plugin.apvts, "lock", lockSwitch);
            throatAtt = std::make_unique<BA> (plugin.apvts, "throat", throatSwitch);
            lockSwitch.setTooltip ("Scale lock: freezes the key/scale (Learn and piano clicks can't change it)");
            throatSwitch.setTooltip ("Throat: a deeper, fuller vocal character");
            formantBox.setTooltip ("Formant: Auto counters the brightness change when pitch is moved");

            for (auto* s : { &trackingSlider, &mixSlider })
            {
                s->setSliderStyle (juce::Slider::LinearHorizontal);
                s->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                addAndMakeVisible (s);
            }
            trackingAtt = std::make_unique<SA> (plugin.apvts, "tracking", trackingSlider);
            mixAtt = std::make_unique<SA> (plugin.apvts, "mix", mixSlider);
            trackingSlider.setTooltip ("Tracking: higher accepts breathier or noisier singing as pitched");

            // piano transpose
            keyDown.onClick = [this] { plugin.setParam ("key", (float) ((plugin.getInt ("key") + 11) % 12)); };
            keyUp.onClick = [this] { plugin.setParam ("key", (float) ((plugin.getInt ("key") + 1) % 12)); };
            addAndMakeVisible (keyDown);
            addAndMakeVisible (keyUp);

            for (auto* c : getChildren())
            {
                c->setWantsKeyboardFocus (false);
                c->addMouseListener (this, true);
            }

            setSize (800, 800);
            startTimerHz (30);
        }

        ~TuneEditor() override { plugin.learning = false; }

        void mouseDown (const juce::MouseEvent& e) override
        {
            plugin.undoManager.beginNewTransaction(); // each click/drag = one undo step

            // piano keys live in paint(); handle their clicks here
            if (e.eventComponent == this)
                handlePianoClick (e.getPosition());
        }

        void paint (juce::Graphics& g) override
        {
            if (bg.isNull() || bg.getWidth() != getWidth())
            {
                bg = juce::Image (juce::Image::RGB, getWidth(), getHeight(), false);
                juce::Graphics b (bg);
                Glass::background (b, getLocalBounds());
                b.setColour (juce::Colour (0xe0120c1d));
                b.fillRoundedRectangle (getLocalBounds().toFloat().reduced (6.0f), 22.0f);
                b.setColour (juce::Colours::white.withAlpha (0.08f));
                b.drawRoundedRectangle (getLocalBounds().toFloat().reduced (6.5f), 22.0f, 1.0f);
                b.setColour (juce::Colours::white.withAlpha (0.06f));
                b.drawHorizontalLine (headerArea.getBottom(), 20.0f, (float) getWidth() - 20.0f);
                for (auto* card : { &keyCard, &retuneCard, &humanizeCard, &barArea, &pianoBox })
                    Glass::panel (b, card->toFloat(), 14.0f, 0.8f);
                b.setColour (juce::Colours::black.withAlpha (0.25f));
                b.fillRect (footerArea.withTrimmedLeft (8).withTrimmedRight (8));
                drawStatic (b);
            }
            g.drawImageAt (bg, 0, 0);

            drawHeaderTitle (g);
            drawDial (g, dialArea.toFloat());
            drawBarLabels (g);
            drawPiano (g);
            drawKeyInfo (g);
            drawFooter (g);
        }

        // the big value readouts sit on top of the knobs
        void paintOverChildren (juce::Graphics& g) override { drawKnobValues (g); }

        void resized() override
        {
            bg = {};
            auto r = getLocalBounds().reduced (22, 14);

            headerArea = r.removeFromTop (58);
            {
                auto h = headerArea;
                auto right = h.removeFromRight (190);
                powerBtn.setBounds (right.removeFromRight (44));
                gearBtn.setBounds (right.removeFromRight (44));
                right.removeFromRight (8);
                redoBtn.setBounds (right.removeFromRight (44));
                undoBtn.setBounds (right.removeFromRight (44));
                presetArea = h.withSizeKeepingCentre (230, 40).translated (58, 0);
                prevBtn.setBounds (presetArea.withWidth (40));
                menuBtn.setBounds (presetArea.withTrimmedLeft (presetArea.getWidth() - 40));
                presetLabel.setBounds (presetArea.reduced (40, 0));
            }

            footerArea = getLocalBounds().removeFromBottom (64).reduced (6, 0);
            r.removeFromBottom (54);

            r.removeFromTop (12);
            auto body = r.removeFromTop (370);
            keyCard = body.removeFromLeft (96);
            {
                auto k = keyCard.reduced (10, 8);
                k.removeFromTop (30);
                learnBtn.setBounds (k.removeFromBottom (32));
                k.removeFromBottom (8);
                const int h = k.getHeight() / 12;
                for (auto* b : keyBtns)
                    b->setBounds (k.removeFromTop (h).reduced (2, 1));
            }
            body.removeFromLeft (14);
            auto right = body.removeFromRight (230);
            retuneCard = right.removeFromTop (right.getHeight() / 2 - 6);
            right.removeFromTop (12);
            humanizeCard = right;
            retuneKnob.setBounds (retuneCard.withTrimmedTop (34).withTrimmedBottom (34).withSizeKeepingCentre (130, 130));
            humanizeKnob.setBounds (humanizeCard.withTrimmedTop (34).withTrimmedBottom (34).withSizeKeepingCentre (130, 130));
            body.removeFromRight (14);
            dialArea = body;

            r.removeFromTop (14);
            barArea = r.removeFromTop (78);
            {
                auto b = barArea.reduced (16, 10);
                const int w = b.getWidth() / 5;
                auto cell = [&b, w] { return b.removeFromLeft (w); };
                auto c1 = cell(); scaleBox.setBounds (c1.withTrimmedTop (24).withSizeKeepingCentre (c1.getWidth() - 20, 30));
                auto c2 = cell(); lockSwitch.setBounds (c2.withTrimmedTop (24).withSizeKeepingCentre (60, 30));
                auto c3 = cell(); c3 = c3.withWidth (w + w / 2);
                trackingSlider.setBounds (c3.withTrimmedTop (24).withSizeKeepingCentre (c3.getWidth() - 20, 24));
                b.removeFromLeft (w / 2);
                auto c4 = cell(); formantBox.setBounds (c4.withTrimmedTop (24).withSizeKeepingCentre (c4.getWidth() - 10, 30));
                auto c5 = b; throatSwitch.setBounds (c5.withTrimmedTop (24).withSizeKeepingCentre (60, 30));
                barCells = { c1, c2, c3, c4, c5 };
            }

            r.removeFromTop (14);
            auto piano = r;
            pianoBox = piano;
            auto inner = piano.reduced (12, 12);
            keyInfo = inner.removeFromLeft (104);
            keyDown.setBounds (keyInfo.withTrimmedTop (keyInfo.getHeight() - 40).withWidth (keyInfo.getWidth() / 2).reduced (8, 4));
            keyUp.setBounds (keyInfo.withTrimmedTop (keyInfo.getHeight() - 40).withTrimmedLeft (keyInfo.getWidth() / 2).reduced (8, 4));
            inner.removeFromLeft (12);
            pianoArea = inner;

            {
                auto f = footerArea.reduced (40, 14);
                inputBox.setBounds (f.withTrimmedLeft (110).withWidth (170));
                mixSlider.setBounds (f.withTrimmedLeft (f.getWidth() - 200));
            }
        }

    private:
        using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
        using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
        using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;

        //------------------------------------------------------------------
        void timerCallback() override
        {
            const int key = plugin.getInt ("key");
            for (int i = 0; i < 12; ++i)
                keyBtns[i]->setToggleState (i == key, juce::dontSendNotification);
            powerBtn.setToggleState (! plugin.getBool ("bypass"), juce::dontSendNotification);
            undoBtn.setEnabled (plugin.undoManager.canUndo());
            redoBtn.setEnabled (plugin.undoManager.canRedo());
            if (presetLabel.getText() != plugin.currentPresetName)
                presetLabel.setText (plugin.currentPresetName, juce::dontSendNotification);

            // learn: finish after 8 seconds of singing
            if (plugin.learning.load() && plugin.learnedSeconds.load() >= 8.0f)
                finishLearn();
            learnBtn.setToggleState (plugin.learning.load(), juce::dontSendNotification);

            // smooth the displayed values
            const float cents = plugin.correctionCents.load();
            shownCents += (cents - shownCents) * 0.35f;
            const float det = plugin.detectedMidi.load();
            if (det >= 0.0f) lastDetected = det, holdFrames = 12;
            else if (holdFrames > 0) --holdFrames;
            level = juce::jmax (plugin.inputLevel.load(), level * 0.9f);
            plugin.copyScope (scope);
            phase += 0.05f;
            repaint();
        }

        void stepPreset (int delta)
        {
            const int n = (int) plugin.presets.size();
            plugin.setCurrentProgram ((plugin.getCurrentProgram() + delta + n) % n);
        }

        void showPresetMenu()
        {
            juce::PopupMenu m;
            for (int i = 0; i < (int) plugin.presets.size(); ++i)
                m.addItem (1 + i, plugin.presets[(size_t) i].name, true, plugin.presets[(size_t) i].name == plugin.currentPresetName);
            m.addSeparator();
            m.addItem (900, "Next preset");
            juce::Component::SafePointer<TuneEditor> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuBtn), [safe] (int r)
            {
                if (safe == nullptr || r == 0) return;
                if (r == 900) safe->stepPreset (1);
                else safe->plugin.setCurrentProgram (r - 1);
            });
        }

        void showSettingsMenu()
        {
            juce::PopupMenu m, ref;
            for (float hz : { 432.0f, 436.0f, 438.0f, 440.0f, 442.0f, 444.0f })
                ref.addItem ((int) hz, juce::String (hz, 0) + " Hz", true, std::abs (plugin.get ("ref") - hz) < 0.05f);
            m.addSubMenu ("Reference pitch (A4)", ref);
            m.addItem (1, "Reset everything");
            juce::Component::SafePointer<TuneEditor> safe (this);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&gearBtn), [safe] (int r)
            {
                if (safe == nullptr || r == 0) return;
                if (r == 1) safe->plugin.applyPreset ({ "Default", {} });
                else        safe->plugin.setParam ("ref", (float) r);
            });
        }

        void toggleLearn()
        {
            if (plugin.getBool ("lock"))
            {
                learnBtn.setToggleState (false, juce::dontSendNotification);
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Scale lock is on",
                                                        "Turn Scale lock off to let Learn change the key.");
                return;
            }
            if (plugin.learning.load())
            {
                finishLearn();
                return;
            }
            for (auto& s : plugin.scoreHist) s.store (0.0f);
            plugin.learnedSeconds = 0.0f;
            plugin.learning = true;
        }

        void finishLearn()
        {
            plugin.learning = false;
            if (plugin.learnedSeconds.load() < 1.0f)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Learn",
                                                        "AMETHYST TUNE didn't hear enough singing. Press Learn and sing for a few seconds.");
                return;
            }
            // Krumhansl-Schmuckler key finding
            static constexpr float major[12] = { 6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f };
            static constexpr float minor[12] = { 6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f };
            float hist[12];
            for (int i = 0; i < 12; ++i) hist[i] = plugin.scoreHist[(size_t) i].load();
            int bestKey = 0, bestScale = 1;
            float bestScore = -1.0e9f;
            for (int key = 0; key < 12; ++key)
                for (int s = 0; s < 2; ++s)
                {
                    float score = 0.0f;
                    for (int i = 0; i < 12; ++i)
                        score += hist[(i + key) % 12] * (s == 0 ? major[i] : minor[i]);
                    if (score > bestScore) { bestScore = score; bestKey = key; bestScale = s == 0 ? 1 : 2; }
                }
            plugin.undoManager.beginNewTransaction();
            plugin.setParam ("key", (float) bestKey);
            plugin.setParam ("scale", (float) bestScale);
        }

        void handlePianoClick (juce::Point<int> p)
        {
            const int note = pianoNoteAt (p);
            if (note < 0)
                return;
            if (plugin.getBool ("lock"))
                return;
            int mask = plugin.effectiveMask();
            mask ^= 1 << note;
            if (mask == 0)
                return; // keep at least one note
            plugin.setParam ("mask", (float) mask);
            plugin.setParam ("scale", (float) kCustomScale);
        }

        //------------------------------------------------------------------
        // piano geometry: 7 white keys, 5 black keys
        static constexpr int whiteNotes[7] = { 0, 2, 4, 5, 7, 9, 11 };
        static constexpr int blackNotes[5] = { 1, 3, 6, 8, 10 };
        static constexpr float blackPos[5] = { 1.0f, 2.0f, 4.0f, 5.0f, 6.0f }; // centred on the boundary after white key n

        juce::Rectangle<float> whiteKey (int i) const
        {
            const auto a = pianoArea.toFloat().withTrimmedTop (24.0f);
            const float w = a.getWidth() / 7.0f;
            return { a.getX() + w * (float) i + 2.0f, a.getY(), w - 4.0f, a.getHeight() };
        }

        juce::Rectangle<float> blackKey (int i) const
        {
            const auto a = pianoArea.toFloat().withTrimmedTop (24.0f);
            const float w = a.getWidth() / 7.0f;
            return juce::Rectangle<float> (w * 0.62f, a.getHeight() * 0.6f).withCentre ({ a.getX() + w * blackPos[i], a.getY() + a.getHeight() * 0.3f });
        }

        int pianoNoteAt (juce::Point<int> p) const
        {
            for (int i = 0; i < 5; ++i)
                if (blackKey (i).contains (p.toFloat())) return blackNotes[i];
            for (int i = 0; i < 7; ++i)
                if (whiteKey (i).contains (p.toFloat())) return whiteNotes[i];
            return -1;
        }

        //------------------------------------------------------------------
        void drawStatic (juce::Graphics& g)
        {
            g.setColour (Theme::text);
            g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            g.drawText ("KEY", keyCard.withHeight (34), juce::Justification::centred);

            auto cardTitle = [&g] (juce::Rectangle<int> card, const char* title, const char* left, const char* right)
            {
                g.setColour (Theme::text.withAlpha (0.85f));
                g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
                g.drawText (title, card.withHeight (40), juce::Justification::centred);
                g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
                g.drawText (left, card.withTrimmedTop (card.getHeight() - 34).withWidth (card.getWidth() / 2), juce::Justification::centred);
                g.drawText (right, card.withTrimmedTop (card.getHeight() - 34).withTrimmedLeft (card.getWidth() / 2), juce::Justification::centred);
            };
            cardTitle (retuneCard, "RETUNE SPEED", "FAST", "SLOW");
            cardTitle (humanizeCard, "HUMANIZE", "LESS", "MORE");

            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText ("INPUT TYPE", footerArea.reduced (40, 0).withWidth (110), juce::Justification::centredLeft);
        }

        void drawHeaderTitle (juce::Graphics& g)
        {
            auto h = headerArea;
            auto orb = h.removeFromLeft (52).toFloat().reduced (6.0f);
            g.setGradientFill (juce::ColourGradient (Theme::accent2, orb.getX(), orb.getY(), juce::Colour (0xff4b2fd0), orb.getRight(), orb.getBottom(), false));
            g.fillEllipse (orb);
            juce::Path w;
            for (int i = 0; i <= 20; ++i)
            {
                const float x = orb.getX() + orb.getWidth() * (0.15f + 0.7f * (float) i / 20.0f);
                const float y = orb.getCentreY() + std::sin ((float) i * 1.3f + phase) * orb.getHeight() * 0.12f * (i % 3 == 0 ? 1.6f : 0.7f);
                if (i == 0) w.startNewSubPath (x, y); else w.lineTo (x, y);
            }
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.strokePath (w, juce::PathStrokeType (1.3f));

            h.removeFromLeft (10);
            g.setFont (juce::FontOptions (22.0f, juce::Font::plain));
            g.setColour (Theme::text);
            const juce::String a ("AMETHYST "), b ("TUNE");
            const int wa = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), a);
            g.drawText (a, h, juce::Justification::centredLeft);
            g.setColour (Theme::accent);
            g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
            g.drawText (b, h.withTrimmedLeft (wa), juce::Justification::centredLeft);

            // preset pill
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRoundedRectangle (presetArea.toFloat(), 18.0f);
            g.setColour (juce::Colours::white.withAlpha (0.1f));
            g.drawRoundedRectangle (presetArea.toFloat().reduced (0.5f), 18.0f, 1.0f);
        }

        void drawDial (juce::Graphics& g, juce::Rectangle<float> area)
        {
            const float d = juce::jmin (area.getWidth(), area.getHeight() - 10.0f);
            const auto c = juce::Point<float> (area.getCentreX(), area.getY() + d * 0.5f + 4.0f);
            const float R = d * 0.5f;

            // outer ring + ticks
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawEllipse (juce::Rectangle<float> (R * 2.0f, R * 2.0f).withCentre (c), 1.5f);
            for (int i = 0; i < 72; ++i)
            {
                const float a = juce::MathConstants<float>::twoPi * (float) i / 72.0f;
                if (std::abs (a - juce::MathConstants<float>::pi) < 0.55f)
                    continue; // gap at the bottom for the note label
                const bool major = i % 6 == 0;
                g.setColour (juce::Colours::white.withAlpha (major ? 0.35f : 0.16f));
                g.drawLine ({ c.getPointOnCircumference (R * (major ? 0.82f : 0.86f), a), c.getPointOnCircumference (R * 0.92f, a) }, major ? 2.0f : 1.2f);
            }

            // correction needle on the outer ring (+-100 cents = +-120 degrees)
            const float needleA = juce::jlimit (-2.1f, 2.1f, shownCents / 100.0f * 2.1f);
            g.setColour (Theme::accent2);
            g.drawLine ({ c.getPointOnCircumference (R * 0.78f, needleA), c.getPointOnCircumference (R * 0.95f, needleA) }, 3.0f);

            // glowing correction arc
            const float arcR = R * 0.64f;
            juce::Path track, arc;
            track.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, -2.4f, 2.4f, true);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.strokePath (track, juce::PathStrokeType (8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            const float amount = juce::jlimit (0.0f, 1.0f, 0.15f + std::abs (shownCents) / 60.0f + level * 0.6f);
            arc.addCentredArc (c.x, c.y, arcR, arcR, 0.0f, -2.4f, -2.4f + 4.8f * amount, true);
            g.setColour (Theme::accent.withAlpha (0.35f));
            g.strokePath (arc, juce::PathStrokeType (16.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4b5cff), c.x - arcR, c.y, Theme::accent, c.x + arcR, c.y, false));
            g.strokePath (arc, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // glass orb with the live waveform
            const float orbR = R * 0.48f;
            const auto orb = juce::Rectangle<float> (orbR * 2.0f, orbR * 2.0f).withCentre (c);
            g.setColour (Theme::accent.withAlpha (0.25f + level * 0.4f));
            g.fillEllipse (orb.expanded (8.0f));
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff6a4fe8), c.x, orb.getY(), juce::Colour (0xff1d1250), c.x, orb.getBottom(), false));
            g.fillEllipse (orb);
            {
                juce::Graphics::ScopedSaveState s (g);
                juce::Path clip;
                clip.addEllipse (orb);
                g.reduceClipRegion (clip);
                juce::Path wave;
                for (size_t i = 0; i < scope.size(); ++i)
                {
                    const float x = orb.getX() + orb.getWidth() * (float) i / (float) (scope.size() - 1);
                    const float y = c.y - juce::jlimit (-1.0f, 1.0f, scope[i] * 3.0f) * orbR * 0.45f;
                    if (i == 0) wave.startNewSubPath (x, y); else wave.lineTo (x, y);
                }
                g.setColour (Theme::accent2.withAlpha (0.5f));
                g.strokePath (wave, juce::PathStrokeType (3.0f));
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.strokePath (wave, juce::PathStrokeType (1.2f));
                g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.28f), c.x, orb.getY(), juce::Colours::white.withAlpha (0.0f), c.x, c.y, false));
                g.fillEllipse (orb.reduced (orbR * 0.15f).withHeight (orbR * 0.9f));
            }
            g.setColour (Theme::accent2.withAlpha (0.8f));
            g.drawEllipse (orb, 2.0f);

            // note readout: the note being sung (or the key when silent) + scale
            const bool singing = holdFrames > 0;
            const int target = singing ? (int) std::lround (plugin.targetMidi.load()) : -1;
            const juce::String note = target >= 0 ? kNoteNames[target % 12] : kNoteNames[plugin.getInt ("key")];
            g.setColour (Theme::accent);
            g.setFont (juce::FontOptions (40.0f, juce::Font::bold));
            g.drawText (note, juce::Rectangle<float> (120.0f, 46.0f).withCentre ({ c.x, c.y + R * 0.83f }).toNearestInt(), juce::Justification::centred);
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            const juce::String sub = singing ? (shownCents >= 0 ? "+" : "") + juce::String (juce::roundToInt (shownCents)) + " CENTS"
                                             : scaleLabel().toUpperCase();
            g.drawText (sub, juce::Rectangle<float> (200.0f, 20.0f).withCentre ({ c.x, c.y + R * 0.83f + 30.0f }).toNearestInt(), juce::Justification::centred);
        }

        juce::String scaleLabel() const
        {
            const int s = plugin.getInt ("scale");
            return s == 1 ? "Major" : s == 2 ? "Minor" : kScales[s];
        }

        void drawKnobValues (juce::Graphics& g)
        {
            auto value = [&g] (juce::Rectangle<int> knob, const juce::String& v, const juce::String& unit)
            {
                const auto c = knob.getCentre().toFloat();
                g.setColour (juce::Colour (0xff0e0a18));
                g.fillEllipse (juce::Rectangle<float> (70.0f, 70.0f).withCentre (c));
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.drawEllipse (juce::Rectangle<float> (70.0f, 70.0f).withCentre (c), 1.0f);
                g.setColour (Theme::text);
                g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
                g.drawText (v, juce::Rectangle<float> (80.0f, 30.0f).withCentre (c.translated (0.0f, -6.0f)).toNearestInt(), juce::Justification::centred);
                g.setColour (Theme::textDim);
                g.setFont (juce::FontOptions (12.0f));
                g.drawText (unit, juce::Rectangle<float> (80.0f, 16.0f).withCentre (c.translated (0.0f, 16.0f)).toNearestInt(), juce::Justification::centred);
            };
            value (retuneKnob.getBounds(), juce::String (juce::roundToInt (plugin.get ("retune"))), "ms");
            value (humanizeKnob.getBounds(), juce::String (juce::roundToInt (plugin.get ("humanize"))), "%");
        }

        void drawBarLabels (juce::Graphics& g)
        {
            if (barCells.size() < 5)
                return;
            static const char* labels[5] = { "SCALE", "SCALE LOCK", "TRACKING", "FORMANT", "THROAT" };
            g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
            for (int i = 0; i < 5; ++i)
            {
                g.setColour (Theme::text.withAlpha (0.8f));
                auto lab = barCells[(size_t) i].withHeight (22);
                if (i == 2)
                {
                    g.drawText (labels[i], lab.withTrimmedRight (lab.getWidth() / 2 + 4), juce::Justification::centredRight);
                    g.setColour (Theme::accent);
                    g.drawText (juce::String (juce::roundToInt (plugin.get ("tracking"))) + "%", lab.withTrimmedLeft (lab.getWidth() / 2 + 4), juce::Justification::centredLeft);
                }
                else
                    g.drawText (labels[i], lab, juce::Justification::centred);
            }
        }

        void drawPiano (juce::Graphics& g)
        {
            const int mask = plugin.effectiveMask();
            const bool singing = holdFrames > 0;
            const int sung = singing ? (int) std::lround (plugin.targetMidi.load()) % 12 : -1;

            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            for (int i = 0; i < 7; ++i)
            {
                const auto k = whiteKey (i);
                const int note = whiteNotes[i];
                const bool inScale = (mask >> note) & 1;
                g.setGradientFill (juce::ColourGradient (inScale ? juce::Colour (0xffcbbcff) : juce::Colour (0xffd7d5de), k.getX(), k.getY(),
                                                         inScale ? juce::Colour (0xff8d6cf2) : juce::Colour (0xffa9a6b6), k.getX(), k.getBottom(), false));
                g.fillRoundedRectangle (k, 5.0f);
                if (note == sung)
                {
                    g.setColour (Theme::accent2.withAlpha (0.6f));
                    g.fillRoundedRectangle (k.withTrimmedTop (k.getHeight() * 0.6f), 5.0f);
                }
                g.setColour (Theme::text.withAlpha (0.85f));
                g.drawText (kNoteNames[note], k.withY ((float) pianoArea.getY()).withHeight (22.0f).toNearestInt(), juce::Justification::centred);
            }
            for (int i = 0; i < 5; ++i)
            {
                const auto k = blackKey (i);
                const int note = blackNotes[i];
                const bool inScale = (mask >> note) & 1;
                if (inScale)
                {
                    g.setColour (Theme::accent.withAlpha (0.45f));
                    g.fillRoundedRectangle (k.expanded (3.0f), 7.0f);
                }
                g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1c1726), k.getX(), k.getY(), juce::Colour (0xff07050c), k.getX(), k.getBottom(), false));
                g.fillRoundedRectangle (k, 5.0f);
                if (note == sung)
                {
                    g.setColour (Theme::accent2.withAlpha (0.7f));
                    g.fillRoundedRectangle (k.withTrimmedTop (k.getHeight() * 0.65f).reduced (3.0f, 0.0f), 4.0f);
                }
                g.setColour (Theme::text.withAlpha (inScale ? 0.95f : 0.5f));
                g.drawText (kNoteNames[note], k.withHeight (22.0f).toNearestInt(), juce::Justification::centred);
            }
        }

        void drawKeyInfo (juce::Graphics& g)
        {
            Glass::well (g, keyInfo.toFloat(), 12.0f);
            g.setColour (Theme::accent);
            g.setFont (juce::FontOptions (54.0f, juce::Font::bold));
            g.drawText (kNoteNames[plugin.getInt ("key")], keyInfo.withHeight (keyInfo.getHeight() - 70).translated (0, 10), juce::Justification::centred);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (scaleLabel().toUpperCase(), keyInfo.withTrimmedTop (keyInfo.getHeight() - 70).withHeight (24), juce::Justification::centred);
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.drawHorizontalLine (keyInfo.getBottom() - 44, (float) keyInfo.getX() + 10.0f, (float) keyInfo.getRight() - 10.0f);
        }

        void drawFooter (juce::Graphics& g)
        {
            auto f = footerArea.reduced (40, 0);
            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            const auto mixArea = f.withTrimmedLeft (f.getWidth() - 320).withWidth (110);
            g.drawText ("MIX", mixArea.withWidth (40), juce::Justification::centredLeft);
            g.setColour (Theme::accent);
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText (juce::String (juce::roundToInt (plugin.get ("mix"))) + "%", mixArea.withTrimmedLeft (44), juce::Justification::centredLeft);
        }

        //------------------------------------------------------------------
        TunePlugin& plugin;
        HeaderIcon prevBtn { HeaderIcon::Kind::Left }, menuBtn { HeaderIcon::Kind::Down }, undoBtn { HeaderIcon::Kind::Undo },
                   redoBtn { HeaderIcon::Kind::Redo }, gearBtn { HeaderIcon::Kind::Gear }, powerBtn { HeaderIcon::Kind::Power },
                   keyDown { HeaderIcon::Kind::Left }, keyUp { HeaderIcon::Kind::Right };
        juce::Label presetLabel;
        juce::OwnedArray<juce::TextButton> keyBtns;
        juce::TextButton learnBtn { "LEARN" };
        ValueSlider retuneKnob { "Retune speed" }, humanizeKnob { "Humanize" }, trackingSlider { "Tracking" }, mixSlider { "Mix" };
        juce::ComboBox scaleBox, formantBox, inputBox;
        ToggleSwitch lockSwitch, throatSwitch;
        std::unique_ptr<SA> retuneAtt, humanizeAtt, trackingAtt, mixAtt;
        std::unique_ptr<CA> scaleAtt, formantAtt, inputAtt;
        std::unique_ptr<BA> lockAtt, throatAtt;

        juce::Rectangle<int> headerArea, presetArea, keyCard, dialArea, retuneCard, humanizeCard, barArea, pianoBox, keyInfo, pianoArea, footerArea;
        std::vector<juce::Rectangle<int>> barCells;
        juce::Image bg;
        std::vector<float> scope;
        float shownCents = 0.0f, lastDetected = -1.0f, level = 0.0f, phase = 0.0f;
        int holdFrames = 0;
    };

    juce::AudioProcessorEditor* TunePlugin::createEditor() { return new TuneEditor (*this); }
}

std::unique_ptr<AmethystPlugin> Builtins::makeTune() { return std::make_unique<TunePlugin>(); }

// Test hook used by --selftest: run TUNE on a slightly flat sine and report the output pitch.
double amethystTuneTest (double inputHz, int key, int scale, double retuneMs)
{
    auto plug = Builtins::makeTune();
    const double sr = 48000.0;
    plug->setRateAndBufferSizeDetails (sr, 512);
    plug->prepareToPlay (sr, 512);
    plug->setParam ("key", (float) key);
    plug->setParam ("scale", (float) scale);
    plug->setParam ("retune", (float) retuneMs);
    plug->setParam ("humanize", 0.0f);

    juce::AudioBuffer<float> block (2, 512);
    juce::MidiBuffer midi;
    std::vector<float> out;
    double phase = 0.0;
    for (int b = 0; b < 200; ++b) // ~2 s
    {
        for (int i = 0; i < 512; ++i)
        {
            const float v = 0.4f * (float) std::sin (phase) + 0.12f * (float) std::sin (2.0 * phase);
            phase += juce::MathConstants<double>::twoPi * inputHz / sr;
            block.setSample (0, i, v);
            block.setSample (1, i, v);
        }
        plug->processBlock (block, midi);
        if (b >= 120)
            for (int i = 0; i < 512; ++i)
                out.push_back (block.getSample (0, i));
    }

    // measure the output pitch by counting positive-going zero crossings (fundamental dominates)
    int crossings = 0;
    int first = -1, last = -1;
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i - 1] <= 0.0f && out[i] > 0.0f)
        {
            if (first < 0) first = (int) i;
            last = (int) i;
            ++crossings;
        }
    if (crossings < 3)
        return 0.0;
    return (crossings - 1) * sr / (double) (last - first);
}
