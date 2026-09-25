#include "PluginEditor.h"

static const char* const kQualityNames[] = { "Lossless PCM", "Opus 256 kbps", "Opus 128 kbps" };
static const char* const kQualityHints[] = { "~1.5 Mbit/s", "~0.26 Mbit/s", "~0.13 Mbit/s" };

// ---------------------------------------------------------------------------

void StyledButton::paintButton(juce::Graphics& g, bool over, bool down)
{
    const auto r = getLocalBounds().toFloat();
    juce::Colour textColour = ll::text;
    if (! isEnabled())
        g.setOpacity(0.45f);

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
    // Only the stock PopupMenu / AlertWindow / TextEditor use this; every
    // other control paints itself.
    lnf.setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff26262f));
    lnf.setColour(juce::PopupMenu::textColourId, ll::menuIdle);
    lnf.setColour(juce::PopupMenu::highlightedBackgroundColourId, ll::border3);
    lnf.setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    lnf.setColour(juce::PopupMenu::headerTextColourId, ll::dim);
    lnf.setColour(juce::AlertWindow::backgroundColourId, ll::card);
    lnf.setColour(juce::AlertWindow::textColourId, ll::text);
    lnf.setColour(juce::AlertWindow::outlineColourId, ll::border3);
    lnf.setColour(juce::TextEditor::backgroundColourId, ll::bg);
    lnf.setColour(juce::TextEditor::textColourId, ll::text);
    lnf.setColour(juce::TextEditor::outlineColourId, ll::border3);
    lnf.setColour(juce::TextEditor::focusedOutlineColourId, ll::accent);
    lnf.setColour(juce::TextButton::buttonColourId, ll::buttonBg);
    lnf.setColour(juce::TextButton::textColourOffId, ll::text);
    setLookAndFeel(&lnf);

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
        processor.server.setSharing(true);
        processor.tunnel.activate(processor.server.getPort());
        maybeAutoCreateNotes();
        updateState();
    };
    addAndMakeVisible(createButton);

    notesConnectButton.onClick = [this]
    {
        GoogleDocs::get().beginSignIn(processor.server.getPort());
        updateState();
    };
    addChildComponent(notesConnectButton);

    notesOpenButton.onClick = [this]
    {
        const auto d = processor.getNotesDoc();
        if (d.isValid())
            GoogleDocs::openExternal(d.url);
    };
    addChildComponent(notesOpenButton);

    notesMenuButton.onClick = [this] { showNotesMenu(); };
    addChildComponent(notesMenuButton);
    GoogleDocs::get().refreshRecentAsync();

    stopButton.onClick = [this]
    {
        // Order matters: close the tap (kicks listeners, refuses new ones)
        // before unregistering, so nobody can slip in during the teardown.
        processor.server.setSharing(false);
        processor.tunnel.deactivate();
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

    updateButton.onClick = [] { openDownloadPage(); };
    addChildComponent(updateButton);
    UpdateChecker::checkAsync();

    setSize(560, 388);
    startTimerHz(30);
    updateState();

    // Pre-warm: opening the editor starts the tunnel (and downloads
    // cloudflared if needed) so Create Public Link is effectively instant
    // and the tunnel hostname has usually propagated through DNS already.
    processor.tunnel.warmUp(processor.server.getPort());
}

int ListenLinkEditor::tunnelState() const
{
    if (! processor.tunnel.isSharing())
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

    // --- Google Doc card: buttons right-aligned, text gets the rest --------
    {
        auto& gd = GoogleDocs::get();
        const bool configured = GoogleDocs::isConfigured();
        const bool signedIn = gd.isSignedIn();
        const auto doc = processor.getNotesDoc();
        int right = 526;
        auto place = [&right](StyledButton& b, const juce::String& text)
        {
            b.setButtonText(text);
            const int w = (int) ll::textWidth(ll::sans(12.0f, true), text) + 26;
            b.setBounds(right - w, 309, w, 26);
            right -= w + 8;
        };

        notesMenuButton.setVisible(configured && signedIn);
        if (notesMenuButton.isVisible())
        {
            place(notesMenuButton, juce::String(doc.isValid() ? "Change" : "Attach doc")
                                       + juce::String::fromUTF8(" \xe2\x96\xbe"));
            notesMenuButton.setEnabled(! gd.isBusy());
        }
        notesOpenButton.setVisible(configured && doc.isValid());
        if (notesOpenButton.isVisible())
            place(notesOpenButton, "Open");
        notesConnectButton.setVisible(configured && ! signedIn);
        if (notesConnectButton.isVisible())
            place(notesConnectButton, gd.isSignInPending() ? "Waiting for Google..." : "Connect Google Docs");
        notesTextRight = right;
    }

    repaint();
}

// ---------------------------------------------------------------------------
// session notes

void ListenLinkEditor::showNotesMenu()
{
    auto& gd = GoogleDocs::get();
    const auto doc = processor.getNotesDoc();
    const auto recent = gd.getRecent();
    const juce::String mid = juce::String::fromUTF8("  \xc2\xb7  ");

    juce::PopupMenu m;
    m.setLookAndFeel(&lnf);   // menus don't inherit the editor's LookAndFeel
    m.addItem(1, "New doc for this session");
    m.addItem(2, "Paste a doc link...");
    if (! recent.empty())
    {
        m.addSeparator();
        m.addSectionHeader("Recent docs");
        for (size_t i = 0; i < recent.size(); ++i)
            m.addItem(100 + (int) i, recent[i].name + (recent[i].when.isNotEmpty() ? mid + recent[i].when : juce::String()),
                      true, recent[i].id == doc.id);
    }
    m.addSeparator();
    if (doc.isValid())
        m.addItem(3, "Detach doc");
    m.addItem(4, "Disconnect Google" + (gd.getEmail().isNotEmpty() ? " (" + gd.getEmail() + ")" : juce::String()));

    juce::Component::SafePointer<ListenLinkEditor> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&notesMenuButton).withMinimumWidth(240),
                    [safe, recent](int r)
    {
        if (safe == nullptr || r == 0)
            return;
        if (r == 1)
            safe->createNotesDoc();
        else if (r == 2)
            safe->promptForDocLink();
        else if (r == 3)
            safe->attachDoc({});
        else if (r == 4)
        {
            GoogleDocs::get().signOut();
            safe->updateState();
        }
        else if (r >= 100 && (size_t) (r - 100) < recent.size())
            safe->attachDoc(recent[(size_t) (r - 100)]);
    });
}

void ListenLinkEditor::attachDoc(const NotesDoc& d)
{
    processor.setNotesDoc(d);
    GoogleDocs::get().clearError();
    updateState();
}

void ListenLinkEditor::maybeAutoCreateNotes()
{
    auto& gd = GoogleDocs::get();
    if (GoogleDocs::isConfigured() && gd.isSignedIn() && ! gd.isBusy()
        && ! processor.getNotesDoc().isValid())
        createNotesDoc();
}

void ListenLinkEditor::createNotesDoc()
{
    const auto name = "Session doc " + juce::String::fromUTF8("\xe2\x80\x93 ")
                    + juce::Time::getCurrentTime().formatted("%Y-%m-%d");
    juce::WeakReference<ListenLinkProcessor> proc(&processor);
    GoogleDocs::get().runJob([proc, name]
    {
        auto& gd = GoogleDocs::get();
        NotesDoc doc;
        juce::String err;
        const bool ok = gd.createDoc(name, doc, err);
        if (err.isNotEmpty())
            gd.setError(err);
        if (ok)
            juce::MessageManager::callAsync([proc, doc]
            {
                if (auto* p = proc.get())
                    p->setNotesDoc(doc);
            });
    });
    updateState();
}

void ListenLinkEditor::promptForDocLink()
{
    auto* w = new juce::AlertWindow("Attach a Google Doc",
        "Paste the link to a Google Doc. In Google Docs, set its sharing to "
        "\"Anyone with the link\" as Editor so listeners can type in it.",
        juce::MessageBoxIconType::NoIcon, this);
    w->setLookAndFeel(&lnf);   // a desktop window: doesn't inherit ours
    w->addTextEditor("url", "", "Doc link");
    w->addButton("Attach", 1, juce::KeyPress(juce::KeyPress::returnKey));
    w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<ListenLinkEditor> safe(this);
    w->enterModalState(true, juce::ModalCallbackFunction::create([safe, w](int r)
    {
        if (r == 1 && safe != nullptr)
            safe->attachPastedDoc(w->getTextEditorContents("url"));
    }), true);
}

void ListenLinkEditor::attachPastedDoc(const juce::String& text)
{
    NotesDoc doc;
    if (! GoogleDocs::parseDocUrl(text, doc))
    {
        GoogleDocs::get().setError("That doesn't look like a Google Docs link.");
        updateState();
        return;
    }
    attachDoc(doc);

    // Best effort: pick up the doc's title from its public page so the card
    // shows a name instead of "Linked doc". Silent if the doc isn't public.
    juce::WeakReference<ListenLinkProcessor> proc(&processor);
    GoogleDocs::get().runJob([proc, doc]
    {
        const auto title = GoogleDocs::fetchPublicTitle(doc.url);
        if (title.isEmpty())
            return;
        juce::MessageManager::callAsync([proc, doc, title]
        {
            if (auto* p = proc.get())
            {
                auto cur = p->getNotesDoc();
                if (cur.id == doc.id)
                {
                    cur.name = title;
                    p->setNotesDoc(cur);
                }
            }
        });
    });
}

// launchInDefaultBrowser goes through NSWorkspace; if a host process ever
// refuses that, /usr/bin/open is a second route to the same page.
void ListenLinkEditor::openDownloadPage()
{
    if (! juce::URL(UpdateChecker::releasePageUrl).launchInDefaultBrowser())
    {
        juce::ChildProcess open;
        open.start(juce::StringArray { "/usr/bin/open", UpdateChecker::releasePageUrl });
    }
}

juce::Rectangle<int> ListenLinkEditor::subtitleRect() const
{
    // Stop 12 px short of whatever sits to the right in the header.
    const int right = updateButton.isVisible() ? updateButton.getX() - 12 : 540 - pillWidth() - 12;
    return { 58, 36, juce::jmax(0, right - 58), 16 };
}

juce::String ListenLinkEditor::subtitleHead() const
{
    const auto newer = UpdateChecker::getAvailableUpdate();
    const juce::String mid = juce::String::fromUTF8(" \xc2\xb7 ");
    return "v" JucePlugin_VersionString + (newer.isNotEmpty() ? mid + newer + " available" : juce::String());
}

juce::Rectangle<int> ListenLinkEditor::subtitleLinkRect() const
{
    const auto r = subtitleRect();
    const int w = (int) std::ceil(ll::textWidth(ll::sans(11.0f), subtitleHead()));
    return r.withWidth(juce::jmin(w, r.getWidth()));
}

void ListenLinkEditor::mouseDown(const juce::MouseEvent& e)
{
    if (updateAvailable() && subtitleLinkRect().contains(e.getPosition()))
        openDownloadPage();
}

void ListenLinkEditor::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(updateAvailable() && subtitleLinkRect().contains(e.getPosition())
                       ? juce::MouseCursor::PointingHandCursor
                       : juce::MouseCursor::NormalCursor);
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
        const auto newer = UpdateChecker::getAvailableUpdate();
        g.setFont(ll::sans(11.0f));
        // Version first: "which build is this?" is the first question in every
        // support exchange, and the DAW rarely shows it anywhere. With an
        // update pending the head goes accent and names it (and is clickable);
        // the serving info follows in dim and is dropped if it would run into
        // the Update button.
        const juce::String mid = juce::String::fromUTF8(" \xc2\xb7 ");
        const auto line = subtitleRect().withY(38).withHeight(12);
        const auto head = subtitleLinkRect().withY(38).withHeight(12);
        g.setColour(newer.isNotEmpty() ? ll::accent : ll::dim);
        g.drawText(subtitleHead(), head, juce::Justification::centredLeft);
        // Only whole: "Serving on p..." next to an Update button reads worse than
        // no port at all (the LIVE/OFF pill still says whether the server is up).
        const auto rest = line.withTrimmedLeft(head.getWidth());
        const juce::String tail = mid + (serving ? "Serving on port " + juce::String(processor.server.getPort())
                                                 : "Server failed to start (ports 17654-17663 busy?)");
        if (rest.getWidth() >= (int) std::ceil(ll::textWidth(ll::sans(11.0f), tail)))
        {
            g.setColour(ll::dim);
            g.drawText(tail, rest, juce::Justification::centredLeft);
        }

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
    g.fillRoundedRectangle(20.0f, 282.0f, 520.0f, 60.0f, 10.0f);    // notes

    const auto sectionFont = ll::sans(10.0f, true).withExtraKerningFactor(0.1f);
    g.setColour(ll::dim);
    g.setFont(sectionFont);
    g.drawText("STREAM QUALITY", 34, 89, 200, 12, juce::Justification::centredLeft);
    g.drawText("PUBLIC LINK", 34, 198, 200, 12, juce::Justification::centredLeft);
    g.drawText("GOOGLE DOC", 34, 293, 200, 12, juce::Justification::centredLeft);

    // --- Google Doc card ------------------------------------------------
    {
        auto& gd = GoogleDocs::get();
        const auto doc = processor.getNotesDoc();
        const auto err = gd.getLastError();
        const auto email = gd.getEmail();

        if (gd.isSignedIn() && email.isNotEmpty())
        {
            g.setColour(ll::faint);
            g.setFont(ll::sans(10.0f));
            const auto w = juce::jmin(260.0f, ll::textWidth(ll::sans(10.0f), email) + 2.0f);
            g.drawText(email, (int) (526.0f - w), 293, (int) w, 12, juce::Justification::centredRight);
        }

        juce::String line;
        juce::Colour col = ll::dim;
        if (! GoogleDocs::isConfigured())
            line = "Google Docs notes aren't available in this build.";
        else if (err.isNotEmpty())
        {
            line = err;
            col = ll::red;
        }
        else if (gd.isBusy())
            line = "Working with Google...";
        else if (gd.isSignInPending())
        {
            // Google's consent page has a separate checkbox for Drive access;
            // people miss it. Say so while they're looking at that page.
            line = "In the browser, tick the Google Drive box, then Continue.";
            col = ll::accent;
        }
        else if (doc.isValid())
        {
            line = doc.name;
            col = ll::text;
        }
        else if (gd.isSignedIn())
            line = "A new doc is created with your public link.";
        else
            line = "Attach a Google Doc that everyone on the link can edit.";

        g.setColour(col);
        g.setFont(ll::sans(12.0f));
        g.drawText(line, 34, 309, juce::jmax(0, notesTextRight - 10 - 34), 26,
                   juce::Justification::centredLeft);
    }

    // --- public link card states ---------------------------------------
    const int state = tunnelState();

    if (state == 2)
    {
        // The link shows instantly, so this dot is the only honest signal of
        // whether the tunnel behind it is actually up yet.
        const bool reg = processor.tunnel.isRegistered();
        const auto st = processor.tunnel.getStatus();
        const bool err = st.contains("Couldn't") || st.contains("Failed") || st.contains("exited");
        const juce::String cap = reg ? "tunnel up" : err ? "tunnel down" : "starting...";
        g.setColour(reg ? ll::green : err ? ll::red : ll::dim);
        g.setFont(ll::sans(10.0f));
        const auto capW = ll::textWidth(ll::sans(10.0f), cap);
        g.fillEllipse(526.0f - capW - 9.0f, 201.5f, 5.0f, 5.0f);
        g.drawText(cap, (int) (526.0f - capW), 198, (int) capW + 2, 12,
                   juce::Justification::centredLeft);

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
    g.drawText(fmt, 22, 356, 300, 12, juce::Justification::centredLeft);
    g.drawText(juce::String(kQualityHints[q]) + " per listener",
               238, 356, 300, 12, juce::Justification::centredRight);
}
