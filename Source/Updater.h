#pragma once

#include <JuceHeader.h>

// Auto-updates (Windows).
// Reads a small JSON file from the public "releases" bucket of the project's Supabase storage:
//   { "version": "1.0.1", "url": "https://<project>.supabase.co/storage/v1/object/public/releases/AMETHYST-2026-Setup-1.0.1.exe",
//     "sha256": "<hex>", "notes": "What's new..." }
// If it's newer than this build, offers to update: downloads the installer (with progress), checks
// its SHA-256, runs it silently and closes AMETHYST. The installer starts AMETHYST again.
class Updater : private juce::Thread
{
public:
    struct Release
    {
        juce::String version, url, sha256, notes;
    };

    explicit Updater (juce::String supabaseUrl);
    ~Updater() override;

    bool isAvailable() const;   // configured, and on a platform we can update
    // Check in the background. onResult runs on the message thread: (found newer?, release, error)
    void check (std::function<void (bool, Release, juce::String)> onResult);

    // Download + verify + launch the installer, with a progress window. done(ok, error) runs on the message
    // thread; when ok, the installer is running and the app should quit.
    static void downloadAndInstall (const Release&, const juce::String& trustedPrefix, std::function<void (bool, juce::String)> done);

    static int compareVersions (const juce::String& a, const juce::String& b);   // <0, 0, >0
    juce::String getManifestUrl() const;
    juce::String getTrustedPrefix() const;   // installer URLs must start with this

private:
    void run() override;
    juce::String base;
    std::function<void (bool, Release, juce::String)> pending;
};
