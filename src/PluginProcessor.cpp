#include "PluginProcessor.h"
#include "PluginEditor.h"

ListenLinkProcessor::ListenLinkProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    addParameter(qualityParam = new juce::AudioParameterChoice(
        juce::ParameterID("quality", 1), "Stream Quality",
        juce::StringArray { "Lossless PCM", "Opus 256 kbps", "Opus 128 kbps" }, 0));

    interleaved.resize(8192 * 2);
    server.startServer();
    server.setStreamMode(qualityParam->getIndex());
}

ListenLinkProcessor::~ListenLinkProcessor()
{
    tunnel.stopTunnel();
    server.stopServer();
}

bool ListenLinkProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out)
        return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void ListenLinkProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    server.setSampleRate(sampleRate);
    if ((int) interleaved.size() < samplesPerBlock * 2)
        interleaved.resize((size_t) samplesPerBlock * 2);
}

void ListenLinkProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    server.setStreamMode(qualityParam->getIndex());

    const int n = buffer.getNumSamples();
    if (n <= 0 || buffer.getNumChannels() == 0 || (int) interleaved.size() < n * 2)
        return;   // audio passes through untouched either way

    const float* left  = buffer.getReadPointer(0);
    const float* right = buffer.getNumChannels() > 1 ? buffer.getReadPointer(1) : left;

    float pl = 0.0f, pr = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float l = left[i], r = right[i];
        interleaved[(size_t) (i * 2)]     = l;
        interleaved[(size_t) (i * 2 + 1)] = r;
        pl = juce::jmax(pl, std::abs(l));
        pr = juce::jmax(pr, std::abs(r));
    }

    auto storePeak = [](std::atomic<float>& peak, float v)
    {
        float cur = peak.load();
        while (v > cur && ! peak.compare_exchange_weak(cur, v)) {}
    };
    storePeak(peakLeft, pl);
    storePeak(peakRight, pr);

    server.pushAudio(interleaved.data(), n);
}

juce::AudioProcessorEditor* ListenLinkProcessor::createEditor()
{
    return new ListenLinkEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ListenLinkProcessor();
}
