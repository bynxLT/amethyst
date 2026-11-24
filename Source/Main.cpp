#include <JuceHeader.h>
#include "MainComponent.h"
#include "GlassLookAndFeel.h"
#include "Plugins/Builtins.h"
#include "Account.h"

bool amethystProtect (const void* data, std::size_t size, std::vector<unsigned char>& out, bool encrypt);

class VocalDAWApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "AMETHYST"; }
    const juce::String getApplicationVersion() override { return "0.2.0"; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    // Headless check of the clip effect renderer:  VocalDAW.exe --selftest
    static int runSelfTest()
    {
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

            if (fails > 0) { std::printf ("clip editing test FAILED\n"); return 1; }
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

        std::printf ("selftest OK\n");
        return 0;
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

        if (commandLine.contains ("--selftest"))
        {
            setApplicationReturnValue (runSelfTest());
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
        explicit MainWindow (const juce::String& name)
            : juce::DocumentWindow (name, Theme::windowBg, juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            setResizable (true, true);
            setResizeLimits (1200, 520, 10000, 10000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
            FloatAboveMain::mainWindow() = this; // plug-in / clip windows float above this one
        }

        ~MainWindow() override
        {
            FloatAboveMain::mainWindow() = nullptr;
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

private:
    GlassLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<juce::AudioPluginInstance> previewPlugin;
    std::unique_ptr<juce::DocumentWindow> previewWindow;
};

START_JUCE_APPLICATION (VocalDAWApplication)
