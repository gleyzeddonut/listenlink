#pragma once

#include <JuceHeader.h>
#include "StreamServer.h"

// Runs `cloudflared tunnel --url http://127.0.0.1:<port>` and scrapes the
// public https://*.trycloudflare.com URL from its output.
class TunnelManager : private juce::Thread
{
public:
    TunnelManager() : juce::Thread("ListenLink tunnel") {}
    ~TunnelManager() override { stopTunnel(); }

    void startTunnel(int localPort)
    {
        stopTunnel();
        {
            const juce::ScopedLock sl(lock);
            publicUrl.clear();
            status = "Starting tunnel...";
        }
        port = localPort;
        shouldRun.store(true);
        startThread();
    }

    void stopTunnel()
    {
        shouldRun.store(false);
        signalThreadShouldExit();
        proc.kill();
        stopThread(3000);
        const juce::ScopedLock sl(lock);
        publicUrl.clear();
        status = "";
    }

    bool isTunnelActive() const { return isThreadRunning() && shouldRun.load(); }

    juce::String getPublicUrl() const  { const juce::ScopedLock sl(lock); return publicUrl; }
    juce::String getStatus() const     { const juce::ScopedLock sl(lock); return status; }

    static juce::String findCloudflared()
    {
        for (const char* p : { "/opt/homebrew/bin/cloudflared",
                               "/usr/local/bin/cloudflared",
                               "/opt/local/bin/cloudflared",
                               "/usr/bin/cloudflared" })
            if (juce::File(p).existsAsFile())
                return p;
        return {};
    }

private:
    void run() override
    {
        const auto exe = findCloudflared();
        if (exe.isEmpty())
        {
            const juce::ScopedLock sl(lock);
            status = "cloudflared not found - run: brew install cloudflared";
            return;
        }

        juce::StringArray args { exe, "tunnel", "--url", "http://127.0.0.1:" + juce::String(port) };
        if (! proc.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            const juce::ScopedLock sl(lock);
            status = "Failed to launch cloudflared";
            return;
        }

        juce::String collected;
        char buf[2048];

        while (! threadShouldExit() && proc.isRunning())
        {
            const int n = proc.readProcessOutput(buf, (int) sizeof(buf));
            if (n > 0)
            {
                collected += juce::String::fromUTF8(buf, n);
                if (getPublicUrl().isEmpty())
                {
                    const int end = collected.indexOf(".trycloudflare.com");
                    if (end >= 0)
                    {
                        const int start = collected.substring(0, end).lastIndexOf("https://");
                        if (start >= 0)
                        {
                            const juce::ScopedLock sl(lock);
                            publicUrl = collected.substring(start, end) + ".trycloudflare.com";
                            status = "Public link active";
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
        }

        proc.kill();

        if (shouldRun.load() && getPublicUrl().isEmpty())
        {
            const juce::ScopedLock sl(lock);
            status = "Tunnel exited before a URL appeared - is the internet up?";
        }
    }

    juce::ChildProcess proc;
    std::atomic<bool> shouldRun { false };
    int port = 0;
    mutable juce::CriticalSection lock;
    juce::String publicUrl, status;
};

// ---------------------------------------------------------------------------

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
        juce::MemoryOutputStream(dest, true).writeInt(qualityParam->getIndex());
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        if (sizeInBytes >= 4)
        {
            juce::MemoryInputStream in(data, (size_t) sizeInBytes, false);
            *qualityParam = in.readInt();
        }
    }

    StreamServer server;
    TunnelManager tunnel;

    // 0 = Lossless PCM, 1 = Opus 256 kbps, 2 = Opus 128 kbps
    juce::AudioParameterChoice* qualityParam = nullptr;

    // Block peaks; the editor reads-and-resets these with exchange(0).
    std::atomic<float> peakLeft { 0.0f }, peakRight { 0.0f };

private:
    std::vector<float> interleaved;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ListenLinkProcessor)
};
