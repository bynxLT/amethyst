#include "Account.h"

bool amethystProtect (const void* data, std::size_t size, std::vector<unsigned char>& out, bool encrypt);

namespace
{
    juce::String toJson (std::initializer_list<std::pair<const char*, juce::String>> fields)
    {
        auto* obj = new juce::DynamicObject();
        for (auto& [k, v] : fields)
            obj->setProperty (k, v);
        return juce::JSON::toString (juce::var (obj), true);
    }
}

//==============================================================================
SupabaseApi::SupabaseApi (juce::String projectUrl, juce::String anonKey)
    : url (projectUrl.trim().trimCharactersAtEnd ("/")), key (anonKey.trim())
{
}

SupabaseApi::Response SupabaseApi::send (const juce::String& method, const juce::String& path,
                                          const juce::String& body, const juce::String& bearer) const
{
    juce::MemoryBlock bytes (body.toRawUTF8(), body.getNumBytesAsUTF8());
    return sendBytes (method, path, body.isNotEmpty() ? &bytes : nullptr, "application/json", bearer, "Accept: application/json\r\n");
}

SupabaseApi::Response SupabaseApi::sendBytes (const juce::String& method, const juce::String& path, const juce::MemoryBlock* body,
                                               const juce::String& contentType, const juce::String& bearer, const juce::String& extraHeaders) const
{
    Response r;
    if (! isConfigured())
    {
        r.networkError = "Accounts aren't configured in this build.";
        return r;
    }

    juce::URL u (url + path);
    if (body != nullptr && body->getSize() > 0)
        u = u.withPOSTData (*body);

    juce::String headers;
    headers << "apikey: " << key << "\r\n";
    if (contentType.isNotEmpty())
        headers << "Content-Type: " << contentType << "\r\n";
    if (bearer.isNotEmpty())
        headers << "Authorization: Bearer " << bearer << "\r\n";
    headers << extraHeaders;

    auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                       .withExtraHeaders (headers)
                       .withConnectionTimeoutMs (20000)
                       .withStatusCode (&r.status)
                       .withHttpRequestCmd (method);

    auto stream = u.createInputStream (options);
    if (stream == nullptr)
    {
        r.networkError = "Couldn't reach the account server. Check your internet connection.";
        return r;
    }

    stream->readIntoMemoryBlock (r.raw);
    if (r.raw.getSize() > 0)
    {
        const auto first = static_cast<const char*> (r.raw.getData())[0];
        if (first == '{' || first == '[')
            r.json = juce::JSON::parse (r.raw.toString());
    }
    return r;
}

namespace
{
    juce::String avatarPath (const juce::String& userId)
    {
        return "avatars/" + juce::URL::addEscapeChars (userId, true) + "/avatar.png";
    }
}

juce::String SupabaseApi::uploadAvatar (const juce::String& accessToken, const juce::String& userId, const juce::MemoryBlock& png) const
{
    const auto r = sendBytes ("POST", "/storage/v1/object/" + avatarPath (userId), &png, "image/png", accessToken,
                              "x-upsert: true\r\nCache-Control: max-age=60\r\n");
    if (r.status >= 200 && r.status < 300)
        return {};
    if (r.status == 403 || r.status == 401 || errorText (r).containsIgnoreCase ("row-level security"))
        return "Profile pictures are part of AMETHYST Pro.";
    return errorText (r);
}

SupabaseApi::Blob SupabaseApi::downloadAvatar (const juce::String& accessToken, const juce::String& userId) const
{
    Blob b;
    const auto r = sendBytes ("GET", "/storage/v1/object/authenticated/" + avatarPath (userId), nullptr, {}, accessToken, {});
    b.networkError = r.networkError.isNotEmpty();

    if (r.status == 200 && r.raw.getSize() > 0 && ! r.json.isObject())
    {
        b.ok = true;
        b.data = r.raw;
        return b;
    }

    const auto code = r.json.getProperty ("statusCode", {}).toString();
    b.notFound = r.status == 404 || code == "404" || errorText (r).containsIgnoreCase ("not found");
    b.error = errorText (r);
    return b;
}

juce::String SupabaseApi::deleteAvatar (const juce::String& accessToken, const juce::String& userId) const
{
    const auto r = sendBytes ("DELETE", "/storage/v1/object/" + avatarPath (userId), nullptr, {}, accessToken, {});
    return (r.status >= 200 && r.status < 300) || r.status == 404 ? juce::String() : errorText (r);
}

juce::String SupabaseApi::errorText (const Response& r)
{
    if (r.networkError.isNotEmpty())
        return r.networkError;

    for (auto* field : { "msg", "error_description", "message", "error" })
    {
        const auto v = r.json.getProperty (field, {}).toString();
        if (v.isNotEmpty())
            return v;
    }
    return "Server error (HTTP " + juce::String (r.status) + ")";
}

SupabaseApi::Session SupabaseApi::parseSession (const Response& r) const
{
    Session s;
    s.networkError = r.networkError.isNotEmpty();

    if (r.status < 200 || r.status >= 300 || ! r.json.isObject())
    {
        s.error = errorText (r);
        return s;
    }

    const auto user = r.json.getProperty ("user", {});
    s.accessToken = r.json.getProperty ("access_token", {}).toString();
    s.refreshToken = r.json.getProperty ("refresh_token", {}).toString();
    s.expiresIn = (int) r.json.getProperty ("expires_in", 3600);
    s.userId = user.getProperty ("id", r.json.getProperty ("id", {})).toString();
    s.email = user.getProperty ("email", r.json.getProperty ("email", {})).toString();

    if (s.accessToken.isNotEmpty() && s.refreshToken.isNotEmpty())
        s.ok = true;
    else if (s.userId.isNotEmpty())
        s.ok = s.needsConfirmation = true;   // sign-up with email confirmation turned on
    else
        s.error = "Unexpected response from the account server.";
    return s;
}

SupabaseApi::Session SupabaseApi::signIn (const juce::String& email, const juce::String& password) const
{
    return parseSession (send ("POST", "/auth/v1/token?grant_type=password", toJson ({ { "email", email }, { "password", password } }), {}));
}

SupabaseApi::Session SupabaseApi::signUp (const juce::String& email, const juce::String& password) const
{
    return parseSession (send ("POST", "/auth/v1/signup", toJson ({ { "email", email }, { "password", password } }), {}));
}

SupabaseApi::Session SupabaseApi::refresh (const juce::String& refreshToken) const
{
    return parseSession (send ("POST", "/auth/v1/token?grant_type=refresh_token", toJson ({ { "refresh_token", refreshToken } }), {}));
}

SupabaseApi::Entitlement SupabaseApi::fetchEntitlement (const juce::String& accessToken, const juce::String& userId) const
{
    Entitlement e;
    const auto r = send ("GET", "/rest/v1/entitlements?select=is_pro,plan,pro_until&user_id=eq." + juce::URL::addEscapeChars (userId, true),
                         {}, accessToken);
    e.networkError = r.networkError.isNotEmpty();

    if (r.status != 200 || ! r.json.isArray())
    {
        e.error = errorText (r);
        return e;
    }

    e.ok = true;
    if (auto* arr = r.json.getArray(); arr != nullptr && ! arr->isEmpty())
    {
        const auto row = arr->getReference (0);
        const bool flag = (bool) row.getProperty ("is_pro", false);
        e.plan = row.getProperty ("plan", "free").toString();
        const auto until = row.getProperty ("pro_until", {});
        if (! until.isVoid() && until.toString().isNotEmpty())
            e.proUntil = juce::Time::fromISO8601 (until.toString());
        e.isPro = flag && (e.proUntil.toMilliseconds() == 0 || e.proUntil > juce::Time::getCurrentTime());
    }
    return e;
}

void SupabaseApi::signOut (const juce::String& accessToken) const
{
    if (accessToken.isNotEmpty())
        send ("POST", "/auth/v1/logout", "{}", accessToken);
}

juce::String SupabaseApi::sendPasswordReset (const juce::String& email) const
{
    const auto r = send ("POST", "/auth/v1/recover", toJson ({ { "email", email } }), {});
    return (r.status >= 200 && r.status < 300) ? juce::String() : errorText (r);
}

//==============================================================================
Account::Account (juce::PropertiesFile& s, juce::String projectUrl, juce::String anonKey)
    : settings (s), api (std::move (projectUrl), std::move (anonKey))
{
    load();
    if (! isConfigured())
        status = "Accounts aren't set up in this build yet.";
}

Account::~Account()
{
    alive.reset(); // pending callbacks become no-ops
}

template <typename Result, typename Work, typename Done>
void Account::runAsync (Work work, Done done)
{
    ++busy;
    sendChangeMessage();
    std::weak_ptr<int> token = alive;

    juce::Thread::launch ([this, token, work, done]
    {
        Result result = work();
        juce::MessageManager::callAsync ([this, token, done, result]
        {
            if (token.expired())
                return;
            --busy;
            done (result);
            sendChangeMessage();
        });
    });
}

bool Account::isPro() const
{
    if (! pro || ! isSignedIn())
        return false;
    const auto now = juce::Time::getCurrentTime();
    if (proUntil.toMilliseconds() != 0 && proUntil < now)
        return false;
    // Works offline for a while after the last successful check.
    return now - lastVerified < juce::RelativeTime::days (offlineGraceDays);
}

juce::String Account::getPlanText() const
{
    if (! isSignedIn())
        return "Not signed in";
    if (isPro())
        return proUntil.toMilliseconds() != 0 ? "Pro - renews/ends " + proUntil.toString (true, false) : juce::String ("Pro");
    if (pro)
        return "Pro (needs to re-check online)";
    return "Free";
}

void Account::applySession (const SupabaseApi::Session& s)
{
    accessToken = s.accessToken;
    refreshToken = s.refreshToken;
    if (s.userId.isNotEmpty()) userId = s.userId;
    if (s.email.isNotEmpty())  email = s.email;
    accessExpiry = juce::Time::getCurrentTime() + juce::RelativeTime::seconds (juce::jmax (60, s.expiresIn - 60));
    save();
}

void Account::clearSession()
{
    accessToken.clear();
    refreshToken.clear();
    userId.clear();
    pro = false;
    plan.clear();
    proUntil = {};
    lastVerified = {};
    avatar = {};
    avatarCacheFile().deleteFile();
    save();
}

void Account::signIn (const juce::String& e, const juce::String& password)
{
    if (e.trim().isEmpty() || password.isEmpty())
    {
        status = "Enter your email and password.";
        sendChangeMessage();
        return;
    }

    status = "Signing in...";
    auto apiCopy = api;
    runAsync<SupabaseApi::Session> ([apiCopy, e, password] { return apiCopy.signIn (e.trim(), password); },
                                    [this] (const SupabaseApi::Session& s)
    {
        if (! s.ok)
        {
            status = s.error;
            return;
        }
        applySession (s);
        status = "Signed in.";
        checkEntitlement();
    });
}

void Account::signUp (const juce::String& e, const juce::String& password)
{
    if (e.trim().isEmpty() || password.length() < 6)
    {
        status = "Use a valid email and a password of at least 6 characters.";
        sendChangeMessage();
        return;
    }

    status = "Creating your account...";
    auto apiCopy = api;
    runAsync<SupabaseApi::Session> ([apiCopy, e, password] { return apiCopy.signUp (e.trim(), password); },
                                    [this, e] (const SupabaseApi::Session& s)
    {
        if (! s.ok)
        {
            status = s.error;
            return;
        }
        if (s.needsConfirmation)
        {
            email = e.trim();
            save();
            status = "Account created! Check your email to confirm it, then sign in.";
            return;
        }
        applySession (s);
        status = "Account created - you're signed in.";
        checkEntitlement();
    });
}

void Account::sendPasswordReset (const juce::String& e)
{
    if (e.trim().isEmpty())
    {
        status = "Type your email first, then click 'Forgot password?'.";
        sendChangeMessage();
        return;
    }

    status = "Sending reset email...";
    auto apiCopy = api;
    runAsync<juce::String> ([apiCopy, e] { return apiCopy.sendPasswordReset (e.trim()); },
                            [this] (const juce::String& err)
    {
        status = err.isEmpty() ? "If that email has an account, a reset link is on its way." : err;
    });
}

void Account::checkEntitlement()
{
    if (accessToken.isEmpty() || userId.isEmpty())
        return;

    auto apiCopy = api;
    const auto token = accessToken, uid = userId;
    runAsync<SupabaseApi::Entitlement> ([apiCopy, token, uid] { return apiCopy.fetchEntitlement (token, uid); },
                                        [this] (const SupabaseApi::Entitlement& e)
    {
        if (! e.ok)
        {
            status = e.networkError ? "Offline - using your last known plan." : "Couldn't check your plan: " + e.error;
            return;
        }
        pro = e.isPro;
        plan = e.plan;
        proUntil = e.proUntil;
        lastVerified = juce::Time::getCurrentTime();
        save();
        status = pro ? "AMETHYST Pro is active. Enjoy!" : "You're on the Free plan.";
        if (pro)
            fetchAvatar();
    });
}

//==============================================================================
juce::File Account::avatarCacheFile() const
{
    return settings.getFile().getSiblingFile ("avatar.png");
}

void Account::fetchAvatar()
{
    if (accessToken.isEmpty() || userId.isEmpty())
        return;

    auto apiCopy = api;
    const auto token = accessToken, uid = userId;
    runAsync<SupabaseApi::Blob> ([apiCopy, token, uid] { return apiCopy.downloadAvatar (token, uid); },
                                 [this] (const SupabaseApi::Blob& b)
    {
        if (b.ok)
        {
            auto img = juce::ImageFileFormat::loadFrom (b.data.getData(), b.data.getSize());
            if (img.isValid())
            {
                avatar = img;
                avatarCacheFile().replaceWithData (b.data.getData(), b.data.getSize());
            }
        }
        else if (b.notFound)
        {
            avatar = {};
            avatarCacheFile().deleteFile();
        }
        // network errors: keep the cached picture
    });
}

void Account::setAvatarFromFile (const juce::File& imageFile)
{
    if (! isPro())
    {
        status = "Profile pictures are part of AMETHYST Pro.";
        sendChangeMessage();
        return;
    }
    if (accessToken.isEmpty() || juce::Time::getCurrentTime() > accessExpiry)
    {
        status = "Reconnecting to your account - try again in a moment.";
        restoreSession();
        return;
    }

    auto img = juce::ImageFileFormat::loadFrom (imageFile);
    if (! img.isValid())
    {
        status = "That file isn't an image AMETHYST can read (use PNG or JPG).";
        sendChangeMessage();
        return;
    }

    // centre-crop to a square and shrink to 256 x 256
    const int side = juce::jmin (img.getWidth(), img.getHeight());
    auto square = img.getClippedImage ({ (img.getWidth() - side) / 2, (img.getHeight() - side) / 2, side, side })
                     .rescaled (256, 256, juce::Graphics::highResamplingQuality);

    juce::MemoryOutputStream out;
    juce::PNGImageFormat png;
    if (! png.writeImageToStream (square, out))
    {
        status = "Couldn't prepare that picture.";
        sendChangeMessage();
        return;
    }
    juce::MemoryBlock data (out.getData(), out.getDataSize());

    status = "Uploading your picture...";
    auto apiCopy = api;
    const auto token = accessToken, uid = userId;
    runAsync<juce::String> ([apiCopy, token, uid, data] { return apiCopy.uploadAvatar (token, uid, data); },
                            [this, square, data] (const juce::String& err)
    {
        if (err.isNotEmpty())
        {
            status = err;
            return;
        }
        avatar = square;
        avatarCacheFile().replaceWithData (data.getData(), data.getSize());
        status = "Profile picture updated.";
    });
}

void Account::removeAvatar()
{
    if (accessToken.isEmpty() || userId.isEmpty())
        return;

    status = "Removing your picture...";
    auto apiCopy = api;
    const auto token = accessToken, uid = userId;
    runAsync<juce::String> ([apiCopy, token, uid] { return apiCopy.deleteAvatar (token, uid); },
                            [this] (const juce::String& err)
    {
        if (err.isNotEmpty())
        {
            status = err;
            return;
        }
        avatar = {};
        avatarCacheFile().deleteFile();
        status = "Profile picture removed.";
    });
}

void Account::restoreSession()
{
    if (! isConfigured() || refreshToken.isEmpty())
        return;

    status = "Checking your account...";
    auto apiCopy = api;
    const auto token = refreshToken;
    runAsync<SupabaseApi::Session> ([apiCopy, token] { return apiCopy.refresh (token); },
                                    [this] (const SupabaseApi::Session& s)
    {
        if (s.ok && ! s.needsConfirmation)
        {
            applySession (s);
            checkEntitlement();
        }
        else if (s.networkError)
        {
            status = "Offline - using your last known plan.";
        }
        else
        {
            clearSession();
            status = "Your session expired. Please sign in again.";
        }
    });
}

void Account::refreshStatus()
{
    if (! isSignedIn())
        return;
    if (accessToken.isEmpty() || juce::Time::getCurrentTime() > accessExpiry)
        restoreSession();
    else
    {
        status = "Checking your plan...";
        checkEntitlement();
    }
}

void Account::signOut()
{
    auto apiCopy = api;
    const auto token = accessToken;
    juce::Thread::launch ([apiCopy, token] { apiCopy.signOut (token); }); // best effort
    clearSession();
    status = "Signed out.";
    sendChangeMessage();
}

//==============================================================================
void Account::save()
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("email", email);
    obj->setProperty ("userId", userId);
    obj->setProperty ("refresh", refreshToken);
    obj->setProperty ("pro", pro);
    obj->setProperty ("plan", plan);
    obj->setProperty ("proUntil", proUntil.toMilliseconds());
    obj->setProperty ("verified", lastVerified.toMilliseconds());
    const auto json = juce::JSON::toString (juce::var (obj), true);

    std::vector<unsigned char> blob;
    if (amethystProtect (json.toRawUTF8(), json.getNumBytesAsUTF8(), blob, true))
        settings.setValue ("account", juce::Base64::toBase64 (blob.data(), blob.size()));
    else
        settings.removeValue ("account"); // never store the session unencrypted
    settings.saveIfNeeded();
}

void Account::load()
{
    const auto b64 = settings.getValue ("account");
    if (b64.isEmpty())
        return;

    juce::MemoryOutputStream decoded;
    if (! juce::Base64::convertFromBase64 (decoded, b64))
        return;

    std::vector<unsigned char> plain;
    if (! amethystProtect (decoded.getData(), decoded.getDataSize(), plain, false))
        return;

    const auto v = juce::JSON::parse (juce::String::fromUTF8 ((const char*) plain.data(), (int) plain.size()));
    email = v.getProperty ("email", {}).toString();
    userId = v.getProperty ("userId", {}).toString();
    refreshToken = v.getProperty ("refresh", {}).toString();
    pro = (bool) v.getProperty ("pro", false);
    plan = v.getProperty ("plan", {}).toString();
    proUntil = juce::Time ((juce::int64) v.getProperty ("proUntil", 0));
    lastVerified = juce::Time ((juce::int64) v.getProperty ("verified", 0));

    if (refreshToken.isNotEmpty() && avatarCacheFile().existsAsFile())
        avatar = juce::ImageFileFormat::loadFrom (avatarCacheFile()); // shows instantly; refreshed after sign-in check
}
