#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <vector>

// Tiny embedded HTTP + WebSocket server.
//   GET /    -> listener web page
//   GET /ws  -> WebSocket upgrade; server then pushes 16-bit interleaved
//               stereo PCM as binary frames (plus a JSON "hello" text frame).
class StreamServer : private juce::Thread
{
public:
    StreamServer();
    ~StreamServer() override;

    bool startServer();
    void stopServer();

    bool isServerRunning() const noexcept   { return serverRunning.load(); }
    int getPort() const noexcept            { return port.load(); }
    int getNumListeners() const;

    void setSampleRate(double sr) noexcept  { sampleRate.store(sr); }
    double getSampleRate() const noexcept   { return sampleRate.load(); }

    // 0 = lossless PCM, 1 = Opus 256 kbps, 2 = Opus 128 kbps
    void setStreamMode(int m) noexcept      { streamMode.store(m); }
    int getStreamMode() const noexcept      { return streamMode.load(); }

    // Advertised to listeners in the hello message so their page can re-find
    // the stream via the link service after a tunnel restart.
    void setStreamId(const juce::String& s) { const juce::ScopedLock sl(idLock); streamId = s; }
    juce::String getStreamId() const        { const juce::ScopedLock sl(idLock); return streamId; }

    // Audio thread. Interleaved stereo floats, numFrames sample-frames.
    void pushAudio(const float* interleavedStereo, int numFrames);

private:
    struct Client
    {
        std::unique_ptr<juce::StreamingSocket> sock;
        std::vector<uint8_t> outbuf;
        size_t outPos = 0;
        double helloRate = 0.0;
        int helloMode = -1;
        bool wantsPcm = false;   // client asked for /ws?fmt=pcm (no browser Opus support)
        bool dead = false;
    };

    struct OpusState;

    class Broadcaster : public juce::Thread
    {
    public:
        explicit Broadcaster(StreamServer& o) : juce::Thread("ListenLink broadcast"), owner(o) {}
        void run() override { owner.broadcastLoop(*this); }
    private:
        StreamServer& owner;
    };

    void run() override;   // accept loop
    void broadcastLoop(juce::Thread& thread);
    void handleConnection(std::unique_ptr<juce::StreamingSocket> sock);
    void serviceClient(Client& c);
    void encodeOpus(const std::vector<float>& interleavedIn, double srcRate, int mode,
                    bool anyListener, std::vector<std::vector<uint8_t>>& out);

    static std::vector<uint8_t> buildWsFrame(uint8_t opcode, const void* data, size_t len);
    static juce::String makeAcceptKey(const juce::String& clientKey);
    std::vector<uint8_t> buildHelloFrame(double rate, int mode) const;

    juce::StreamingSocket listener;
    std::unique_ptr<Broadcaster> broadcaster;

    std::atomic<bool> serverRunning { false };
    std::atomic<int> port { 0 };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> streamMode { 0 };
    std::unique_ptr<OpusState> opusState;   // broadcast thread only

    juce::CriticalSection clientsLock;
    mutable juce::CriticalSection idLock;
    juce::String streamId;
    std::vector<std::unique_ptr<Client>> clients;

    int lastListenerCount = -1;   // broadcast thread only

    juce::AbstractFifo fifo { 1 << 19 };   // interleaved float samples
    std::vector<float> fifoBuffer;
    std::vector<float> readTmp;
    std::vector<int16_t> pcmTmp;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StreamServer)
};
