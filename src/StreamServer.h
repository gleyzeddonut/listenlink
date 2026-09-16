#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <memory>
#include <vector>

// Tiny embedded HTTP + WebSocket server.
//   GET /          -> listener web page
//   GET /ws        -> WebSocket upgrade; server then pushes 16-bit interleaved
//                     stereo PCM as binary frames (plus a JSON "hello" text frame).
//   GET /ws?doc=1  -> shared-doc channel: no audio. The page's collaborative
//                     notes (a Yjs CRDT) send opaque updates as JSON text frames
//                     {"d":"<base64>"}; the server appends each to an in-memory
//                     log, relays it to every other doc socket, and replays the
//                     whole log to newcomers as {"doc":[...]}. Nothing persists
//                     beyond this StreamServer's lifetime.
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

    // Gate for the whole share path: when off, new /ws upgrades are refused
    // and connected listeners are kicked. The warm (unregistered) tunnel can
    // then never carry audio — only Create Public Link opens the tap.
    void setSharing(bool on)
    {
        sharing.store(on);
        if (! on)
        {
            const juce::ScopedLock sl(clientsLock);
            for (auto& c : clients)
            {
                c->dead = true;
                if (c->sock != nullptr)
                    c->sock->close();
            }
        }
    }

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
        bool docOnly = false;    // /ws?doc=1: shared-doc channel, never receives audio
        bool docSynced = false;  // doc log has been replayed to this client
        bool dead = false;
        uint32_t id = 0;         // unique per connection (snapshot requests are addressed by it)

        // Inbound side. Audio clients only ever send close frames; doc clients
        // send text messages. Buffered until a whole frame is present.
        std::vector<uint8_t> inbuf;
        std::vector<uint8_t> fragBuf;   // fragmented message being reassembled
        uint8_t fragOpcode = 0;
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
    bool parseInbound(Client& c);   // false = protocol error / close frame -> drop client
    void handleClientMessage(Client& c, uint8_t opcode, const std::vector<uint8_t>& payload);
    void handleDocMessage(Client& c, const juce::String& text);
    void sendDocLog(Client& c);
    static void queueFrame(Client& c, const std::vector<uint8_t>& frame);
    static void queueText(Client& c, const juce::String& json);
    void encodeOpus(const std::vector<float>& interleavedIn, double srcRate, int mode,
                    bool anyListener, std::vector<std::vector<uint8_t>>& out);

    static std::vector<uint8_t> buildWsFrame(uint8_t opcode, const void* data, size_t len);
    static juce::String makeAcceptKey(const juce::String& clientKey);
    std::vector<uint8_t> buildHelloFrame(double rate, int mode) const;

    juce::StreamingSocket listener;
    std::unique_ptr<Broadcaster> broadcaster;

    std::atomic<bool> serverRunning { false };
    std::atomic<bool> sharing { false };
    std::atomic<int> port { 0 };
    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> streamMode { 0 };
    std::unique_ptr<OpusState> opusState;   // broadcast thread only

    juce::CriticalSection clientsLock;
    mutable juce::CriticalSection idLock;
    juce::String streamId;
    std::vector<std::unique_ptr<Client>> clients;

    int lastListenerCount = -1;   // broadcast thread only

    // Shared doc: append-only log of base64 Yjs updates (broadcast thread only).
    // When it grows past a threshold, one client is asked for a snapshot of the
    // whole document and the log collapses to that snapshot plus whatever
    // arrived after the request went out (Yjs updates are idempotent, so the
    // overlap is harmless).
    std::vector<juce::String> docLog;
    size_t docLogBytes = 0;
    size_t docCompactAt = 0;           // next compaction threshold (grows with the doc)
    size_t docSnapMark = 0;
    uint32_t docSnapRequestedAt = 0;   // ms tick; 0 = no snapshot request pending
    uint32_t docSnapClientId = 0;      // only this client's snapshot is accepted
    std::vector<uint8_t> docReplayFrame;   // cached {"doc":[...]} frame; empty = stale
    uint32_t nextClientId = 1;         // accept thread only

    juce::AbstractFifo fifo { 1 << 19 };   // interleaved float samples
    std::vector<float> fifoBuffer;
    std::vector<float> readTmp;
    std::vector<int16_t> pcmTmp;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StreamServer)
};
