#include "Updater.h"

Updater::Updater (juce::String supabaseUrl) : juce::Thread ("AMETHYST update check"), base (supabaseUrl.trimCharactersAtEnd ("/")) {}

Updater::~Updater() { stopThread (4000); }

bool Updater::isAvailable() const
{
   #if JUCE_WINDOWS
    return base.startsWithIgnoreCase ("https://");
   #else
    return false;
   #endif
}

juce::String Updater::getManifestUrl() const   { return base + "/storage/v1/object/public/releases/latest.json"; }
juce::String Updater::getTrustedPrefix() const { return base + "/storage/v1/object/public/releases/"; }

int Updater::compareVersions (const juce::String& a, const juce::String& b)
{
    auto pa = juce::StringArray::fromTokens (a.trim(), ".", {}), pb = juce::StringArray::fromTokens (b.trim(), ".", {});
    for (int i = 0; i < juce::jmax (pa.size(), pb.size()); ++i)
    {
        const int x = pa[i].getIntValue(), y = pb[i].getIntValue();
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

void Updater::check (std::function<void (bool, Release, juce::String)> onResult)
{
    if (isThreadRunning() || ! isAvailable())
        return;
    pending = std::move (onResult);
    startThread (juce::Thread::Priority::low);
}

void Updater::run()
{
    Release rel;
    juce::String error;
    bool newer = false;

    // cache-busting query so a CDN never hands us an old manifest
    juce::URL url (getManifestUrl() + "?t=" + juce::String (juce::Time::currentTimeMillis()));
    int status = 0;
    auto stream = url.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                             .withConnectionTimeoutMs (8000)
                                             .withStatusCode (&status));
    if (stream == nullptr)
        error = "Couldn't reach the update server.";
    else if (status == 404 || status == 400)
        error = "No releases published yet.";
    else
    {
        const auto json = juce::JSON::parse (stream->readEntireStreamAsString());
        rel.version = json.getProperty ("version", {}).toString();
        rel.url = json.getProperty ("url", {}).toString();
        rel.sha256 = json.getProperty ("sha256", {}).toString().toLowerCase();
        rel.notes = json.getProperty ("notes", {}).toString();
        if (rel.version.isEmpty() || rel.url.isEmpty())
            error = "The update information was incomplete.";
        else
            newer = compareVersions (rel.version, ProjectInfo::versionString) > 0;
    }

    auto cb = pending;
    juce::MessageManager::callAsync ([cb, newer, rel, error] { if (cb) cb (newer, rel, error); });
}

namespace
{
    // Downloads with progress, verifies the hash, starts the installer. Deletes itself when done.
    class DownloadTask : public juce::ThreadWithProgressWindow
    {
    public:
        DownloadTask (const Updater::Release& r, juce::File dest, std::function<void (bool, juce::String)> cb)
            : juce::ThreadWithProgressWindow ("Updating AMETHYST 2026", true, true, 10000, "Cancel"),
              rel (r), file (std::move (dest)), done (std::move (cb)) {}

        void run() override
        {
            setStatusMessage ("Downloading version " + rel.version + "...");
            int status = 0;
            auto in = juce::URL (rel.url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                               .withConnectionTimeoutMs (15000).withStatusCode (&status));
            if (in == nullptr || status >= 400) { error = "The download failed."; return; }
            file.deleteFile();
            {
                juce::FileOutputStream out (file);
                if (! out.openedOk()) { error = "Couldn't save the installer."; return; }
                const auto total = in->getTotalLength();
                juce::HeapBlock<char> buf (65536);
                juce::int64 got = 0;
                while (! in->isExhausted() && ! threadShouldExit())
                {
                    const int n = in->read (buf, 65536);
                    if (n <= 0) break;
                    out.write (buf, (size_t) n);
                    got += n;
                    if (total > 0) setProgress ((double) got / (double) total);
                }
                out.flush();
                if (threadShouldExit()) { error = "Update cancelled."; return; }
                if (total > 0 && got != total) { error = "The download was incomplete."; return; }
            }
            setStatusMessage ("Checking the download...");
            setProgress (-1.0);
            if (rel.sha256.isNotEmpty())
            {
                juce::FileInputStream fin (file);
                const auto hash = juce::SHA256 (fin).toHexString().toLowerCase();
                if (hash != rel.sha256) { error = "The download didn't match the published checksum, so it wasn't installed."; return; }
            }
            ok = true;
        }

        void threadComplete (bool cancelled) override
        {
            if (cancelled && error.isEmpty()) error = "Update cancelled.";
            if (ok && ! cancelled)
            {
                // silent install with a progress bar; the installer closes AMETHYST and starts it again
                ok = file.startAsProcess ("/SILENT /SUPPRESSMSGBOXES /NORESTART /CLOSEAPPLICATIONS");
                if (! ok) error = "Couldn't start the installer.";
            }
            else
                file.deleteFile();
            if (done) done (ok && ! cancelled, error);
            delete this;
        }

        const Updater::Release rel;
        const juce::File file;
        std::function<void (bool, juce::String)> done;
        juce::String error;
        bool ok = false;
    };
}

void Updater::downloadAndInstall (const Release& rel, const juce::String& trustedPrefix, std::function<void (bool, juce::String)> done)
{
    // only ever run installers from our own release storage, over https
    if (! rel.url.startsWith (trustedPrefix) || ! rel.url.endsWithIgnoreCase (".exe"))
    {
        if (done) done (false, "The update link isn't from AMETHYST's release server, so it was ignored.");
        return;
    }
    const auto dest = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("AMETHYST-2026-update.exe");
    (new DownloadTask (rel, dest, std::move (done)))->launchThread();
}
