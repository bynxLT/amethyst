#pragma once

#include <JuceHeader.h>

// Discord Rich Presence over Discord's local IPC (no SDK). Runs on its own thread:
// connects to the Discord desktop app when it's running, keeps the activity up to date,
// reconnects if Discord restarts, and clears the status when turned off or on exit.
class DiscordPresence : private juce::Thread
{
public:
    DiscordPresence();
    ~DiscordPresence() override;

    struct Activity
    {
        juce::String details, state;           // the two text lines
        juce::String smallImage, smallText;    // e.g. "ultra" / "Ultra Pro"
        juce::String plan;                     // "Ultra Pro" / "Pro" - shown in the text (empty = don't show)
        juce::String edition;                  // "Anime Edition" (empty = the normal app)
        bool operator== (const Activity& o) const { return details == o.details && state == o.state && smallImage == o.smallImage && smallText == o.smallText && plan == o.plan && edition == o.edition; }
    };

    void setApplicationId (const juce::String& id);
    void setEnabled (bool);
    void setActivity (const Activity&);          // cheap; sent only when it changes
    bool isConnected() const noexcept { return connected.load(); }

private:
    void run() override;
    bool sendFrame (int opcode, const juce::var& payload);
    void drain();
    void disconnect();

    juce::CriticalSection lock;
    juce::String appId;
    Activity activity;
    bool dirty = true;
    std::atomic<bool> enabled { false }, connected { false };
    void* pipe = nullptr;
    juce::int64 sessionStart = 0;
};
