#pragma once

#include <JuceHeader.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <atomic>
#include <functional>
#include <vector>

// OAuth client for the Drive API. Injected by CMake from google-client.cmake
// (git-ignored). Google treats desktop-app secrets as non-confidential, but
// the file stays out of the public repo anyway. Without it the build is fine
// and the notes card says the feature is unavailable.
#ifndef LL_GOOGLE_CLIENT_ID
 #define LL_GOOGLE_CLIENT_ID ""
#endif
#ifndef LL_GOOGLE_CLIENT_SECRET
 #define LL_GOOGLE_CLIENT_SECRET ""
#endif

// A Google Doc attached to a stream as its session notes.
struct NotesDoc
{
    juce::String id, name, url;
    juce::String when;   // "Sep 25" for the recent-docs menu; not persisted
    bool isValid() const { return id.isNotEmpty() && url.isNotEmpty(); }

    static juce::String urlFor(const juce::String& id)
    {
        return "https://docs.google.com/document/d/" + id + "/edit";
    }
};

// One Google connection per USER (not per plugin instance): the refresh token
// lives in ~/Library/Application Support/ListenLink/google-auth.json (0600),
// never in plugin state (project files get shared). Scope is drive.file only:
// the plugin can create docs and see the docs it created, nothing else in the
// user's Drive. All network calls go through /usr/bin/curl (like the rest of
// the plugin); bearer tokens and secrets travel in a temp config file, not on
// the command line. Blocking calls are meant for a background thread.
class GoogleDocs
{
public:
    static GoogleDocs& get() { static GoogleDocs g; return g; }

    static bool isConfigured() { return juce::String(LL_GOOGLE_CLIENT_ID).isNotEmpty(); }

    bool isSignedIn() const           { const juce::ScopedLock sl(lock); return refreshToken.isNotEmpty(); }
    juce::String getEmail() const     { const juce::ScopedLock sl(lock); return email; }
    bool isBusy() const               { return busy.load(); }
    juce::String getLastError() const { const juce::ScopedLock sl(lock); return lastError; }
    void clearError()                 { const juce::ScopedLock sl(lock); lastError.clear(); }
    bool isSignInPending() const      { const juce::ScopedLock sl(lock); return pendingState.isNotEmpty(); }

    // ---- sign-in ---------------------------------------------------------

    // Opens the system browser on Google's consent page. The redirect lands on
    // the plugin's own HTTP server (StreamServer routes /oauth/callback here).
    void beginSignIn(int localPort)
    {
        if (! isConfigured() || localPort <= 0)
            return;

        juce::String verifier, state, redirect;
        {
            const juce::ScopedLock sl(lock);
            pendingVerifier = randomToken(48);
            pendingState = randomToken(24);
            pendingRedirect = "http://127.0.0.1:" + juce::String(localPort) + "/oauth/callback";
            verifier = pendingVerifier;
            state = pendingState;
            redirect = pendingRedirect;
            lastError.clear();
        }

        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(verifier.toRawUTF8(), (CC_LONG) verifier.getNumBytesAsUTF8(), digest);

        const juce::String url =
            "https://accounts.google.com/o/oauth2/v2/auth?client_id=" + enc(LL_GOOGLE_CLIENT_ID)
            + "&redirect_uri=" + enc(redirect)
            + "&response_type=code"
            + "&scope=" + enc("https://www.googleapis.com/auth/drive.file openid email")
            + "&code_challenge=" + base64Url(digest, sizeof(digest))
            + "&code_challenge_method=S256"
            + "&state=" + state
            + "&access_type=offline&prompt=consent";
        openExternal(url);
    }

    void cancelSignIn()
    {
        const juce::ScopedLock sl(lock);
        pendingState.clear();
        pendingVerifier.clear();
    }

    // Runs on the StreamServer accept thread. Exchanges the code for tokens
    // (blocking, a second or two) so the page it returns can tell the truth.
    // Returns the HTTP status to answer with; html receives the body.
    int handleOAuthCallback(const juce::String& query, juce::String& html)
    {
        juce::String verifier, state, redirect;
        {
            const juce::ScopedLock sl(lock);
            verifier = pendingVerifier;
            state = pendingState;
            redirect = pendingRedirect;
        }
        if (state.isEmpty())
        {
            html = page("Nothing to do", "ListenLink isn't waiting for a Google sign-in. You can close this tab.");
            return 404;
        }

        const auto params = parseQuery(query);
        // A stray or forged hit must not cancel the real callback: only a
        // matching state consumes the pending sign-in.
        if (params["state"] != state)
        {
            html = page("Not for this session", "This sign-in link doesn't match the one ListenLink opened. Try Connect again in the plugin.");
            return 400;
        }
        {
            const juce::ScopedLock sl(lock);
            pendingState.clear();
            pendingVerifier.clear();
        }

        if (params.containsKey("error") || ! params.containsKey("code"))
        {
            setError("Google sign-in was cancelled.");
            html = page("Not connected", "Google sign-in was cancelled. You can close this tab.");
            return 200;
        }

        int status = 0;
        const auto resp = juce::JSON::parse(httpForm(kTokenUrl,
            "code=" + enc(params["code"])
            + "&client_id=" + enc(LL_GOOGLE_CLIENT_ID)
            + "&client_secret=" + enc(LL_GOOGLE_CLIENT_SECRET)
            + "&redirect_uri=" + enc(redirect)
            + "&grant_type=authorization_code"
            + "&code_verifier=" + verifier, status));

        const auto rt = resp.getProperty("refresh_token", "").toString();
        const auto at = resp.getProperty("access_token", "").toString();
        if (status != 200 || rt.isEmpty() || at.isEmpty())
        {
            setError("Google sign-in failed (" + describe(resp, status) + ").");
            html = page("Not connected", "Google didn't complete the sign-in (" + describe(resp, status)
                        + "). Close this tab and try Connect again.");
            return 200;
        }

        // Google's consent page lets people untick individual permissions.
        // Without Drive access nothing here works, so don't keep a token that
        // can only tell us the email address - seen in the field 2026-09-25.
        if (! resp.getProperty("scope", "").toString().contains("auth/drive.file"))
        {
            int st = 0;
            httpForm("https://oauth2.googleapis.com/revoke", "token=" + enc(rt), st);
            setError("Google didn't grant Drive access. Connect again and tick the Google Drive box.");
            html = page("Drive access not granted",
                        "The sign-in went through, but the Google Drive permission wasn't ticked, so "
                        "ListenLink can't create docs. Close this tab, click Connect Google Docs again, "
                        "and tick the box that mentions Google Drive files.");
            return 200;
        }

        {
            const juce::ScopedLock sl(lock);
            refreshToken = rt;
            accessToken = at;
            expiresAt = juce::Time::currentTimeMillis()
                      + (juce::int64) (int) resp.getProperty("expires_in", 3600) * 1000 - 60000;
            email = emailFromIdToken(resp.getProperty("id_token", "").toString());
            lastError.clear();
        }
        save();
        refreshRecentAsync();
        html = page("Connected", "ListenLink is connected to Google Docs. You can close this tab and go back to your DAW.");
        return 200;
    }

    // Revokes the token at Google (best effort) and forgets it locally.
    void signOut()
    {
        juce::String rt;
        {
            const juce::ScopedLock sl(lock);
            rt = refreshToken;
            refreshToken.clear();
            accessToken.clear();
            email.clear();
            recent.clear();
            lastError.clear();
        }
        authFile().deleteFile();
        if (rt.isNotEmpty())
            juce::Thread::launch([rt]
            {
                int status = 0;
                httpForm("https://oauth2.googleapis.com/revoke", "token=" + enc(rt), status);
            });
    }

    // ---- Drive ------------------------------------------------------------

    // Creates a Google Doc and shares it "anyone with the link can edit".
    bool createDoc(const juce::String& name, NotesDoc& out, juce::String& err)
    {
        juce::String tok;
        if (! ensureAccessToken(tok, err))
            return false;

        int status = 0;
        auto* body = new juce::DynamicObject();
        body->setProperty("name", name);
        body->setProperty("mimeType", "application/vnd.google-apps.document");
        const auto resp = juce::JSON::parse(httpJson("POST",
            "https://www.googleapis.com/drive/v3/files?fields=id,name",
            tok, juce::JSON::toString(juce::var(body)), status));

        const auto id = resp.getProperty("id", "").toString();
        if (status != 200 || id.isEmpty())
        {
            const auto why = describe(resp, status);
            err = why.containsIgnoreCase("insufficient authentication scopes")
                ? juce::String("Google didn't grant Drive access. Disconnect, then connect again and tick the Google Drive box.")
                : "Couldn't create the doc (" + why + ").";
            return false;
        }
        out = { id, resp.getProperty("name", name).toString(), NotesDoc::urlFor(id), {} };

        auto* perm = new juce::DynamicObject();
        perm->setProperty("role", "writer");
        perm->setProperty("type", "anyone");
        const auto presp = juce::JSON::parse(httpJson("POST",
            "https://www.googleapis.com/drive/v3/files/" + id + "/permissions",
            tok, juce::JSON::toString(juce::var(perm)), status));
        if (status != 200)
            err = "Doc created, but sharing couldn't be opened (" + describe(presp, status)
                + "). Set it to \"Anyone with the link can edit\" in Google Docs.";

        refreshRecentAsync();
        return true;
    }

    // Docs this plugin created, newest first (drive.file only ever sees those).
    bool listRecent(std::vector<NotesDoc>& out, juce::String& err)
    {
        juce::String tok;
        if (! ensureAccessToken(tok, err))
            return false;

        int status = 0;
        const auto resp = juce::JSON::parse(httpJson("GET",
            "https://www.googleapis.com/drive/v3/files?pageSize=12&orderBy=modifiedTime%20desc"
            "&fields=files(id,name,modifiedTime)&q="
            + enc("mimeType='application/vnd.google-apps.document' and trashed=false"),
            tok, {}, status));
        if (status != 200)
        {
            err = "Couldn't list your docs (" + describe(resp, status) + ").";
            return false;
        }
        out.clear();
        if (auto* files = resp.getProperty("files", juce::var()).getArray())
            for (const auto& f : *files)
            {
                const auto id = f.getProperty("id", "").toString();
                if (id.isEmpty())
                    continue;
                NotesDoc d { id, f.getProperty("name", "Untitled").toString(), NotesDoc::urlFor(id), {} };
                const auto t = juce::Time::fromISO8601(f.getProperty("modifiedTime", "").toString());
                if (t.toMilliseconds() > 0)
                    d.when = t.formatted("%b %e").replace("  ", " ");
                out.push_back(d);
            }
        return true;
    }

    std::vector<NotesDoc> getRecent() const { const juce::ScopedLock sl(lock); return recent; }

    void refreshRecentAsync()
    {
        if (! isSignedIn() || refreshing.exchange(true))
            return;
        juce::Thread::launch([this]
        {
            std::vector<NotesDoc> docs;
            juce::String err;
            if (listRecent(docs, err))
            {
                const juce::ScopedLock sl(lock);
                recent = std::move(docs);
            }
            refreshing.store(false);
        });
    }

    // A pasted Google Docs link. Only docs.google.com/document/d/<id> is
    // accepted - the URL ends up on the listener page as a link.
    static bool parseDocUrl(const juce::String& text, NotesDoc& out)
    {
        const auto t = text.trim();
        const juce::String prefix = "docs.google.com/document/d/";
        const int at = t.indexOf(prefix);
        if (at < 0 || ! (t.startsWith("https://") || t.startsWith("http://") || t.startsWith("docs.")))
            return false;
        juce::String id;
        for (auto c : t.substring(at + prefix.length()))
        {
            if (juce::CharacterFunctions::isLetterOrDigit(c) || c == '_' || c == '-')
                id += c;
            else
                break;
        }
        if (id.length() < 10)
            return false;
        out = { id, "Linked doc", NotesDoc::urlFor(id), {} };
        return true;
    }

    // Title of a link-shared doc from its public HTML (blocking, best effort).
    static juce::String fetchPublicTitle(const juce::String& url)
    {
        juce::ChildProcess curl;
        if (! curl.start(juce::StringArray { "/usr/bin/curl", "-sSL", "-m", "10", "-o", "-", url }))
            return {};
        const auto html = curl.readAllProcessOutput();
        curl.waitForProcessToFinish(12000);
        auto title = html.fromFirstOccurrenceOf("<title>", false, false)
                         .upToFirstOccurrenceOf("</title>", false, false).trim();
        if (title.endsWith(" - Google Docs"))
            title = title.dropLastCharacters(14).trim();
        else
            return {};   // sign-in page or not a doc
        return title.replace("&amp;", "&").replace("&#39;", "'").replace("&quot;", "\"")
                    .replace("&lt;", "<").replace("&gt;", ">");
    }

    // Runs one blocking job at a time on a background thread; the UI polls
    // isBusy() and getLastError(). A second request while busy is dropped.
    bool runJob(std::function<void()> job)
    {
        if (busy.exchange(true))
            return false;
        clearError();
        juce::Thread::launch([this, job]
        {
            job();
            busy.store(false);
        });
        return true;
    }

    void setError(const juce::String& e) { const juce::ScopedLock sl(lock); lastError = e; }

    static void openExternal(const juce::String& url)
    {
        if (! juce::URL(url).launchInDefaultBrowser())
        {
            juce::ChildProcess open;
            open.start(juce::StringArray { "/usr/bin/open", url });
        }
    }

private:
    GoogleDocs() { load(); }

    static constexpr const char* kTokenUrl = "https://oauth2.googleapis.com/token";

    // ---- token storage ----------------------------------------------------

    static juce::File authFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Application Support/ListenLink/google-auth.json");
    }

    void load()
    {
        const auto f = authFile();
        if (! f.existsAsFile())
            return;
        const auto v = juce::JSON::parse(f.loadFileAsString());
        const juce::ScopedLock sl(lock);
        refreshToken = v.getProperty("refresh_token", "").toString();
        email = v.getProperty("email", "").toString();
    }

    void save()
    {
        auto* o = new juce::DynamicObject();
        {
            const juce::ScopedLock sl(lock);
            o->setProperty("refresh_token", refreshToken);
            o->setProperty("email", email);
        }
        const auto f = authFile();
        f.getParentDirectory().createDirectory();
        f.replaceWithText(juce::JSON::toString(juce::var(o)));
        ::chmod(f.getFullPathName().toRawUTF8(), S_IRUSR | S_IWUSR);
    }

    bool ensureAccessToken(juce::String& token, juce::String& err)
    {
        juce::String rt;
        {
            const juce::ScopedLock sl(lock);
            if (accessToken.isNotEmpty() && juce::Time::currentTimeMillis() < expiresAt)
            {
                token = accessToken;
                return true;
            }
            rt = refreshToken;
        }
        if (rt.isEmpty())
        {
            err = "Not connected to Google.";
            return false;
        }

        int status = 0;
        const auto resp = juce::JSON::parse(httpForm(kTokenUrl,
            "client_id=" + enc(LL_GOOGLE_CLIENT_ID)
            + "&client_secret=" + enc(LL_GOOGLE_CLIENT_SECRET)
            + "&refresh_token=" + enc(rt)
            + "&grant_type=refresh_token", status));
        const auto at = resp.getProperty("access_token", "").toString();
        if (status != 200 || at.isEmpty())
        {
            if (resp.getProperty("error", "").toString() == "invalid_grant")
            {
                // Revoked or expired at Google's end: forget it so the UI
                // offers Connect again instead of failing forever.
                {
                    const juce::ScopedLock sl(lock);
                    refreshToken.clear();
                    accessToken.clear();
                    email.clear();
                    recent.clear();
                }
                authFile().deleteFile();
                err = "Google connection expired - connect again.";
            }
            else
                err = "Couldn't reach Google (" + describe(resp, status) + ").";
            return false;
        }
        {
            const juce::ScopedLock sl(lock);
            accessToken = at;
            expiresAt = juce::Time::currentTimeMillis()
                      + (juce::int64) (int) resp.getProperty("expires_in", 3600) * 1000 - 60000;
        }
        token = at;
        return true;
    }

    // ---- HTTP via curl ----------------------------------------------------

    // curl with the secrets in a 0600 config file (-K): nothing sensitive on
    // the command line. Returns the body; status receives the HTTP code
    // (0 = curl itself failed).
    static juce::String httpRaw(const juce::StringArray& args, const juce::String& configLines,
                                const juce::String& bodyData, int& status)
    {
        status = 0;
        const auto cfg = juce::File::createTempFile("llcfg");
        const auto bodyFile = juce::File::createTempFile("llbody");
        juce::String config = configLines;
        if (bodyData.isNotEmpty())
        {
            bodyFile.replaceWithText(bodyData);
            ::chmod(bodyFile.getFullPathName().toRawUTF8(), S_IRUSR | S_IWUSR);
            config += "data-binary = \"@" + bodyFile.getFullPathName() + "\"\n";
        }
        cfg.replaceWithText(config);
        ::chmod(cfg.getFullPathName().toRawUTF8(), S_IRUSR | S_IWUSR);

        juce::StringArray full { "/usr/bin/curl", "-sS", "-m", "20", "-w", "\n%{http_code}",
                                 "-K", cfg.getFullPathName() };
        full.addArray(args);

        juce::String out;
        juce::ChildProcess curl;
        if (curl.start(full, juce::ChildProcess::wantStdOut))
        {
            out = curl.readAllProcessOutput();
            curl.waitForProcessToFinish(25000);
        }
        cfg.deleteFile();
        bodyFile.deleteFile();

        const int nl = out.lastIndexOfChar('\n');
        if (nl < 0)
            return {};
        status = out.substring(nl + 1).trim().getIntValue();
        return out.substring(0, nl);
    }

    static juce::String httpForm(const juce::String& url, const juce::String& form, int& status)
    {
        return httpRaw(juce::StringArray { "-X", "POST",
                                           "-H", "Content-Type: application/x-www-form-urlencoded", url },
                       {}, form, status);
    }

    static juce::String httpJson(const juce::String& method, const juce::String& url,
                                 const juce::String& bearer, const juce::String& json, int& status)
    {
        juce::StringArray args { "-X", method, "-H", "Accept: application/json" };
        if (json.isNotEmpty())
            args.addArray({ "-H", "Content-Type: application/json" });
        args.add(url);
        return httpRaw(args, "header = \"Authorization: Bearer " + bearer + "\"\n", json, status);
    }

    // ---- small helpers ----------------------------------------------------

    static juce::String enc(const juce::String& s) { return juce::URL::addEscapeChars(s, true); }

    static juce::String base64Url(const void* data, size_t len)
    {
        juce::MemoryOutputStream out;
        juce::Base64::convertToBase64(out, data, len);
        return out.toString().replaceCharacter('+', '-').replaceCharacter('/', '_').removeCharacters("=");
    }

    static juce::String randomToken(int bytes)
    {
        std::vector<uint8_t> b((size_t) bytes);
        arc4random_buf(b.data(), b.size());
        return base64Url(b.data(), b.size());
    }

    static juce::StringPairArray parseQuery(const juce::String& query)
    {
        juce::StringPairArray out;
        for (const auto& pair : juce::StringArray::fromTokens(query.fromFirstOccurrenceOf("?", false, false), "&", ""))
            out.set(juce::URL::removeEscapeChars(pair.upToFirstOccurrenceOf("=", false, false)),
                    juce::URL::removeEscapeChars(pair.fromFirstOccurrenceOf("=", false, false)));
        return out;
    }

    static juce::String emailFromIdToken(const juce::String& jwt)
    {
        const auto parts = juce::StringArray::fromTokens(jwt, ".", "");
        if (parts.size() < 2)
            return {};
        auto b64 = parts[1].replaceCharacter('-', '+').replaceCharacter('_', '/');
        while (b64.length() % 4 != 0)
            b64 += "=";
        juce::MemoryOutputStream decoded;
        if (! juce::Base64::convertFromBase64(decoded, b64))
            return {};
        return juce::JSON::parse(decoded.toString()).getProperty("email", "").toString();
    }

    static juce::String describe(const juce::var& resp, int status)
    {
        auto e = resp.getProperty("error", juce::var());
        juce::String msg = e.isObject() ? e.getProperty("message", "").toString() : e.toString();
        const auto desc = resp.getProperty("error_description", "").toString();
        if (msg.isEmpty()) msg = desc;
        else if (desc.isNotEmpty()) msg += ": " + desc;
        if (msg.isEmpty())
            msg = status == 0 ? juce::String("no connection") : "HTTP " + juce::String(status);
        return msg;
    }

    static juce::String page(const juce::String& title, const juce::String& text)
    {
        return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
               "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>ListenLink</title><style>:root{color-scheme:dark}"
               "body{font-family:-apple-system,'Segoe UI',Helvetica,Arial,sans-serif;background:#101014;"
               "color:#e8e8ec;min-height:100vh;margin:0;display:flex;align-items:center;justify-content:center}"
               ".card{background:#1a1a21;border:1px solid #2a2a33;border-radius:16px;padding:40px 44px;"
               "width:min(420px,92vw);text-align:center}h1{font-size:20px;margin:0 0 8px}"
               "p{color:#8a8a96;font-size:14px;margin:0;line-height:1.5}</style></head><body>"
               "<div class=\"card\"><h1>" + title + "</h1><p>" + text + "</p></div></body></html>";
    }

    mutable juce::CriticalSection lock;
    juce::String refreshToken, accessToken, email, lastError;
    juce::int64 expiresAt = 0;
    juce::String pendingVerifier, pendingState, pendingRedirect;
    std::vector<NotesDoc> recent;
    std::atomic<bool> busy { false }, refreshing { false };
};
