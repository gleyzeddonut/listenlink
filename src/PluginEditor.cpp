#include "PluginEditor.h"

static const char* const kQualityNames[] = { "Lossless PCM", "Opus 256 kbps", "Opus 128 kbps" };
static const char* const kQualityHints[] = { "~1.5 Mbit/s", "~0.26 Mbit/s", "~0.13 Mbit/s" };

// ---------------------------------------------------------------------------

void StyledButton::paintButton(juce::Graphics& g, bool over, bool down)
{
    const auto r = getLocalBounds().toFloat();
    juce::Colour textColour = ll::text;

    switch (style)
    {
        case Style::normal:
            g.setColour(over || down ? ll::buttonHov : ll::buttonBg);
            g.fillRoundedRectangle(r, 7.0f);
            break;

        case Style::accentBtn:
            g.setColour(over || down ? ll::accent.brighter(0.15f) : ll::accent);
            g.fillRoundedRectangle(r, 7.0f);
            textColour = juce::Colours::white;
            break;

        case Style::danger:
            if (over || down)
            {
                g.setColour(ll::red.withAlpha(0.1f));
                g.fillRoundedRectangle(r, 7.0f);
            }
            g.setColour(ll::red.withAlpha(0.35f));
            g.drawRoundedRectangle(r.reduced(0.5f), 7.0f, 1.0f);
            textColour = ll::red;
            break;
    }

    g.setColour(colourOverride.isTransparent() ? textColour : colourOverride);
    g.setFont(ll::sans(12.0f, true));
    g.drawText(getButtonText(), getLocalBounds(), juce::Justification::centred);
}

void QualityButton::paintButton(juce::Graphics& g, bool over, bool down)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour(over || down ? ll::buttonHov : ll::buttonBg);
    g.fillRoundedRectangle(r, 7.0f);

    g.setColour(ll::text);
    g.setFont(ll::sans(12.0f, true));
    g.drawText(getButtonText(), getLocalBounds().withTrimmedLeft(10).withTrimmedRight(24),
               juce::Justification::centredLeft);

    g.setColour(ll::dim);
    g.setFont(ll::sans(9.0f));
    g.drawText(juce::String::fromUTF8("\xe2\x96\xbe"),
               getLocalBounds().withTrimmedRight(9), juce::Justification::centredRight);
}

// ---------------------------------------------------------------------------

void MeterPanel::paint(juce::Graphics& g)
{
    constexpr int labelW = 14, gap = 8, barH = 8;
    const float barX = (float) (labelW + gap);
    const float barW = (float) getWidth() - barX;

    g.setFont(ll::mono(10.0f));
    g.setColour(ll::dim);
    g.drawText("L", 0, -1, labelW, barH + 2, juce::Justification::centredLeft);
    g.drawText("R", 0, barH + 6 - 1, labelW, barH + 2, juce::Justification::centredLeft);

    juce::ColourGradient grad(ll::green, barX, 0.0f, ll::red, barX + barW, 0.0f, false);
    grad.addColour(0.55, ll::green);
    grad.addColour(0.80, ll::yellow);

    for (int ch = 0; ch < 2; ++ch)
    {
        const float y = (float) (ch * (barH + 6));
        const juce::Rectangle<float> bar(barX, y, barW, (float) barH);

        g.setColour(ll::track);
        g.fillRoundedRectangle(bar, 4.0f);

        const float lev = ch == 0 ? levL : levR;
        if (lev > 0.004f)
        {
            g.saveState();
            g.reduceClipRegion(bar.withWidth(bar.getWidth() * lev).getSmallestIntegerContainer());
            g.setGradientFill(grad);
            g.fillRoundedRectangle(bar, 4.0f);
            g.restoreState();
        }

        const float pk = ch == 0 ? pkL : pkR;
        if (pk > 0.004f)
        {
            const float px = juce::jlimit(barX, barX + barW - 2.0f, barX + barW * pk - 1.0f);
            g.setColour(ll::text.withAlpha(0.75f));
            g.fillRoundedRectangle(px, y - 1.0f, 2.0f, (float) barH + 2.0f, 1.0f);
        }
    }

    // dB scale: -60 at 0%, then -24 / -12 / -6 / 0 at their percentage positions.
    const int scaleY = barH + 6 + barH + 6;
    g.setFont(ll::mono(9.0f));
    g.setColour(ll::faint);
    g.drawText("-60", (int) barX, scaleY, 30, 12, juce::Justification::centredLeft);
    struct Mark { const char* label; float pos; };
    for (const auto& m : { Mark{"-24", 0.6f}, Mark{"-12", 0.8f}, Mark{"-6", 0.9f} })
        g.drawText(m.label, (int) (barX + barW * m.pos) - 15, scaleY, 30, 12,
                   juce::Justification::centred);
    g.drawText("0 dB", (int) (barX + barW) - 34, scaleY, 34, 12, juce::Justification::centredRight);
}

// ---------------------------------------------------------------------------

void QualityPopup::show(juce::Rectangle<int> anchor, int selectedIndex)
{
    selected = selectedIndex;
    hover = -1;
    panel = { anchor.getRight() - 200, anchor.getBottom() + 4, 200, 4 + 3 * 29 + 4 };
    setVisible(true);
    toFront(false);
    repaint();
}

juce::Rectangle<int> QualityPopup::rowRect(int i) const
{
    return { panel.getX() + 4, panel.getY() + 4 + i * 29, panel.getWidth() - 8, 29 };
}

void QualityPopup::paint(juce::Graphics& g)
{
    juce::DropShadow(juce::Colour(0x80000000), 24, { 0, 8 })
        .drawForRectangle(g, panel);

    g.setColour(juce::Colour(0xff26262f));
    g.fillRoundedRectangle(panel.toFloat(), 8.0f);
    g.setColour(ll::border3);
    g.drawRoundedRectangle(panel.toFloat().reduced(0.5f), 8.0f, 1.0f);

    for (int i = 0; i < 3; ++i)
    {
        const auto row = rowRect(i);
        if (i == hover)
        {
            g.setColour(ll::border3);
            g.fillRoundedRectangle(row.toFloat(), 5.0f);
        }
        g.setColour(i == selected ? juce::Colours::white : ll::menuIdle);
        g.setFont(ll::sans(12.0f));
        g.drawText(kQualityNames[i], row.withTrimmedLeft(10), juce::Justification::centredLeft);
        g.setColour(ll::faint);
        g.setFont(ll::mono(10.0f));
        g.drawText(kQualityHints[i], row.withTrimmedRight(10), juce::Justification::centredRight);
    }
}

void QualityPopup::mouseMove(const juce::MouseEvent& e)
{
    int newHover = -1;
    for (int i = 0; i < 3; ++i)
        if (rowRect(i).contains(e.getPosition()))
            newHover = i;
    if (newHover != hover)
    {
        hover = newHover;
        repaint();
    }
}

void QualityPopup::mouseDown(const juce::MouseEvent& e)
{
    for (int i = 0; i < 3; ++i)
    {
        if (rowRect(i).contains(e.getPosition()))
        {
            setVisible(false);
            if (onPick)
                onPick(i);
            return;
        }
    }
    setVisible(false);
}

// ---------------------------------------------------------------------------

ListenLinkEditor::ListenLinkEditor(ListenLinkProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    addAndMakeVisible(meter);

    qualityButton.onClick = [this]
    {
        if (popup.isVisible())
            popup.setVisible(false);
        else
            popup.show(qualityButton.getBounds(), processor.qualityParam->getIndex());
    };
    addAndMakeVisible(qualityButton);

    publicUrlLabel.setFont(ll::mono(13.0f));
    publicUrlLabel.setColour(juce::Label::textColourId, ll::green);
    publicUrlLabel.setJustificationType(juce::Justification::centredLeft);
    publicUrlLabel.setBorderSize({ 0, 0, 0, 0 });
    publicUrlLabel.setMinimumHorizontalScale(1.0f);
    publicUrlLabel.setInterceptsMouseClicks(false, false);
    addChildComponent(publicUrlLabel);

    copyPublicButton.onClick = [this]
    {
        juce::SystemClipboard::copyTextToClipboard(processor.tunnel.getPublicUrl());
        copiedPublic = 45;
        copyPublicButton.setButtonText("Copied");
        copyPublicButton.setTextColourOverride(ll::green);
    };
    addChildComponent(copyPublicButton);

    createButton.onClick = [this]
    {
        processor.tunnel.startTunnel(processor.server.getPort());
        updateState();
    };
    addAndMakeVisible(createButton);

    stopButton.onClick = [this]
    {
        processor.tunnel.stopTunnel();
        updateState();
    };
    addChildComponent(stopButton);

    popup.onPick = [this](int i)
    {
        if (i != processor.qualityParam->getIndex())
            *processor.qualityParam = i;
        updateState();
    };
    addChildComponent(popup);

    updateButton.onClick = []
    { juce::URL(UpdateChecker::releasePageUrl).launchInDefaultBrowser(); };
    addChildComponent(updateButton);
    UpdateChecker::checkAsync();

    setSize(560, 314);
    startTimerHz(30);
    updateState();

    TunnelManager::prefetchAsync();
}

int ListenLinkEditor::tunnelState() const
{
    if (! processor.tunnel.isTunnelActive())
        return 0;
    return processor.tunnel.getPublicUrl().isEmpty() ? 1 : 2;
}

int ListenLinkEditor::pillWidth() const
{
    const bool serving = processor.server.isServerRunning();
    const int n = processor.server.getNumListeners();
    const juce::String word = serving ? "LIVE" : "OFF";
    const juce::String count = juce::String::fromUTF8("\xc2\xb7 ") + juce::String(n)
                               + (n == 1 ? " listener" : " listeners");
    return (int) (12 + 7 + 7 + ll::textWidth(ll::sans(12.0f, true), word)
                  + 6 + ll::textWidth(ll::sans(12.0f), count) + 12);
}

void ListenLinkEditor::timerCallback()
{
    ++tick;

    auto toNorm = [](float v)
    {
        const float db = juce::Decibels::gainToDecibels(v, -60.0f);
        return juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
    };

    const float nL = toNorm(processor.peakLeft.exchange(0.0f));
    const float nR = toNorm(processor.peakRight.exchange(0.0f));
    dispL = juce::jmax(nL, dispL - 0.045f);
    dispR = juce::jmax(nR, dispR - 0.045f);
    holdL = juce::jmax(nL, holdL - 0.004f);
    holdR = juce::jmax(nR, holdR - 0.004f);
    meter.setValues(dispL, dispR, holdL, holdR);

    repaint(330, 16, 210, 34);                 // LIVE pill pulse
    if (tunnelState() == 1)
        repaint(20, 196, 520, 60);             // spinner

    if (copiedPublic > 0 && --copiedPublic == 0)
    {
        copyPublicButton.setButtonText("Copy");
        copyPublicButton.clearTextColourOverride();
    }

    if (tick % 15 == 1)
        updateState();
}

void ListenLinkEditor::updateState()
{
    publicUrlLabel.setText(processor.tunnel.getPublicUrl(), juce::dontSendNotification);

    qualityButton.setButtonText(kQualityNames[processor.qualityParam->getIndex()]);
    const int qw = (int) ll::textWidth(ll::sans(12.0f, true), qualityButton.getButtonText()) + 37;
    qualityButton.setBounds(526 - qw, 82, qw, 26);

    const int state = tunnelState();
    createButton.setVisible(state == 0);
    publicUrlLabel.setVisible(state == 2);
    copyPublicButton.setVisible(state == 2);
    stopButton.setVisible(state == 2);

    const auto updateTag = UpdateChecker::getAvailableUpdate();
    if (updateTag.isNotEmpty())
    {
        updateButton.setButtonText("Update to " + updateTag);
        const int w = (int) ll::textWidth(ll::sans(12.0f, true), updateButton.getButtonText()) + 24;
        updateButton.setBounds(540 - pillWidth() - 10 - w, 25, w, 24);
        updateButton.setVisible(true);
    }

    repaint();
}

void ListenLinkEditor::resized()
{
    meter.setBounds(34, 118, 492, 44);

    publicUrlLabel.setBounds(44, 220, 320, 34);
    copyPublicButton.setBounds(384, 220, 74, 34);
    stopButton.setBounds(468, 220, 58, 34);

    const int cw = (int) ll::textWidth(ll::sans(12.0f, true), createButton.getButtonText()) + 32;
    createButton.setBounds(526 - cw, 221, cw, 32);

    popup.setBounds(getLocalBounds());
}

void ListenLinkEditor::paint(juce::Graphics& g)
{
    g.fillAll(ll::bg);
    g.setColour(ll::border1);
    g.drawRect(getLocalBounds(), 1);

    // --- header ---------------------------------------------------------
    {
        const juce::Rectangle<float> logo(20.0f, 23.0f, 28.0f, 28.0f);
        juce::ColourGradient lg(ll::accent, logo.getX(), logo.getY(),
                                ll::accent2, logo.getRight(), logo.getBottom(), false);
        g.setGradientFill(lg);
        g.fillRoundedRectangle(logo, 8.0f);
        g.setColour(juce::Colours::white);
        g.drawEllipse(logo.getCentreX() - 5.0f, logo.getCentreY() - 5.0f, 10.0f, 10.0f, 2.0f);

        g.setColour(ll::text);
        g.setFont(ll::sans(16.0f, true));
        g.drawText("ListenLink", 58, 20, 300, 17, juce::Justification::centredLeft);

        const bool serving = processor.server.isServerRunning();
        g.setColour(ll::dim);
        g.setFont(ll::sans(11.0f));
        g.drawText(serving ? "Serving on port " + juce::String(processor.server.getPort())
                           : "Server failed to start (ports 17654-17663 busy?)",
                   58, 38, 340, 12, juce::Justification::centredLeft);

        // LIVE pill
        const int n = processor.server.getNumListeners();
        const auto pillColour = serving ? ll::green : ll::red;
        const juce::String word = serving ? "LIVE" : "OFF";
        const juce::String count = juce::String::fromUTF8("\xc2\xb7 ") + juce::String(n)
                                   + (n == 1 ? " listener" : " listeners");
        const float wWord = ll::textWidth(ll::sans(12.0f, true), word);
        const float wCount = ll::textWidth(ll::sans(12.0f), count);
        const float pillW = 12 + 7 + 7 + wWord + 6 + wCount + 12;
        const juce::Rectangle<float> pill(540.0f - pillW, 25.0f, pillW, 24.0f);

        g.setColour(pillColour.withAlpha(0.1f));
        g.fillRoundedRectangle(pill, 12.0f);
        g.setColour(pillColour.withAlpha(0.25f));
        g.drawRoundedRectangle(pill.reduced(0.5f), 12.0f, 1.0f);

        const float pulse = 0.65f + 0.35f * std::cos(juce::MathConstants<float>::twoPi
                                                     * (float) tick / 30.0f / 1.2f);
        g.setColour(pillColour.withAlpha(serving ? pulse : 1.0f));
        g.fillEllipse(pill.getX() + 12.0f, pill.getCentreY() - 3.5f, 7.0f, 7.0f);

        float tx = pill.getX() + 12 + 7 + 7;
        g.setColour(pillColour);
        g.setFont(ll::sans(12.0f, true));
        g.drawText(word, (int) tx, (int) pill.getY(), (int) wWord + 2, 24,
                   juce::Justification::centredLeft);
        g.setColour(ll::dim);
        g.setFont(ll::sans(12.0f));
        g.drawText(count, (int) (tx + wWord + 6), (int) pill.getY(), (int) wCount + 4, 24,
                   juce::Justification::centredLeft);
    }

    // --- cards ----------------------------------------------------------
    g.setColour(ll::card);
    g.fillRoundedRectangle(20.0f, 68.0f, 520.0f, 102.0f, 10.0f);    // meters
    g.fillRoundedRectangle(20.0f, 184.0f, 520.0f, 84.0f, 10.0f);    // public

    const auto sectionFont = ll::sans(10.0f, true).withExtraKerningFactor(0.1f);
    g.setColour(ll::dim);
    g.setFont(sectionFont);
    g.drawText("STREAM QUALITY", 34, 89, 200, 12, juce::Justification::centredLeft);
    g.drawText("PUBLIC LINK", 34, 198, 200, 12, juce::Justification::centredLeft);

    // --- public link card states ---------------------------------------
    const int state = tunnelState();

    if (state == 2)
    {
        g.setColour(ll::green);
        g.setFont(ll::sans(10.0f));
        g.fillEllipse(455.0f, 201.5f, 5.0f, 5.0f);
        g.drawText("tunnel up", 464, 198, 62, 12, juce::Justification::centredLeft);

        g.setColour(ll::bg);
        g.fillRoundedRectangle(34.0f, 220.0f, 340.0f, 34.0f, 7.0f);
        g.setColour(ll::green.withAlpha(0.3f));
        g.drawRoundedRectangle(34.5f, 220.5f, 339.0f, 33.0f, 7.0f, 1.0f);
    }
    else if (state == 1)
    {
        const juce::Rectangle<float> spin(34.0f, 230.0f, 14.0f, 14.0f);
        g.setColour(ll::buttonBg);
        g.drawEllipse(spin, 2.0f);
        juce::Path head;
        const float angle = juce::MathConstants<float>::twoPi * (float) tick / 24.0f;
        head.addCentredArc(spin.getCentreX(), spin.getCentreY(), 6.0f, 6.0f,
                           angle, 0.0f, juce::MathConstants<float>::halfPi, true);
        g.setColour(ll::accent);
        g.strokePath(head, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));

        g.setColour(ll::dim);
        g.setFont(ll::sans(12.0f));
        const auto status = processor.tunnel.getStatus();
        g.drawText(status.isNotEmpty() ? status : juce::String("Starting cloudflared tunnel..."),
                   56, 220, 400, 34, juce::Justification::centredLeft);
    }
    else
    {
        const auto status = processor.tunnel.getStatus();
        g.setColour(status.isNotEmpty() ? ll::red : ll::dim);
        g.setFont(ll::sans(12.0f));
        g.drawText(status.isNotEmpty() ? status : juce::String("Generate a public link."),
                   34, 220, 350, 34, juce::Justification::centredLeft);
    }

    // --- footer ---------------------------------------------------------
    const int q = processor.qualityParam->getIndex();
    const double khz = processor.server.getSampleRate() / 1000.0;
    const juce::String rateStr = khz == std::floor(khz) ? juce::String((int) khz)
                                                        : juce::String(khz, 1);
    const juce::String mid = juce::String::fromUTF8(" \xc2\xb7 ");
    const juce::String fmt = q == 0 ? rateStr + " kHz" + mid + "16-bit PCM"
                                    : "Opus " + juce::String(q == 1 ? 256 : 128) + " kbps"
                                          + mid + "48 kHz";
    g.setColour(ll::faint);
    g.setFont(ll::mono(10.0f));
    g.drawText(fmt, 22, 282, 300, 12, juce::Justification::centredLeft);
    g.drawText(juce::String(kQualityHints[q]) + " per listener",
               238, 282, 300, 12, juce::Justification::centredRight);
}
