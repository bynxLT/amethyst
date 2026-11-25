#include "DiscordPresence.h"

void* amethystIpcConnect();
bool amethystIpcWrite (void* handle, const void* data, std::size_t size);
int amethystIpcRead (void* handle, void* buffer, std::size_t size);
void amethystIpcClose (void* handle);
int amethystProcessId();

DiscordPresence::DiscordPresence() : juce::Thread ("AMETHYST Discord")
{
    sessionStart = juce::Time::currentTimeMillis() / 1000;
    startThread (juce::Thread::Priority::low);
}

DiscordPresence::~DiscordPresence()
{
    enabled = false;
    notify();
    stopThread (3000);
    disconnect();
}

void DiscordPresence::setApplicationId (const juce::String& id)
{
    const juce::ScopedLock sl (lock);
    if (id.trim() != appId)
    {
        appId = id.trim();
        dirty = true;
        if (pipe != nullptr) connected = false;   // the thread reconnects with the new id
    }
    notify();
}

void DiscordPresence::setEnabled (bool on)
{
    enabled = on;
    {
        const juce::ScopedLock sl (lock);
        dirty = true;
    }
    notify();
}

void DiscordPresence::setActivity (const Activity& a)
{
    const juce::ScopedLock sl (lock);
    if (a == activity)
        return;
    activity = a;
    dirty = true;
    notify();
}

// Frame: little-endian u32 opcode, u32 length, then UTF-8 JSON.
bool DiscordPresence::sendFrame (int opcode, const juce::var& payload)
{
    const auto json = juce::JSON::toString (payload, true);
    const auto utf8 = json.toUTF8();
    const auto len = (juce::uint32) std::strlen (utf8.getAddress());
    juce::MemoryOutputStream out;
    out.writeInt (opcode);
    out.writeInt ((int) len);
    out.write (utf8.getAddress(), len);
    return amethystIpcWrite (pipe, out.getData(), out.getDataSize());
}

void DiscordPresence::drain()
{
    char buf[4096];
    for (int guard = 0; guard < 16; ++guard)
    {
        const int n = amethystIpcRead (pipe, buf, sizeof (buf));
        if (n < 0) { disconnect(); return; }
        if (n == 0) return;
    }
}

void DiscordPresence::disconnect()
{
    if (pipe != nullptr)
        amethystIpcClose (pipe);
    pipe = nullptr;
    connected = false;
}

void DiscordPresence::run()
{
    juce::int64 nextConnectTry = 0;
    bool shownSomething = false;

    while (! threadShouldExit())
    {
        juce::String id;
        Activity a;
        bool needSend = false;
        {
            const juce::ScopedLock sl (lock);
            id = appId;
            a = activity;
            needSend = dirty;
        }

        // turned off (or no id): clear what we showed, then let go of the connection
        if (! enabled.load() || id.isEmpty())
        {
            if (pipe != nullptr && shownSomething)
            {
                auto args = new juce::DynamicObject();
                args->setProperty ("pid", amethystProcessId());
                auto cmd = new juce::DynamicObject();
                cmd->setProperty ("cmd", "SET_ACTIVITY");
                cmd->setProperty ("args", juce::var (args));
                cmd->setProperty ("nonce", juce::Uuid().toString());
                sendFrame (1, juce::var (cmd));
                shownSomething = false;
            }
            disconnect();
            wait (1000);
            continue;
        }

        if (pipe != nullptr && ! connected.load())
            disconnect();   // id changed

        if (pipe == nullptr)
        {
            const auto now = juce::Time::currentTimeMillis();
            if (now < nextConnectTry) { wait (500); continue; }
            nextConnectTry = now + 10000;   // Discord not running: try again in 10 s
            pipe = amethystIpcConnect();
            if (pipe == nullptr) continue;

            auto hello = new juce::DynamicObject();
            hello->setProperty ("v", 1);
            hello->setProperty ("client_id", id);
            if (! sendFrame (0, juce::var (hello))) { disconnect(); continue; }

            // wait briefly for Discord's READY reply
            char buf[4096];
            bool ready = false;
            for (int k = 0; k < 30 && ! threadShouldExit(); ++k)
            {
                const int n = amethystIpcRead (pipe, buf, sizeof (buf));
                if (n < 0) break;
                if (n > 0 && juce::String::fromUTF8 (buf + 8, juce::jmax (0, n - 8)).contains ("READY")) { ready = true; break; }
                wait (100);
            }
            if (! ready) { disconnect(); continue; }
            connected = true;
            needSend = true;
        }

        if (needSend)
        {
            auto assets = new juce::DynamicObject();
            assets->setProperty ("large_image", "amethyst");
            // hovering the big logo shows the plan too, even without uploaded badge art
            juce::String title ("AMETHYST 2026");
            if (a.edition.isNotEmpty()) title << " " << a.edition;
            if (a.plan.isNotEmpty())    title << " " << a.plan;
            assets->setProperty ("large_text", title);
            if (a.smallImage.isNotEmpty())
            {
                assets->setProperty ("small_image", a.smallImage);
                assets->setProperty ("small_text", a.smallText);
            }
            auto stamps = new juce::DynamicObject();
            stamps->setProperty ("start", sessionStart);

            auto act = new juce::DynamicObject();
            if (a.details.isNotEmpty()) act->setProperty ("details", a.details);
            if (a.state.isNotEmpty())   act->setProperty ("state", a.state);
            act->setProperty ("timestamps", juce::var (stamps));
            act->setProperty ("assets", juce::var (assets));

            auto args = new juce::DynamicObject();
            args->setProperty ("pid", amethystProcessId());
            args->setProperty ("activity", juce::var (act));

            auto cmd = new juce::DynamicObject();
            cmd->setProperty ("cmd", "SET_ACTIVITY");
            cmd->setProperty ("args", juce::var (args));
            cmd->setProperty ("nonce", juce::Uuid().toString());

            if (sendFrame (1, juce::var (cmd)))
            {
                const juce::ScopedLock sl (lock);
                if (a == activity) dirty = false;
                shownSomething = true;
            }
            else
                disconnect();
        }

        drain();
        wait (1000);
    }

    if (pipe != nullptr && shownSomething)
    {
        // leaving: clear the status
        auto args = new juce::DynamicObject();
        args->setProperty ("pid", amethystProcessId());
        auto cmd = new juce::DynamicObject();
        cmd->setProperty ("cmd", "SET_ACTIVITY");
        cmd->setProperty ("args", juce::var (args));
        cmd->setProperty ("nonce", juce::Uuid().toString());
        sendFrame (1, juce::var (cmd));
    }
}
