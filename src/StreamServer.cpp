#include "StreamServer.h"
#include "WebPage.h"

#include <CommonCrypto/CommonDigest.h>
#include <opus.h>

static constexpr int kFirstPort = 17654;
static constexpr int kLastPort  = 17663;
static constexpr int kOpusRate  = 48000;
static constexpr int kOpusFrame = 960;   // 20 ms @ 48 kHz

// Inbound WebSocket limits. A doc snapshot of even a very long lyric sheet is
// well under 1 MB of base64; anything bigger is not a browser we want to talk to.
static constexpr size_t kMaxInboundMessage = 2u << 20;   // one message (after reassembly)
static constexpr size_t kMaxInboundBuffer  = 4u << 20;   // unparsed bytes per client
static constexpr size_t kMaxDrainPerTick   = 256u << 10; // bytes read per client per 10 ms tick
static constexpr size_t kMaxDocEntry       = 1u << 20;   // one base64 update or snapshot
static constexpr size_t kMaxOutBacklog     = 1u << 20;   // unsent bytes before a doc socket is dropped
static constexpr size_t kDocCompactBytes   = 96u << 10;  // first compaction threshold
static constexpr uint32_t kDocSnapTimeoutMs = 15000;

// All encoding state lives on the broadcast thread.
struct StreamServer::OpusState
{
    OpusEncoder* enc = nullptr;
    int bitrate = 0;
    double srcRate = 0.0;
    juce::LagrangeInterpolator interpL, interpR;
    std::vector<float> inL, inR, outL, outR;
    std::vector<float> accum;   // interleaved stereo @48k awaiting a full frame

    void clearBuffers()
    {
        inL.clear(); inR.clear(); accum.clear();
        interpL.reset(); interpR.reset();
    }

    ~OpusState()
    {
        if (enc != nullptr)
            opus_encoder_destroy(enc);
    }
};

StreamServer::StreamServer() : juce::Thread("ListenLink accept")
{
    fifoBuffer.resize((size_t) fifo.getTotalSize());
}

StreamServer::~StreamServer()
{
    stopServer();
}

bool StreamServer::startServer()
{
    if (serverRunning.load())
        return true;

    for (int p = kFirstPort; p <= kLastPort; ++p)
    {
        if (listener.createListener(p))
        {
            port.store(p);
            serverRunning.store(true);
            broadcaster = std::make_unique<Broadcaster>(*this);
            broadcaster->startThread();
            startThread();
            return true;
        }
    }
    return false;
}

void StreamServer::stopServer()
{
    if (! serverRunning.exchange(false))
        return;

    signalThreadShouldExit();
    listener.close();          // unblocks waitForNextConnection
    stopThread(3000);

    if (broadcaster != nullptr)
    {
        broadcaster->stopThread(3000);
        broadcaster = nullptr;
    }

    const juce::ScopedLock sl(clientsLock);
    clients.clear();
}

int StreamServer::getNumListeners() const
{
    const juce::ScopedLock sl(clientsLock);
    int n = 0;
    for (auto& c : clients)
        if (! c->docOnly)
            ++n;
    return n;
}

void StreamServer::pushAudio(const float* data, int numFrames)
{
    const int n = numFrames * 2;
    int s1, n1, s2, n2;
    fifo.prepareToWrite(n, s1, n1, s2, n2);
    if (n1 > 0) std::memcpy(fifoBuffer.data() + s1, data, (size_t) n1 * sizeof(float));
    if (n2 > 0) std::memcpy(fifoBuffer.data() + s2, data + n1, (size_t) n2 * sizeof(float));
    fifo.finishedWrite(n1 + n2);
}

// ---------------------------------------------------------------------------
// accept loop

void StreamServer::run()
{
    while (! threadShouldExit())
    {
        auto* raw = listener.waitForNextConnection();
        if (raw == nullptr)
        {
            if (threadShouldExit())
                break;
            juce::Thread::sleep(50);
            continue;
        }
        handleConnection(std::unique_ptr<juce::StreamingSocket>(raw));
    }
}

void StreamServer::handleConnection(std::unique_ptr<juce::StreamingSocket> sock)
{
    // Read the HTTP request (headers only).
    juce::String request;
    {
        juce::MemoryBlock data;
        char buf[4096];
        const auto deadline = juce::Time::getMillisecondCounter() + 3000;

        while (juce::Time::getMillisecondCounter() < deadline && data.getSize() < 16384)
        {
            if (sock->waitUntilReady(true, 200) != 1)
            {
                if (threadShouldExit()) return;
                continue;
            }
            const int n = sock->read(buf, (int) sizeof(buf), false);
            if (n <= 0)
                break;
            data.append(buf, (size_t) n);
            if (juce::String::createStringFromData(data.getData(), (int) data.getSize())
                    .contains("\r\n\r\n"))
                break;
        }
        request = juce::String::createStringFromData(data.getData(), (int) data.getSize());
    }

    if (request.isEmpty())
        return;

    const auto firstLine = request.upToFirstOccurrenceOf("\r\n", false, false);
    const auto target = firstLine.fromFirstOccurrenceOf(" ", false, false)
                                 .upToFirstOccurrenceOf(" ", false, false);
    const auto path = target.upToFirstOccurrenceOf("?", false, false);

    if (path == "/ws")
    {
        if (! sharing.load())
        {
            const juce::String resp = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sock->write(resp.toRawUTF8(), (int) resp.getNumBytesAsUTF8());
            return;
        }

        // WebSocket upgrade
        juce::String key;
        for (const auto& line : juce::StringArray::fromLines(request))
            if (line.startsWithIgnoreCase("Sec-WebSocket-Key:"))
                key = line.fromFirstOccurrenceOf(":", false, false).trim();

        if (key.isEmpty())
            return;

        const auto response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + makeAcceptKey(key) + "\r\n\r\n";
        sock->write(response.toRawUTF8(), (int) response.getNumBytesAsUTF8());

        auto client = std::make_unique<Client>();
        client->sock = std::move(sock);
        client->wantsPcm = target.contains("fmt=pcm");
        client->docOnly = target.contains("doc=1");
        client->id = nextClientId++;
        // helloRate/helloMode stay unset; the broadcast loop sends the hello
        // (with the right codec for this client) on its next tick.

        const juce::ScopedLock sl(clientsLock);
        clients.push_back(std::move(client));
        return;
    }

    if (path == "/" || path == "/index.html")
    {
        // fromUTF8, not the char* constructor: that one reads bytes as Latin-1
        // and would double-encode any non-ASCII in the page on the way out.
        const auto body = juce::String::fromUTF8(kListenerPage);
        const auto response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " + juce::String((int) body.getNumBytesAsUTF8()) + "\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n" + body;
        sock->write(response.toRawUTF8(), (int) response.getNumBytesAsUTF8());
        return;
    }

    const juce::String notFound = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    sock->write(notFound.toRawUTF8(), (int) notFound.getNumBytesAsUTF8());
}

// ---------------------------------------------------------------------------
// broadcast loop

void StreamServer::broadcastLoop(juce::Thread& thread)
{
    while (! thread.threadShouldExit())
    {
        thread.wait(10);

        // Drain the FIFO (whether or not anyone is listening, so it never backs up).
        int ready = fifo.getNumReady();
        ready -= ready % 2;
        const int maxChunk = 9600;   // 100 ms of stereo @48k per iteration
        if (ready > maxChunk)
            ready = maxChunk;

        readTmp.resize((size_t) juce::jmax(0, ready));
        if (ready > 0)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead(ready, s1, n1, s2, n2);
            if (n1 > 0) std::memcpy(readTmp.data(), fifoBuffer.data() + s1, (size_t) n1 * sizeof(float));
            if (n2 > 0) std::memcpy(readTmp.data() + n1, fifoBuffer.data() + s2, (size_t) n2 * sizeof(float));
            fifo.finishedRead(n1 + n2);
        }

        const int mode = streamMode.load();
        const double srcRate = sampleRate.load();

        bool needPcm = (mode == 0), needOpus = false;
        {
            const juce::ScopedLock sl(clientsLock);
            for (auto& c : clients)
            {
                if (c->docOnly) continue;
                if (c->wantsPcm) needPcm = true;
                else if (mode != 0) needOpus = true;
            }
        }

        std::vector<uint8_t> pcmFrame;
        if (needPcm && ! readTmp.empty())
        {
            pcmTmp.resize(readTmp.size());
            for (size_t i = 0; i < readTmp.size(); ++i)
            {
                const float v = juce::jlimit(-1.0f, 1.0f, readTmp[i]);
                pcmTmp[i] = (int16_t) juce::roundToInt(v * 32767.0f);
            }
            pcmFrame = buildWsFrame(0x02, pcmTmp.data(), pcmTmp.size() * sizeof(int16_t));
        }

        std::vector<std::vector<uint8_t>> opusFrames;
        if (mode != 0)
            encodeOpus(readTmp, srcRate, mode, needOpus, opusFrames);
        else if (opusState != nullptr)
            opusState->clearBuffers();   // don't hold stale audio across a mode switch

        const juce::ScopedLock sl(clientsLock);
        for (auto& c : clients)
        {
            if (c->docOnly)
            {
                if (! c->docSynced)
                    sendDocLog(*c);
                serviceClient(*c);
                continue;
            }

            const bool pcmClient = (mode == 0) || c->wantsPcm;
            const double clientRate = pcmClient ? srcRate : (double) kOpusRate;
            const int clientMode = pcmClient ? 0 : mode;

            if (clientRate != c->helloRate || clientMode != c->helloMode)
            {
                c->helloRate = clientRate;
                c->helloMode = clientMode;
                const auto hello = buildHelloFrame(clientRate, clientMode);
                c->outbuf.insert(c->outbuf.end(), hello.begin(), hello.end());
            }

            const bool hasRoom = c->outbuf.size() - c->outPos < (size_t) (1 << 20);
            if (hasRoom)
            {
                if (pcmClient)
                {
                    if (! pcmFrame.empty())
                        c->outbuf.insert(c->outbuf.end(), pcmFrame.begin(), pcmFrame.end());
                }
                else
                {
                    for (const auto& f : opusFrames)
                        c->outbuf.insert(c->outbuf.end(), f.begin(), f.end());
                }
            }

            serviceClient(*c);
        }

        // The client asked for a doc snapshot left before answering: stop
        // waiting on it so the next update can ask someone else.
        for (auto& c : clients)
            if (c->dead && docSnapRequestedAt != 0 && c->id == docSnapClientId)
                docSnapRequestedAt = 0;

        clients.erase(std::remove_if(clients.begin(), clients.end(),
                                     [](const std::unique_ptr<Client>& c) { return c->dead; }),
                      clients.end());

        // Tell everyone how many listeners there are whenever the count changes.
        // Doc-only sockets are not listeners (the same person may hold both).
        int count = 0;
        for (auto& c : clients)
            if (! c->docOnly)
                ++count;
        if (count != lastListenerCount)
        {
            lastListenerCount = count;
            const auto json = "{\"listeners\":" + juce::String(count) + "}";
            const auto countFrame = buildWsFrame(0x01, json.toRawUTF8(), json.getNumBytesAsUTF8());
            for (auto& c : clients)
                c->outbuf.insert(c->outbuf.end(), countFrame.begin(), countFrame.end());
        }
    }
}

void StreamServer::encodeOpus(const std::vector<float>& interleavedIn, double srcRate, int mode,
                              bool anyListener, std::vector<std::vector<uint8_t>>& out)
{
    if (opusState == nullptr)
        opusState = std::make_unique<OpusState>();
    auto& o = *opusState;

    if (o.enc == nullptr)
    {
        int err = OPUS_OK;
        o.enc = opus_encoder_create(kOpusRate, 2, OPUS_APPLICATION_AUDIO, &err);
        if (err != OPUS_OK)
        {
            o.enc = nullptr;
            return;
        }
    }

    const int bitrate = mode == 1 ? 256000 : 128000;
    if (bitrate != o.bitrate)
    {
        o.bitrate = bitrate;
        opus_encoder_ctl(o.enc, OPUS_SET_BITRATE(bitrate));
    }

    if (srcRate != o.srcRate)
    {
        o.srcRate = srcRate;
        o.clearBuffers();
    }

    const int inFrames = (int) interleavedIn.size() / 2;

    if (srcRate == (double) kOpusRate)
    {
        o.accum.insert(o.accum.end(), interleavedIn.begin(), interleavedIn.end());
    }
    else
    {
        for (int i = 0; i < inFrames; ++i)
        {
            o.inL.push_back(interleavedIn[(size_t) i * 2]);
            o.inR.push_back(interleavedIn[(size_t) i * 2 + 1]);
        }

        // Resample to 48k, leaving a few samples of history for the interpolator.
        const double ratio = srcRate / (double) kOpusRate;
        const int numOut = (int) (((double) o.inL.size() - 8.0) / ratio);
        if (numOut > 0)
        {
            o.outL.resize((size_t) numOut);
            o.outR.resize((size_t) numOut);
            const int usedL = o.interpL.process(ratio, o.inL.data(), o.outL.data(), numOut);
            const int usedR = o.interpR.process(ratio, o.inR.data(), o.outR.data(), numOut);
            o.inL.erase(o.inL.begin(), o.inL.begin() + usedL);
            o.inR.erase(o.inR.begin(), o.inR.begin() + usedR);
            for (int i = 0; i < numOut; ++i)
            {
                o.accum.push_back(o.outL[(size_t) i]);
                o.accum.push_back(o.outR[(size_t) i]);
            }
        }
    }

    unsigned char packet[4000];
    while (o.accum.size() >= (size_t) kOpusFrame * 2)
    {
        const int n = opus_encode_float(o.enc, o.accum.data(), kOpusFrame,
                                        packet, (opus_int32) sizeof(packet));
        o.accum.erase(o.accum.begin(), o.accum.begin() + kOpusFrame * 2);
        if (n > 0 && anyListener)
            out.push_back(buildWsFrame(0x02, packet, (size_t) n));
    }
}

void StreamServer::serviceClient(Client& c)
{
    // Drain whatever the socket has and parse it: close frames from anyone,
    // doc messages from doc clients. Audio clients otherwise never send.
    // Bounded per tick: this runs on the audio broadcast thread, so one chatty
    // doc socket must not delay the audio clients behind it in the list.
    size_t drained = 0;
    while (drained < kMaxDrainPerTick && c.sock->waitUntilReady(true, 0) == 1)
    {
        uint8_t buf[4096];
        const int n = c.sock->read(buf, (int) sizeof(buf), false);
        if (n <= 0)
        {
            c.dead = true;
            return;
        }
        c.inbuf.insert(c.inbuf.end(), buf, buf + n);
        drained += (size_t) n;
        if (c.inbuf.size() > kMaxInboundBuffer)
        {
            c.dead = true;
            return;
        }
        if (n < (int) sizeof(buf))
            break;
    }

    if (! c.inbuf.empty() && ! parseInbound(c))
    {
        c.dead = true;
        return;
    }

    // Flush as much as the socket will take without blocking.
    while (c.outPos < c.outbuf.size())
    {
        if (c.sock->waitUntilReady(false, 0) != 1)
            break;
        const int chunk = (int) juce::jmin((size_t) 65536, c.outbuf.size() - c.outPos);
        const int written = c.sock->write(c.outbuf.data() + c.outPos, chunk);
        if (written < 0)
        {
            c.dead = true;
            return;
        }
        c.outPos += (size_t) written;
        if (written < chunk)
            break;
    }

    if (c.outPos > 0 && c.outPos == c.outbuf.size())
    {
        c.outbuf.clear();
        c.outPos = 0;
    }
    else if (c.outPos > (size_t) (1 << 20))
    {
        c.outbuf.erase(c.outbuf.begin(), c.outbuf.begin() + (long) c.outPos);
        c.outPos = 0;
    }
}

// ---------------------------------------------------------------------------
// inbound frames (RFC 6455): client frames are always masked

bool StreamServer::parseInbound(Client& c)
{
    size_t pos = 0;
    auto& in = c.inbuf;

    while (true)
    {
        const size_t avail = in.size() - pos;
        if (avail < 2)
            break;

        const uint8_t b0 = in[pos], b1 = in[pos + 1];
        const bool fin = (b0 & 0x80) != 0;
        const uint8_t opcode = b0 & 0x0f;
        const bool masked = (b1 & 0x80) != 0;
        uint64_t len = b1 & 0x7f;
        size_t hdr = 2;

        if (len == 126)
        {
            if (avail < 4) break;
            len = ((uint64_t) in[pos + 2] << 8) | in[pos + 3];
            hdr = 4;
        }
        else if (len == 127)
        {
            if (avail < 10) break;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | in[pos + 2 + (size_t) i];
            hdr = 10;
        }

        if (! masked || len > kMaxInboundMessage)
            return false;
        if (avail < hdr + 4 + len)
            break;   // wait for the rest of the frame

        const uint8_t mask[4] = { in[pos + hdr], in[pos + hdr + 1], in[pos + hdr + 2], in[pos + hdr + 3] };
        uint8_t* data = &c.inbuf[pos + hdr + 4];
        for (size_t i = 0; i < (size_t) len; ++i)
            data[i] ^= mask[i & 3];
        std::vector<uint8_t> payload(data, data + (size_t) len);
        pos += hdr + 4 + (size_t) len;

        switch (opcode)
        {
            case 0x08:   // close
                return false;

            case 0x09:   // ping -> pong with the same payload
            {
                queueFrame(c, buildWsFrame(0x0A, payload.data(), payload.size()));
                break;
            }

            case 0x0A:   // pong
                break;

            case 0x00:   // continuation
                if (c.fragOpcode == 0)
                    return false;
                c.fragBuf.insert(c.fragBuf.end(), payload.begin(), payload.end());
                if (c.fragBuf.size() > kMaxInboundMessage)
                    return false;
                if (fin)
                {
                    handleClientMessage(c, c.fragOpcode, c.fragBuf);
                    c.fragBuf.clear();
                    c.fragOpcode = 0;
                }
                break;

            case 0x01:   // text
            case 0x02:   // binary
                if (! fin)
                {
                    c.fragOpcode = opcode;
                    c.fragBuf = std::move(payload);
                }
                else
                {
                    handleClientMessage(c, opcode, payload);
                }
                break;

            default:
                return false;
        }
    }

    c.inbuf.erase(c.inbuf.begin(), c.inbuf.begin() + (long) pos);
    return true;
}

void StreamServer::handleClientMessage(Client& c, uint8_t opcode, const std::vector<uint8_t>& payload)
{
    if (! c.docOnly || opcode != 0x01 || payload.empty())
        return;   // audio sockets have nothing to say; binary is not part of the protocol
    handleDocMessage(c, juce::String::fromUTF8((const char*) payload.data(), (int) payload.size()));
}

// ---------------------------------------------------------------------------
// shared doc relay (broadcast thread only)

// Strict enough that the client's atob() can never throw on a replayed entry:
// non-empty, length a multiple of 4, '=' padding only at the very end.
static bool isStrictBase64(const juce::String& s)
{
    const auto n = s.getNumBytesAsUTF8();
    if (n == 0 || (n % 4) != 0)
        return false;
    const char* p = s.toRawUTF8();
    size_t pad = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const char ch = p[i];
        if (ch == '=')
        {
            if (++pad > 2 || i < n - 2) return false;
        }
        else if (pad != 0 || ! ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                                 || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/'))
        {
            return false;
        }
    }
    return true;
}

void StreamServer::queueFrame(Client& c, const std::vector<uint8_t>& frame)
{
    // A doc socket that stops draining (backgrounded phone tab, dead TCP path)
    // is dropped rather than buffered without bound; it replays on reconnect.
    if (c.docOnly && c.outbuf.size() - c.outPos + frame.size() > kMaxOutBacklog)
    {
        c.dead = true;
        return;
    }
    c.outbuf.insert(c.outbuf.end(), frame.begin(), frame.end());
}

void StreamServer::queueText(Client& c, const juce::String& json)
{
    queueFrame(c, buildWsFrame(0x01, json.toRawUTF8(), json.getNumBytesAsUTF8()));
}

void StreamServer::sendDocLog(Client& c)
{
    c.docSynced = true;
    if (docReplayFrame.empty())
    {
        juce::String json;
        json.preallocateBytes(docLogBytes + docLog.size() * 3 + 16);
        json << "{\"doc\":[";
        for (size_t i = 0; i < docLog.size(); ++i)
            json << (i == 0 ? "\"" : ",\"") << docLog[i] << "\"";
        json << "]}";
        docReplayFrame = buildWsFrame(0x01, json.toRawUTF8(), json.getNumBytesAsUTF8());
    }
    queueFrame(c, docReplayFrame);
}

void StreamServer::handleDocMessage(Client& from, const juce::String& text)
{
    const auto v = juce::JSON::parse(text);
    auto* obj = v.getDynamicObject();
    if (obj == nullptr)
        return;

    const auto now = juce::Time::getMillisecondCounter();
    if (docSnapRequestedAt != 0 && now - docSnapRequestedAt > kDocSnapTimeoutMs)
        docSnapRequestedAt = 0;   // asked client went away; allow another request

    if (obj->hasProperty("d"))
    {
        const auto& dv = v["d"];
        if (! dv.isString())
            return;
        const auto u = dv.toString();
        if (u.getNumBytesAsUTF8() > kMaxDocEntry || ! isStrictBase64(u))
            return;

        docLog.push_back(u);
        docLogBytes += (size_t) u.length();
        docReplayFrame.clear();

        // The base64 alphabet needs no JSON escaping, so build the relay by hand.
        const auto relayJson = "{\"d\":\"" + u + "\"}";
        const auto relay = buildWsFrame(0x01, relayJson.toRawUTF8(), relayJson.getNumBytesAsUTF8());
        for (auto& c : clients)
            if (c->docOnly && c->docSynced && c.get() != &from && ! c->dead)
                queueFrame(*c, relay);

        if (docCompactAt == 0)
            docCompactAt = kDocCompactBytes;
        if (docLogBytes > docCompactAt && docSnapRequestedAt == 0)
        {
            docSnapRequestedAt = now;
            docSnapClientId = from.id;
            docSnapMark = docLog.size();
            queueText(from, "{\"snapreq\":1}");
        }
        return;
    }

    if (obj->hasProperty("snap"))
    {
        // Only the client we asked, and only for the outstanding request: a
        // late snapshot from an earlier request would be missing everything
        // logged since its mark.
        if (docSnapRequestedAt == 0 || from.id != docSnapClientId)
            return;
        const auto& sv = v["snap"];
        if (! sv.isString())
            return;
        const auto snap = sv.toString();
        if (snap.getNumBytesAsUTF8() > kMaxDocEntry || ! isStrictBase64(snap))
            return;

        std::vector<juce::String> fresh;
        const auto mark = juce::jmin(docSnapMark, docLog.size());
        fresh.reserve(1 + docLog.size() - mark);
        fresh.push_back(snap);
        fresh.insert(fresh.end(), docLog.begin() + (long) mark, docLog.end());
        docLog.swap(fresh);
        docLogBytes = 0;
        for (const auto& e : docLog)
            docLogBytes += (size_t) e.length();
        docReplayFrame.clear();
        docSnapRequestedAt = 0;
        // Next compaction once the log has doubled again, so a large document
        // does not trigger a full snapshot round-trip on every keystroke.
        docCompactAt = juce::jmax(kDocCompactBytes, docLogBytes * 2);
    }
}

// ---------------------------------------------------------------------------
// helpers

std::vector<uint8_t> StreamServer::buildWsFrame(uint8_t opcode, const void* data, size_t len)
{
    std::vector<uint8_t> f;
    f.reserve(len + 10);
    f.push_back((uint8_t) (0x80 | opcode));   // FIN + opcode

    if (len < 126)
    {
        f.push_back((uint8_t) len);
    }
    else if (len < 65536)
    {
        f.push_back(126);
        f.push_back((uint8_t) (len >> 8));
        f.push_back((uint8_t) (len & 0xff));
    }
    else
    {
        f.push_back(127);
        for (int i = 7; i >= 0; --i)
            f.push_back((uint8_t) ((len >> (i * 8)) & 0xff));
    }

    const auto* p = static_cast<const uint8_t*>(data);
    f.insert(f.end(), p, p + len);
    return f;
}

juce::String StreamServer::makeAcceptKey(const juce::String& clientKey)
{
    const auto combined = clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(combined.toRawUTF8(), (CC_LONG) combined.getNumBytesAsUTF8(), digest);

    juce::MemoryOutputStream out;
    juce::Base64::convertToBase64(out, digest, sizeof(digest));
    return out.toString();
}

std::vector<uint8_t> StreamServer::buildHelloFrame(double rate, int mode) const
{
    const auto id = getStreamId();
    const auto idField = id.isEmpty() ? juce::String()
                                      : ",\"streamId\":\"" + id + "\"";
    juce::String json;
    if (mode == 0)
        json = "{\"codec\":\"pcm\",\"sampleRate\":" + juce::String((int) rate)
               + ",\"channels\":2" + idField + "}";
    else
        json = "{\"codec\":\"opus\",\"sampleRate\":48000,\"channels\":2,\"bitrate\":"
               + juce::String(mode == 1 ? 256000 : 128000) + idField + "}";
    return buildWsFrame(0x01, json.toRawUTF8(), json.getNumBytesAsUTF8());
}
