#pragma once

#include <JuceHeader.h>
#include "License.h"

//==============================================================================
// Minimal, synchronous Supabase Auth + REST client. Call from a background thread.
class SupabaseApi
{
public:
    struct Session
    {
        bool ok = false;
        bool networkError = false;
        bool needsConfirmation = false;   // sign-up succeeded but the email must be confirmed first
        juce::String error;
        juce::String accessToken, refreshToken, userId, email;
        int expiresIn = 0;
    };

    struct Entitlement
    {
        bool ok = false;
        bool networkError = false;
        juce::String error;
        bool isPro = false;               // already accounts for pro_until (Ultra counts as Pro)
        bool isUltra = false;             // Ultra Pro tier
        juce::String plan;
        juce::Time proUntil;              // zero = no expiry
    };

    SupabaseApi (juce::String projectUrl, juce::String anonKey);

    bool isConfigured() const { return url.isNotEmpty() && key.isNotEmpty(); }

    Session signIn (const juce::String& email, const juce::String& password) const;
    Session signUp (const juce::String& email, const juce::String& password) const;
    Session refresh (const juce::String& refreshToken) const;
    Entitlement fetchEntitlement (const juce::String& accessToken, const juce::String& userId) const;

    // Signed license from the "license" Edge Function. notDeployed = the function isn't set up yet (404).
    struct LicenseResult { bool ok = false, notDeployed = false, networkError = false; juce::String error, token; };
    LicenseResult fetchLicense (const juce::String& accessToken) const;
    void signOut (const juce::String& accessToken) const;
    juce::String sendPasswordReset (const juce::String& email) const;   // empty = success

    // Profile picture in Supabase Storage (bucket "avatars", path <uid>/avatar.png).
    struct Blob { bool ok = false; bool notFound = false; bool networkError = false; juce::String error; juce::MemoryBlock data; };
    juce::String uploadAvatar (const juce::String& accessToken, const juce::String& userId, const juce::MemoryBlock& image,
                               const juce::String& mimeType = "image/png") const; // empty = success
    Blob downloadAvatar (const juce::String& accessToken, const juce::String& userId) const;
    juce::String deleteAvatar (const juce::String& accessToken, const juce::String& userId) const;

private:
    struct Response { int status = 0; juce::var json; juce::String networkError; juce::MemoryBlock raw; };

    Response send (const juce::String& method, const juce::String& path, const juce::String& body, const juce::String& bearer) const;
    Response sendBytes (const juce::String& method, const juce::String& path, const juce::MemoryBlock* body,
                        const juce::String& contentType, const juce::String& bearer, const juce::String& extraHeaders) const;
    Session parseSession (const Response&) const;
    static juce::String errorText (const Response&);

    juce::String url, key;
};

//==============================================================================
// Signed-in state, Pro entitlement and session persistence. Message thread only;
// network calls run in the background and report back via change messages.
class Account : public juce::ChangeBroadcaster
{
public:
    Account (juce::PropertiesFile& settings, juce::String projectUrl, juce::String anonKey);
    ~Account() override;

    bool isConfigured() const { return api.isConfigured(); }
    bool isSignedIn() const   { return refreshToken.isNotEmpty(); }
    bool isBusy() const       { return busy > 0; }
    // The plan comes from the signed license (works offline until it expires, see License.h). While the
    // license server isn't deployed yet, a live check of the entitlements table counts for this session only.
    int getTier() const;                       // 0 Free, 1 Pro, 2 Ultra Pro
    bool isPro() const   { return getTier() >= 1; }
    bool isUltra() const { return getTier() >= 2; }
    juce::String getLicenseProblem() const;    // why paid features are off right now ("" = fine)

    // One line about the license for the Account window: 1 = signed license active, 0 = live check only, -1 = paused
    struct LicenseSummary { int state = 0; juce::String text; };
    LicenseSummary getLicenseSummary() const;
    juce::String getLicenseTimesForDebug() const;   // issue / seen / now (no tokens)

    juce::String getEmail() const  { return email; }

    // Profile picture (Pro only). Empty image = none.
    juce::Image getAvatar() const  { return isPro() ? avatar : juce::Image(); }
    void setAvatarFromFile (const juce::File& imageFile);
    void removeAvatar();
    juce::String getStatus() const { return status; }
    juce::String getPlanText() const;

    void restoreSession();
    void signIn (const juce::String& email, const juce::String& password);
    void signUp (const juce::String& email, const juce::String& password);
    void sendPasswordReset (const juce::String& email);
    void refreshStatus();
    void signOut();

    static constexpr int offlineGraceDays = 7;

private:
    template <typename Result, typename Work, typename Done>
    void runAsync (Work work, Done done);

    void applySession (const SupabaseApi::Session&);
    void checkEntitlement();
    void fetchAvatar();
    juce::File avatarCacheFile() const;
    juce::Image avatar;
    void clearSession();
    void save();
    void load();

    juce::PropertiesFile& settings;
    SupabaseApi api;

    juce::String email, userId, accessToken, refreshToken, status;
    juce::Time accessExpiry;
    bool pro = false, ultra = false;          // display only; access comes from the license
    juce::String licenseToken;
    License::Info license;
    int liveLegacyTier = 0;                   // session only, never saved
    mutable juce::int64 latestSeen = 0;       // latest time this PC has seen (clock-rollback check)
    // fromServer = just issued: the server's clock is the truth, so 'latest seen' is reset to it
    // (fixes a clock that was once set ahead); a license loaded from disk can only move it forward.
    void applyLicense (const juce::String& token, const License::Info&, bool fromServer);
    juce::String plan;
    juce::Time proUntil, lastVerified;
    int busy = 0;
    std::shared_ptr<int> alive = std::make_shared<int> (0);

    JUCE_DECLARE_NON_COPYABLE (Account)
};
