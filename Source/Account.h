#pragma once

#include <JuceHeader.h>

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
        bool isPro = false;               // already accounts for pro_until
        juce::String plan;
        juce::Time proUntil;              // zero = no expiry
    };

    SupabaseApi (juce::String projectUrl, juce::String anonKey);

    bool isConfigured() const { return url.isNotEmpty() && key.isNotEmpty(); }

    Session signIn (const juce::String& email, const juce::String& password) const;
    Session signUp (const juce::String& email, const juce::String& password) const;
    Session refresh (const juce::String& refreshToken) const;
    Entitlement fetchEntitlement (const juce::String& accessToken, const juce::String& userId) const;
    void signOut (const juce::String& accessToken) const;
    juce::String sendPasswordReset (const juce::String& email) const;   // empty = success

    // Profile picture in Supabase Storage (bucket "avatars", path <uid>/avatar.png).
    struct Blob { bool ok = false; bool notFound = false; bool networkError = false; juce::String error; juce::MemoryBlock data; };
    juce::String uploadAvatar (const juce::String& accessToken, const juce::String& userId, const juce::MemoryBlock& png) const; // empty = success
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
    bool isPro() const;

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
    bool pro = false;
    juce::String plan;
    juce::Time proUntil, lastVerified;
    int busy = 0;
    std::shared_ptr<int> alive = std::make_shared<int> (0);

    JUCE_DECLARE_NON_COPYABLE (Account)
};
