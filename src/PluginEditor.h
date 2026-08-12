#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

// Design tokens from the handoff (design 1b).
namespace ll
{
    const juce::Colour bg        (0xff17171d);
    const juce::Colour card      (0xff20202a);
    const juce::Colour track     (0xff26262f);
    const juce::Colour buttonBg  (0xff2c2c38);
    const juce::Colour buttonHov (0xff383846);
    const juce::Colour border1   (0xff24242e);
    const juce::Colour border2   (0xff2a2a33);
    const juce::Colour border3   (0xff33333e);
    const juce::Colour text      (0xffe8e8ec);
    const juce::Colour dim       (0xff8a8a96);
    const juce::Colour faint     (0xff5c5c68);
    const juce::Colour menuIdle  (0xffb8b8c2);
    const juce::Colour accent    (0xff4f6bff);
    const juce::Colour accent2   (0xff7a4fff);
    const juce::Colour green     (0xff3ddc84);
    const juce::Colour yellow    (0xffffd24a);
    const juce::Colour red       (0xffff5d5d);

    inline juce::Font sans(float size, bool bold = false)
    {
        return juce::Font(juce::FontOptions(juce::Font::getDefaultSansSerifFontName(),
                                            size, bold ? juce::Font::bold : juce::Font::plain));
    }

    inline juce::Font mono(float size)
    {
        return juce::Font(juce::FontOptions("Menlo", size, juce::Font::plain));
    }

    inline float textWidth(const juce::Font& f, const juce::String& s)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText(f, s, 0.0f, 0.0f);
        return ga.getBoundingBox(0, -1, true).getWidth();
    }
}

class StyledButton : public juce::Button
{
public:
    enum class Style { normal, accentBtn, danger };

    StyledButton(const juce::String& label, Style s) : juce::Button(label), style(s)
    {
        setButtonText(label);
    }

    void setTextColourOverride(juce::Colour c) { colourOverride = c; repaint(); }
    void clearTextColourOverride()             { colourOverride = {}; repaint(); }

    void paintButton(juce::Graphics& g, bool over, bool down) override;

private:
    Style style;
    juce::Colour colourOverride;
};

// The "STREAM QUALITY" dropdown trigger: label + small ▾.
class QualityButton : public juce::Button
{
public:
    QualityButton() : juce::Button("quality") {}
    void paintButton(juce::Graphics& g, bool over, bool down) override;
};

// L/R meter bars with peak-hold lines and the dB scale beneath.
class MeterPanel : public juce::Component
{
public:
    void setValues(float l, float r, float peakL, float peakR)
    {
        levL = l; levR = r; pkL = peakL; pkR = peakR;
        repaint();
    }

    void paint(juce::Graphics&) override;

private:
    float levL = 0, levR = 0, pkL = 0, pkR = 0;
};

// Full-editor overlay: draws the quality menu panel, dismisses on outside click.
class QualityPopup : public juce::Component
{
public:
    std::function<void(int)> onPick;

    void show(juce::Rectangle<int> anchor, int selectedIndex);
    void paint(juce::Graphics&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override { hover = -1; repaint(); }

private:
    juce::Rectangle<int> rowRect(int i) const;
    juce::Rectangle<int> panel;
    int selected = 0, hover = -1;
};

class ListenLinkEditor : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit ListenLinkEditor(ListenLinkProcessor&);
    ~ListenLinkEditor() override = default;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateState();               // 2 Hz: text, visibility, layout of variable-width bits
    int tunnelState() const;          // 0 idle, 1 starting, 2 up
    int pillWidth() const;            // current width of the LIVE pill

    ListenLinkProcessor& processor;

    MeterPanel meter;
    QualityButton qualityButton;
    StyledButton copyPublicButton { "Copy", StyledButton::Style::normal };
    StyledButton createButton     { "Create public link", StyledButton::Style::accentBtn };
    StyledButton stopButton       { "Stop", StyledButton::Style::danger };
    StyledButton updateButton     { "Update", StyledButton::Style::accentBtn };
    juce::Label publicUrlLabel;
    QualityPopup popup;

    float dispL = 0, dispR = 0, holdL = 0, holdR = 0;
    int tick = 0, copiedPublic = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ListenLinkEditor)
};
