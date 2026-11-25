#include "Account.h"
#include "AvatarMedia.h"

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

juce::String SupabaseApi::uploadAvatar (const juce::String& accessToken, const juce::String& userId, const juce::MemoryBlock& image,
                                       const juce::String& mimeType) const
{
    const auto r = sendBytes ("POST", "/storage/v1/object/" + avatarPath (userId), &image, mimeType, accessToken,
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
    const auto userFilter = "&user_id=eq." + juce::URL::addEscapeChars (userId, true);
    auto r = send ("GET", "/rest/v1/entitlements?select=is_pro,is_ultra,plan,pro_until" + userFilter, {}, accessToken);

    // Projects that haven't run the Ultra Pro migration yet don't have is_ultra - ask without it.
    if (r.status == 400 && errorText (r).containsIgnoreCase ("is_ultra"))
        r = send ("GET", "/rest/v1/entitlements?select=is_pro,plan,pro_until" + userFilter, {}, accessToken);

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
        const bool proFlag = (bool) row.getProperty ("is_pro", false);
        const bool ultraFlag = (bool) row.getProperty ("is_ultra", false);
        e.plan = row.getProperty ("plan", "free").toString();
        const auto until = row.getProperty ("pro_until", {});
        if (! until.isVoid() && until.toString().isNotEmpty())
            e.proUntil = juce::Time::fromISO8601 (until.toString());
        const bool current = e.proUntil.toMilliseconds() == 0 || e.proUntil > juce::Time::getCurrentTime();
        e.isUltra = ultraFlag && current;
        e.isPro = (proFlag || ultraFlag) && current; // Ultra includes everything in Pro
    }
    return e;
}

SupabaseApi::LicenseResult SupabaseApi::fetchLicense (const juce::String& accessToken) const
{
    LicenseResult l;
    auto r = send ("POST", "/functions/v1/license", "{}", accessToken);
    l.networkError = r.networkError.isNotEmpty();
    if (r.status == 404)
    {
        l.notDeployed = true;
        return l;
    }
    if (r.status != 200 || ! r.json.isObject())
    {
        l.error = errorText (r);
        return l;
    }
    l.token = r.json.getProperty ("license", {}).toString();
    l.ok = l.token.isNotEmpty();
    if (! l.ok) l.error = "empty license";
    return l;
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

int Account::getTier() const
{
    if (! isSignedIn())
        return 0;
    const auto now = juce::Time::currentTimeMillis();
    const auto check = License::evaluate (license, now, latestSeen, userId);
    if (check.state != License::State::ClockRolledBack)
        latestSeen = juce::jmax (latestSeen, now);
    return juce::jmax (check.state == License::State::Active ? check.tier : 0, liveLegacyTier);
}

juce::String Account::getLicenseProblem() const
{
    if (! isSignedIn() || liveLegacyTier > 0)
        return {};
    const auto check = License::evaluate (license, juce::Time::currentTimeMillis(), latestSeen, userId);
    return check.state == License::State::Active ? juce::String() : check.reason;
}

juce::String Account::getLicenseTimesForDebug() const
{
    auto f = [] (juce::int64 ms) { return ms > 0 ? juce::Time (ms).toString (true, true, true, true) : juce::String ("-"); };
    return "issued " + f (license.iat) + " | latest seen " + f (latestSeen) + " | now " + f (juce::Time::currentTimeMillis());
}

Account::LicenseSummary Account::getLicenseSummary() const
{
    LicenseSummary s;
    if (! isSignedIn())
        return s;
    const auto check = License::evaluate (license, juce::Time::currentTimeMillis(), latestSeen, userId);
    if (check.state == License::State::Active)
    {
        s.state = 1;
        s.text = "Signed license verified - works offline until " + juce::Time (license.exp).formatted ("%d %b %Y");
    }
    else if (liveLegacyTier > 0 || ! license.valid)
        s.text = "Checked live (no signed license saved) - needs internet";
    else
    {
        s.state = -1;
        s.text = check.reason;
    }
    return s;
}

void Account::applyLicense (const juce::String& token, const License::Info& info, bool fromServer)
{
    licenseToken = token;
    license = info;
    liveLegacyTier = 0;
    latestSeen = fromServer ? info.iat : juce::jmax (latestSeen, info.iat);   // the server's clock
    pro = info.tier >= 1;
    ultra = info.tier >= 2;
    plan = info.plan;
    proUntil = juce::Time (info.proUntil);
    lastVerified = juce::Time::getCurrentTime();
}

juce::String Account::getPlanText() const
{
    if (! isSignedIn())
        return "Not signed in";
    if (isPro())
    {
        const juce::String tierName = isUltra() ? "Ultra Pro" : "Pro";
        return proUntil.toMilliseconds() != 0 ? tierName + " - renews/ends " + proUntil.toString (true, false) : tierName;
    }
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
    ultra = false;
    plan.clear();
    proUntil = {};
    lastVerified = {};
    licenseToken.clear();
    license = {};
    liveLegacyTier = 0;   // latestSeen is kept on purpose (clock-rollback check survives signing out)
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
    auto planMessage = [] (int tier) { return tier >= 2 ? juce::String ("AMETHYST Ultra Pro is active. Enjoy!")
                                              : tier == 1 ? juce::String ("AMETHYST Pro is active. Enjoy!") : juce::String ("You're on the Free plan."); };

    // 2) live check of the entitlements table: used when the license server is missing or failing, for this
    //    session only (never saved) - a server problem must never take a paying user's plan away
    auto liveCheck = [this, apiCopy, token, uid, planMessage] (juce::String note)
    {
        runAsync<SupabaseApi::Entitlement> ([apiCopy, token, uid] { return apiCopy.fetchEntitlement (token, uid); },
                                            [this, planMessage, note] (const SupabaseApi::Entitlement& e)
        {
            if (! e.ok)
            {
                status = e.networkError ? "Offline - and no saved license yet." : "Couldn't check your plan: " + e.error;
                return;
            }
            liveLegacyTier = e.isUltra ? 2 : (e.isPro ? 1 : 0);
            pro = e.isPro;
            ultra = e.isUltra;
            plan = e.plan;
            proUntil = e.proUntil;
            status = planMessage (liveLegacyTier) + (note.isNotEmpty() ? "\n(" + note + ")" : juce::String());
            if (liveLegacyTier >= 1)
                fetchAvatar();
        });
    };

    // 1) signed license from the license server
    runAsync<SupabaseApi::LicenseResult> ([apiCopy, token] { return apiCopy.fetchLicense (token); },
                                          [this, planMessage, liveCheck] (const SupabaseApi::LicenseResult& r)
    {
        if (r.ok)
        {
            const auto info = License::verify (r.token);
            if (! info.valid || info.sub != userId)
            {
                liveCheck ("license server: its license didn't verify - " + (info.valid ? juce::String ("wrong account") : info.error)
                           + ". Check that LICENSE_PRIVATE_KEY matches this build.");
                return;
            }
            applyLicense (r.token, info, true);
            save();
            const auto problem = getLicenseProblem();
            status = problem.isNotEmpty() ? problem : planMessage (getTier());
            if (isPro())
                fetchAvatar();
            return;
        }
        if (r.networkError)
        {
            status = "Offline - using your saved license.";
            return;
        }
        liveCheck (r.notDeployed ? juce::String() : "license server: " + (r.error.isNotEmpty() ? r.error : juce::String ("error")));
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

    // still pictures become a 256 px square; GIFs and videos become a strip of 96 px frames
    juce::String error;
    auto square = AvatarMedia::load (imageFile, error);
    if (! square.isValid())
    {
        status = error;
        sendChangeMessage();
        return;
    }

    juce::String mime;
    const auto data = AvatarMedia::encode (square, mime);
    if (data.isEmpty())
    {
        status = "Couldn't prepare that picture.";
        sendChangeMessage();
        return;
    }

    status = AvatarMedia::isAnimated (square) ? "Uploading your animated picture..." : "Uploading your picture...";
    auto apiCopy = api;
    const auto token = accessToken, uid = userId;
    runAsync<juce::String> ([apiCopy, token, uid, data, mime] { return apiCopy.uploadAvatar (token, uid, data, mime); },
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
    obj->setProperty ("license", licenseToken);   // signed; editing it breaks the signature
    latestSeen = juce::jmax (latestSeen, juce::Time::currentTimeMillis());
    obj->setProperty ("seen", latestSeen);
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
    latestSeen = (juce::int64) v.getProperty ("seen", 0);
    licenseToken = v.getProperty ("license", {}).toString();
    if (licenseToken.isNotEmpty())
    {
        const auto info = License::verify (licenseToken);
        if (info.valid && info.sub == userId)
            applyLicense (licenseToken, info, false);
        else
            licenseToken.clear();   // tampered or from another account: ignored
    }
    if (const auto problem = getLicenseProblem(); problem.isNotEmpty() && licenseToken.isNotEmpty())
        status = problem;   // e.g. the clock was set back while AMETHYST was closed

    if (refreshToken.isNotEmpty() && avatarCacheFile().existsAsFile())
        avatar = juce::ImageFileFormat::loadFrom (avatarCacheFile()); // shows instantly; refreshed after sign-in check
}
