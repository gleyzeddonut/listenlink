#pragma once

#include <JuceHeader.h>
#include "StreamServer.h"

// Stream identity for the link service at gggaudio.store/l/. The id becomes
// the shareable short link; the token proves ownership when re-registering a
// new tunnel URL. One identity per plugin INSTANCE, persisted in plugin state,
// so each project keeps its own stable link and two projects streaming at
// once never fight over one registry entry.
struct StreamIdentity
{
    juce::String id, token;

    bool isValid() const
    {
        return id.length() >= 6 && id.length() <= 32
            && id.containsOnly("abcdefghijklmnopqrstuvwxyz0123456789")
            && token.length() >= 16 && token.length() <= 128;
    }

    static StreamIdentity generate()
    {
        StreamIdentity ident;
        auto& rng = juce::Random::getSystemRandom();
        const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        for (int i = 0; i < 10; ++i)
            ident.id += alphabet[rng.nextInt(36)];
        ident.token = juce::Uuid().toString().removeCharacters("-")
                    + juce::Uuid().toString().removeCharacters("-");
        return ident;
    }
};

// Runs `cloudflared tunnel --url http://127.0.0.1:<port>` and scrapes the
// public https://*.trycloudflare.com URL from its output.
class TunnelManager : private juce::Thread
{
public:
    TunnelManager() : juce::Thread("ListenLink tunnel") {}
    ~TunnelManager() override { stopTunnel(); }

    void setIdentity(const StreamIdentity& ident)
    {
        const juce::ScopedLock sl(lock);
        identity = ident;
    }

    // Keep a quick tunnel running in the background (unregistered, so nothing
    // is shared) — Create Public Link then only has to register the URL, and
    // the fresh hostname has usually finished propagating through DNS before
    // anyone opens the link.
    void warmUp(int localPort)
    {
        if (localPort <= 0)   // server never came up: nothing to tunnel to
            return;
        if (isThreadRunning() && shouldRun.load())
            return;
        port = localPort;
        shouldRun.store(true);
        startThread();
    }

    // "Create Public Link": the short link is deterministic, so show it at
    // once; the warm tunnel URL (or the next one scraped) gets registered by
    // the tunnel thread. Early clicks land on the offline page, which flips
    // to the stream as soon as registration lands.
    void activate(int localPort)
    {
        {
            const juce::ScopedLock sl(lock);
            publicUrl = identity.id.isNotEmpty()
                ? juce::String(kLinkService) + "/" + identity.id : juce::String();
            status = "Starting tunnel...";
            active.store(true);
        }
        needsRegister.store(true);
        warmUp(localPort);
    }

    // "Stop": unshare the link, but keep the tunnel warm for the next Create.
    // active flips inside the lock so registerAndPublish can never publish
    // into a stopped state.
    void deactivate()
    {
        needsRegister.store(false);
        bool wasRegistered;
        juce::String id, tok;
        {
            const juce::ScopedLock sl(lock);
            active.store(false);
            wasRegistered = registered.exchange(false);
            id = identity.id;
            tok = identity.token;
            publicUrl.clear();
            status = "";
        }
        if (wasRegistered)
        {
            // Through the tunnel thread when it's alive: a detached unregister
            // racing a quick re-Create's register can arrive late at the
            // Worker and delete the fresh registration.
            if (isThreadRunning() && shouldRun.load())
                needsUnregister.store(true);
            else
                unregisterAsync(id, tok);
        }
    }

    void stopTunnel()
    {
        active.store(false);
        needsRegister.store(false);
        shouldRun.store(false);
        signalThreadShouldExit();
        proc.kill();
        stopThread(3000);
        juce::String id, tok;
        {
            const juce::ScopedLock sl(lock);
            id = identity.id;
            tok = identity.token;
            publicUrl.clear();
            tunnelUrl.clear();
            status = "";
        }
        // Also drain an unregister the (now stopped) thread never got to.
        if (registered.exchange(false) || needsUnregister.exchange(false))
            unregisterAsync(id, tok);
    }

    bool isSharing() const    { return active.load(); }
    bool isRegistered() const { return registered.load(); }

    juce::String getPublicUrl() const  { const juce::ScopedLock sl(lock); return publicUrl; }
    juce::String getStatus() const     { const juce::ScopedLock sl(lock); return status; }

    static juce::File downloadedBinary()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Application Support/ListenLink/cloudflared");
    }

    // cloudflared silently loads ~/.cloudflared/config.yml even for quick
    // tunnels, and a named tunnel's ingress rules there take precedence over
    // our --url: the quick tunnel's hostname matches none of them, so every
    // listener request falls through to the catch-all (typically
    // http_status:404). Seen in the wild 2026-09-10 on a machine that also ran
    // an unrelated named tunnel. Pointing --config at our own minimal file
    // keeps the user's config out of the picture. /dev/null works too (with a
    // harmless "file was empty" log line) and is the fallback if we can't write.
    static juce::String isolatedConfigPath()
    {
        const auto file = downloadedBinary().getSiblingFile("cloudflared-config.yml");
        const juce::String contents =
            "# Written by ListenLink. Passed to cloudflared via --config so a user-level\n"
            "# ~/.cloudflared/config.yml (named-tunnel ingress rules) cannot hijack the\n"
            "# quick tunnel. Keep this file minimal.\n"
            "no-autoupdate: true\n";
        if (file.existsAsFile() && file.loadFileAsString() == contents)
            return file.getFullPathName();
        if (file.getParentDirectory().createDirectory() && file.replaceWithText(contents))
            return file.getFullPathName();
        return "/dev/null";
    }

    static juce::String findCloudflared()
    {
        for (const char* p : { "/opt/homebrew/bin/cloudflared",
                               "/usr/local/bin/cloudflared",
                               "/opt/local/bin/cloudflared",
                               "/usr/bin/cloudflared" })
            if (juce::File(p).existsAsFile())
                return p;
        if (downloadedBinary().existsAsFile())
            return downloadedBinary().getFullPathName();
        return {};
    }

    // One-time download of the official cloudflared build (~17 MB) into
    // Application Support. Blocking; safe from any thread — concurrent callers
    // collapse into a single download.
    static bool ensureCloudflared()
    {
        if (findCloudflared().isNotEmpty())
            return true;

        static std::atomic<bool> busy { false };
        if (busy.exchange(true))
        {
            while (busy.load())
                juce::Thread::sleep(200);
            return findCloudflared().isNotEmpty();
        }

        const auto dir = downloadedBinary().getParentDirectory();
        dir.createDirectory();
        const auto tgz = dir.getChildFile("cloudflared.tgz");

       #if defined(__aarch64__)
        const char* url = "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-darwin-arm64.tgz";
       #else
        const char* url = "https://github.com/cloudflare/cloudflared/releases/latest/download/cloudflared-darwin-amd64.tgz";
       #endif

        bool ok = false;
        juce::ChildProcess curl;
        if (curl.start(juce::StringArray { "/usr/bin/curl", "-fsSL", "--connect-timeout", "15",
                                           "-o", tgz.getFullPathName(), url })
            && curl.waitForProcessToFinish(180000) && curl.getExitCode() == 0)
        {
            juce::ChildProcess tar;
            ok = tar.start(juce::StringArray { "/usr/bin/tar", "-xzf", tgz.getFullPathName(),
                                               "-C", dir.getFullPathName() })
                 && tar.waitForProcessToFinish(30000) && tar.getExitCode() == 0
                 && downloadedBinary().existsAsFile()
                 && downloadedBinary().setExecutePermission(true);
        }
        tgz.deleteFile();
        busy.store(false);
        return ok;
    }

private:
    void run() override
    {
        // Keep retrying the one-time download instead of exiting: a dead
        // thread here would leave a later Create stuck at "Starting tunnel...".
        while (findCloudflared().isEmpty() && shouldRun.load() && ! threadShouldExit())
        {
            if (active.load())
            {
                const juce::ScopedLock sl(lock);
                status = "One-time setup: downloading tunnel engine (~17 MB)...";
            }
            if (ensureCloudflared())
                break;
            if (active.load())
            {
                const juce::ScopedLock sl(lock);
                status = "Couldn't download the tunnel engine - retrying...";
            }
            for (int waited = 0; waited < 15000 && ! threadShouldExit(); waited += 100)
                juce::Thread::sleep(100);
        }

        const auto exe = findCloudflared();
        if (exe.isEmpty())
            return;   // only reachable when told to exit mid-download

        // Self-healing: if cloudflared dies mid-session, relaunch it (fresh
        // quick tunnels get a new URL) with capped backoff instead of leaving
        // a dead link on screen.
        for (int attempt = 0; shouldRun.load() && ! threadShouldExit(); ++attempt)
        {
            if (attempt > 0)
            {
                {
                    // Keep showing the short link while healing — it stays the
                    // valid thing to share. Only a raw tunnel URL goes stale.
                    const juce::ScopedLock sl(lock);
                    tunnelUrl.clear();
                    if (active.load())
                    {
                        if (! registered.load())
                            publicUrl.clear();
                        status = "Tunnel dropped - reconnecting...";
                    }
                }
                const int backoffMs = juce::jmin(2000 * (1 << juce::jmin(attempt - 1, 4)), 30000);
                for (int waited = 0; waited < backoffMs && ! threadShouldExit(); waited += 100)
                    juce::Thread::sleep(100);
                if (threadShouldExit())
                    break;
            }

            juce::StringArray args { exe, "--config", isolatedConfigPath(),
                                     "tunnel", "--no-autoupdate",
                                     "--url", "http://127.0.0.1:" + juce::String(port) };
            if (! proc.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            {
                if (active.load())
                {
                    const juce::ScopedLock sl(lock);
                    status = "Failed to launch cloudflared";
                }
                return;
            }

            juce::String collected;
            char buf[2048];
            bool urlFound = false;   // NOT getPublicUrl().isEmpty() — that now
                                     // holds the short link before the scrape

            while (! threadShouldExit() && proc.isRunning())
            {
                const int n = proc.readProcessOutput(buf, (int) sizeof(buf));
                if (n > 0)
                {
                    // Only accumulate while still scraping — cloudflared logs
                    // for the whole session and this would otherwise grow
                    // without bound. (Reads continue so the pipe stays drained.)
                    if (! urlFound)
                    {
                        collected += juce::String::fromUTF8(buf, n);
                        const int end = collected.indexOf(".trycloudflare.com");
                        if (end >= 0)
                        {
                            const int start = collected.substring(0, end).lastIndexOf("https://");
                            if (start >= 0)
                            {
                                const auto scraped = collected.substring(start, end) + ".trycloudflare.com";
                                urlFound = true;
                                attempt = 0;   // healthy again: future drops back off from scratch
                                {
                                    const juce::ScopedLock sl(lock);
                                    tunnelUrl = scraped;
                                }
                                if (active.load())
                                    needsRegister.store(true);
                            }
                        }
                        if (collected.length() > 65536)
                            collected = collected.substring(collected.length() - 8192);
                    }
                }
                else
                {
                    juce::Thread::sleep(100);
                }

                // Unregister first, register second, both on THIS thread, so
                // a Stop -> Create cycle can never have its stale unregister
                // outrun the fresh registration at the Worker.
                if (needsUnregister.exchange(false))
                {
                    juce::String id, tok;
                    {
                        const juce::ScopedLock sl(lock);
                        id = identity.id;
                        tok = identity.token;
                    }
                    unregisterBlocking(id, tok);
                }

                // Registration is decoupled from the scrape: activate() can
                // request it any time after warm-up, and a re-scrape while
                // sharing (tunnel healed) re-requests it.
                if (urlFound && active.load() && needsRegister.exchange(false))
                {
                    juce::String u;
                    {
                        const juce::ScopedLock sl(lock);
                        u = tunnelUrl;
                    }
                    if (u.isNotEmpty())
                        registerAndPublish(u);
                }
            }

            proc.kill();
        }

        if (shouldRun.load() && active.load() && ! registered.load())
        {
            const juce::ScopedLock sl(lock);
            status = "Tunnel exited before a URL appeared - is the internet up?";
        }
    }

    // Blocking; runs on the tunnel thread. Don't show the raw tunnel URL
    // while registering — a link that swaps out from under the user invites
    // copying the wrong one. Reveal only the final link; the tunnel URL
    // appears solely if registration fails.
    void registerAndPublish(const juce::String& url)
    {
        juce::String id, tok;
        {
            const juce::ScopedLock sl(lock);
            id = identity.id;
            tok = identity.token;
        }
        const bool useShortLink = registerLink(id, tok, url);
        {
            const juce::ScopedLock sl(lock);
            if (active.load())
            {
                registered.store(useShortLink);
                publicUrl = useShortLink
                    ? juce::String(kLinkService) + "/" + id
                    : url;   // link service unreachable: raw URL is the only working link
                status = "Public link active";
                return;
            }
        }
        // Stop was pressed mid-registration: leave nothing behind. Blocking
        // (we're on the tunnel thread) so it can't outrun a later register.
        if (useShortLink)
            unregisterBlocking(id, tok);
    }

    // Register the current tunnel URL with the link service so the user's
    // stable short link (and reconnecting listener pages) can find it.
    static bool registerLink(const juce::String& id, const juce::String& tok,
                             const juce::String& url)
    {
        if (id.isEmpty() || tok.isEmpty())
            return false;
        juce::ChildProcess curl;
        if (! curl.start(juce::StringArray { "/usr/bin/curl", "-fsS", "-o", "/dev/null", "-m", "15",
                "-X", "POST", juce::String(kLinkService) + "/register",
                "-H", "Content-Type: application/json",
                "-d", "{\"id\":\"" + id + "\",\"token\":\"" + tok
                      + "\",\"url\":\"" + url + "\"}" }))
            return false;
        return curl.waitForProcessToFinish(20000) && curl.getExitCode() == 0;
    }

    static void unregisterBlocking(const juce::String& id, const juce::String& tok)
    {
        juce::ChildProcess curl;
        if (curl.start(juce::StringArray { "/usr/bin/curl", "-fsS", "-o", "/dev/null", "-m", "15",
                "-X", "POST", juce::String(kLinkService) + "/unregister",
                "-H", "Content-Type: application/json",
                "-d", "{\"id\":\"" + id + "\",\"token\":\"" + tok + "\"}" }))
            curl.waitForProcessToFinish(20000);
    }

    static void unregisterAsync(const juce::String& id, const juce::String& tok)
    {
        juce::Thread::launch([id, tok] { unregisterBlocking(id, tok); });
    }

    static constexpr const char* kLinkService = "https://gggaudio.store/l";

    StreamIdentity identity;
    std::atomic<bool> registered { false };
    std::atomic<bool> active { false };           // user wants the link shared
    std::atomic<bool> needsRegister { false };    // tunnel thread owes a registration
    std::atomic<bool> needsUnregister { false };  // tunnel thread owes an unregistration
    juce::ChildProcess proc;
    std::atomic<bool> shouldRun { false };
    int port = 0;
    mutable juce::CriticalSection lock;
    juce::String publicUrl, status, tunnelUrl;  // tunnelUrl: latest scraped raw URL
};

// ---------------------------------------------------------------------------

// Checks GitHub once per process for a newer release tag.
class UpdateChecker
{
public:
    static void checkAsync()
    {
        static std::atomic<bool> started { false };
        if (started.exchange(true))
            return;

        juce::Thread::launch([]
        {
            juce::ChildProcess curl;
            if (! curl.start(juce::StringArray {
                    "/usr/bin/curl", "-fsSL", "--connect-timeout", "10", "-m", "20",
                    "https://api.github.com/repos/gleyzeddonut/listenlink/releases/latest" }))
                return;

            const auto tag = juce::JSON::parse(curl.readAllProcessOutput())
                                 .getProperty("tag_name", {})
                                 .toString().trim();

            if (tag.startsWithChar('v') && isNewer(tag.substring(1), JucePlugin_VersionString))
            {
                const juce::ScopedLock sl(lock());
                latest() = tag;
            }
        });
    }

    // Newer release tag (e.g. "v0.3.0"), or empty if up to date / not checked yet.
    static juce::String getAvailableUpdate()
    {
        const juce::ScopedLock sl(lock());
        return latest();
    }

    // Where the update button lands. Baked into shipped binaries, so this URL
    // must stay alive forever and must never become a paywalled page — the
    // site's download page auto-serves the newest installer.
    static constexpr const char* releasePageUrl =
        "https://gggaudio.store/listenlink/download/";

private:
    static juce::CriticalSection& lock()  { static juce::CriticalSection l; return l; }
    static juce::String& latest()         { static juce::String s; return s; }

    static bool isNewer(const juce::String& remote, const juce::String& local)
    {
        const auto ra = juce::StringArray::fromTokens(remote, ".", "");
        const auto la = juce::StringArray::fromTokens(local, ".", "");
        for (int i = 0; i < juce::jmax(ra.size(), la.size()); ++i)
        {
            const int r = i < ra.size() ? ra[i].getIntValue() : 0;
            const int l = i < la.size() ? la[i].getIntValue() : 0;
            if (r != l)
                return r > l;
        }
        return false;
    }
};

class ListenLinkProcessor : public juce::AudioProcessor
{
public:
    ListenLinkProcessor();
    ~ListenLinkProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                       { return true; }

    const juce::String getName() const override           { return "ListenLink"; }
    bool acceptsMidi() const override                     { return false; }
    bool producesMidi() const override                    { return false; }
    double getTailLengthSeconds() const override          { return 0.0; }

    int getNumPrograms() override                         { return 1; }
    int getCurrentProgram() override                      { return 0; }
    void setCurrentProgram(int) override                  {}
    const juce::String getProgramName(int) override       { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& dest) override
    {
        juce::MemoryOutputStream out(dest, true);
        out.writeInt(qualityParam->getIndex());
        out.writeString(identity.id);
        out.writeString(identity.token);
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        if (sizeInBytes >= 4)
        {
            juce::MemoryInputStream in(data, (size_t) sizeInBytes, false);
            *qualityParam = in.readInt();

            // Older saves end after the int; keep the generated identity then.
            // A warm (unshared) tunnel is fine to adopt over — only an
            // actively shared link must keep its identity.
            StreamIdentity saved { in.readString(), in.readString() };
            if (saved.isValid() && ! tunnel.isSharing())
            {
                identity = saved;
                tunnel.setIdentity(identity);
                server.setStreamId(identity.id);
            }
        }
    }

    StreamServer server;
    TunnelManager tunnel;

    // 0 = Lossless PCM, 1 = Opus 256 kbps, 2 = Opus 128 kbps
    juce::AudioParameterChoice* qualityParam = nullptr;

    // Block peaks; the editor reads-and-resets these with exchange(0).
    std::atomic<float> peakLeft { 0.0f }, peakRight { 0.0f };

    // This instance's short-link identity (persisted in plugin state).
    StreamIdentity identity;

private:
    std::vector<float> interleaved;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ListenLinkProcessor)
};
