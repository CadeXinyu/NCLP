#include "NCLPEditor.h"

#include "NCLPImpedanceCanvas.h"

#include <algorithm>
#include <cmath>

namespace nclp
{
namespace
{
// Open Ephys renders source editors in a 136 px-high strip whose body begins
// at y=23.  Keep every control clear of that divider and leave a visible outer
// margin; these named bounds are also checked for overlap at compile time.
struct EditorBounds
{
    int x;
    int y;
    int width;
    int height;

    constexpr int right() const { return x + width; }
    constexpr int bottom() const { return y + height; }
};

constexpr bool boundsOverlap(const EditorBounds& first,
                             const EditorBounds& second)
{
    return first.x < second.right() && second.x < first.right() &&
           first.y < second.bottom() && second.y < first.bottom();
}

template <size_t Count>
constexpr bool boundsDoNotOverlap(
    const std::array<EditorBounds, Count>& bounds)
{
    for (size_t first = 0; first < Count; ++first)
        for (size_t second = first + 1; second < Count; ++second)
            if (boundsOverlap(bounds[first], bounds[second]))
                return false;
    return true;
}

template <size_t Count>
constexpr bool boundsStayInsideBody(
    const std::array<EditorBounds, Count>& bounds,
    int rightLimit)
{
    for (const EditorBounds& item : bounds)
    {
        if (item.x < 3 || item.y < 25 || item.right() > rightLimit ||
            item.bottom() > 126)
            return false;
    }
    return true;
}

namespace editorLayout
{
constexpr int width = 448;
constexpr std::array<EditorBounds, kPortCount> ports = {{
    {3, 28, 120, 18},
    {3, 48, 120, 18},
    {3, 68, 120, 18},
    {3, 88, 120, 18}
}};
constexpr EditorBounds connect{5, 108, 62, 18};
constexpr EditorBounds check{72, 108, 36, 18};
constexpr EditorBounds rescan{113, 108, 47, 18};
constexpr EditorBounds reset{165, 108, 45, 18};
constexpr EditorBounds sampleRate{130, 27, 80, 30};
constexpr EditorBounds bandwidth{130, 59, 80, 47};
constexpr EditorBounds auxTitle{216, 27, 30, 18};
constexpr EditorBounds aux{250, 27, 70, 18};
constexpr EditorBounds dspRequestedTitle{252, 54, 32, 12};
constexpr EditorBounds dspActualTitle{288, 54, 32, 12};
constexpr EditorBounds dsp{216, 68, 32, 18};
constexpr EditorBounds dspRequested{252, 68, 32, 18};
constexpr EditorBounds dspActual{288, 68, 32, 18};
constexpr EditorBounds algo{216, 108, 50, 18};
constexpr EditorBounds out{270, 108, 50, 18};
constexpr EditorBounds impedance{328, 108, 108, 18};
constexpr EditorBounds status{328, 27, 108, 18};
constexpr EditorBounds counters{328, 49, 108, 18};
constexpr EditorBounds ttlTitle{328, 71, 108, 12};
constexpr EditorBounds ttl{328, 86, 108, 18};

constexpr std::array<EditorBounds, 24> controls = {{
    ports[0], ports[1], ports[2], ports[3],
    connect, check, rescan, reset, sampleRate, bandwidth,
    auxTitle, aux, dspRequestedTitle, dspActualTitle,
    dsp, dspRequested, dspActual, algo, out, impedance,
    status, counters, ttlTitle, ttl
}};
} // namespace editorLayout

static_assert(boundsDoNotOverlap(editorLayout::controls),
              "NCLP editor controls overlap");
static_assert(boundsStayInsideBody(editorLayout::controls,
                                  editorLayout::width - 12),
              "NCLP editor control touches the title divider or outer edge");

void applyBounds(Component& component, const EditorBounds& bounds)
{
    component.setBounds(bounds.x, bounds.y, bounds.width, bounds.height);
}

void styleUtilityButton(UtilityButton& button, float fontSize = 9.0f)
{
    button.setRadius(3.0f);
    button.setFont(FontOptions("Small Text", fontSize, Font::plain));
}

void styleCaption(Label& label, float fontSize = 9.0f)
{
    label.setFont(FontOptions("Small Text", fontSize, Font::plain));
    label.setColour(Label::textColourId, Colours::darkgrey);
}

void styleReadout(Label& label)
{
    label.setJustificationType(Justification::centred);
    label.setFont(FontOptions("Small Text", 9.0f, Font::plain));
    label.setMinimumHorizontalScale(0.55f);
    label.setColour(Label::backgroundColourId, Colours::lightgrey);
    label.setColour(Label::textColourId, Colours::darkgrey);
}

String formatSampleRate(uint32_t sampleRateHz)
{
    const int decimals = sampleRateHz < 10000U ? 2 : 1;
    return String(static_cast<double>(sampleRateHz) / 1000.0, decimals) + " kS/s";
}

String formatMebibytes(int bytes)
{
    if (bytes <= 0)
        return "unknown";
    return String(static_cast<double>(bytes) / (1024.0 * 1024.0), 2) + " MiB";
}

String formatSamplesAndMilliseconds(size_t samples, uint32_t sampleRateHz)
{
    String result = String(static_cast<int64>(samples));
    if (sampleRateHz > 0U)
    {
        result += " (" +
                  String(1000.0 * static_cast<double>(samples) /
                             static_cast<double>(sampleRateHz),
                         2) +
                  " ms)";
    }
    return result;
}

String chipName(const LaneDetection& lane)
{
    return lane.detected ? String(chipTypeName(lane.chip)) : String();
}

String formatLowerBandwidth(uint32_t milliHz)
{
    const int decimals = milliHz % 1000U == 0U ? 0 :
                         milliHz % 100U == 0U ? 1 :
                         milliHz % 10U == 0U ? 2 : 3;
    return String(static_cast<double>(milliHz) / 1000.0, decimals);
}

bool sameAcquisitionConfig(const AcquisitionConfig& first,
                           const AcquisitionConfig& second)
{
    return first.generation == second.generation &&
           first.sampleRateHz == second.sampleRateHz &&
           first.flags == second.flags &&
           first.analogLowerMilliHz == second.analogLowerMilliHz &&
           first.analogUpperHz == second.analogUpperHz &&
           first.dspRequestedMilliHz == second.dspRequestedMilliHz &&
           first.dspActualMilliHz == second.dspActualMilliHz &&
           first.dspCode == second.dspCode &&
           first.ttlSettleChannel == second.ttlSettleChannel &&
           first.initialized == second.initialized &&
           first.dspEnabled == second.dspEnabled &&
           first.ttlSettleEnabled == second.ttlSettleEnabled;
}
} // namespace

PortSummaryComponent::PortSummaryComponent(const String& portName)
    : portName_(portName)
{
    firstCell_ = std::make_unique<UtilityButton>(" ");
    firstCell_->setRadius(3.0f);
    firstCell_->setBounds(23, 1, 20, 17);
    firstCell_->setFont(FontOptions("Small Text", 13.0f, Font::plain));
    firstCell_->setCorners(true, false, true, false);
    firstCell_->setInterceptsMouseClicks(false, false);
    firstCell_->setEnabledState(false);
    addAndMakeVisible(firstCell_.get());

    secondCell_ = std::make_unique<UtilityButton>(" ");
    secondCell_->setRadius(3.0f);
    secondCell_->setBounds(43, 1, 20, 17);
    secondCell_->setFont(FontOptions("Small Text", 13.0f, Font::plain));
    secondCell_->setCorners(false, true, false, true);
    secondCell_->setInterceptsMouseClicks(false, false);
    secondCell_->setEnabledState(false);
    addAndMakeVisible(secondCell_.get());

    firstPhaseCell_ = std::make_unique<UtilityButton>("None");
    firstPhaseCell_->setRadius(3.0f);
    firstPhaseCell_->setBounds(66, 1, 27, 17);
    firstPhaseCell_->setFont(FontOptions("Small Text", 7.0f, Font::plain));
    firstPhaseCell_->setCorners(true, false, true, false);
    firstPhaseCell_->setInterceptsMouseClicks(false, false);
    firstPhaseCell_->setEnabledState(false);
    addAndMakeVisible(firstPhaseCell_.get());

    secondPhaseCell_ = std::make_unique<UtilityButton>("None");
    secondPhaseCell_->setRadius(3.0f);
    secondPhaseCell_->setBounds(93, 1, 27, 17);
    secondPhaseCell_->setFont(FontOptions("Small Text", 7.0f, Font::plain));
    secondPhaseCell_->setCorners(false, true, false, true);
    secondPhaseCell_->setInterceptsMouseClicks(false, false);
    secondPhaseCell_->setEnabledState(false);
    addAndMakeVisible(secondPhaseCell_.get());
}

void PortSummaryComponent::setLanes(const LaneDetection& first,
                                    const LaneDetection& second,
                                    const String& fullSummary,
                                    bool scanValid)
{
    active_ = scanValid && (first.detected || second.detected);
    updateCell(*firstCell_, first, portName_ + "1", scanValid);
    updateCell(*secondCell_, second, portName_ + "2", scanValid);
    updatePhaseCell(*firstPhaseCell_, first, portName_ + "1", scanValid);
    updatePhaseCell(*secondPhaseCell_, second, portName_ + "2", scanValid);
    setTooltip(fullSummary);
    repaint();
}

void PortSummaryComponent::updateCell(UtilityButton& cell,
                                      const LaneDetection& lane,
                                      const String& laneName,
                                      bool scanValid)
{
    const bool detected = scanValid && lane.detected;
    cell.setLabel(detected ? String(static_cast<int>(lane.channelCount)) : " ");
    cell.setEnabledState(detected);
    cell.setToggleState(false, dontSendNotification);
    cell.setTooltip(detected
                        ? laneName + ": " + chipName(lane) + " — " +
                              String(static_cast<int>(lane.channelCount)) + " ch"
                        : laneName + (scanValid ? ": No headstage" : ": Not scanned"));
}

void PortSummaryComponent::updatePhaseCell(UtilityButton& cell,
                                           const LaneDetection& lane,
                                           const String& laneName,
                                           bool scanValid)
{
    const bool detected = scanValid && lane.detected;
    if (! detected)
    {
        cell.setLabel("None");
        cell.setEnabledState(false);
        cell.setToggleState(false, dontSendNotification);
        cell.setTooltip(laneName +
                        (scanValid
                             ? ": No headstage; no cable-delay phase"
                             : ": Not scanned; no cable-delay phase"));
        return;
    }

    if (! lane.hasSelectedPhase())
    {
        cell.setLabel("?");
        cell.setEnabledState(false);
        cell.setToggleState(false, dontSendNotification);
        cell.setTooltip(laneName +
                        ": Headstage detected, but no selected phase was returned");
        return;
    }

    const String phaseText = String(lane.selectedPhaseTap) + "/" +
                             String(static_cast<int>(kCablePhaseTapLast));
    cell.setLabel(phaseText);
    cell.setEnabledState(true);
    cell.setToggleState(false, dontSendNotification);
    String tooltip = laneName + ": selected effective cable-delay phase " +
                     phaseText;
    if (lane.chip == ChipType::RHD2164)
        tooltip += ". RHD2164 reports max(MISO-A, MISO-B)";
    else
        tooltip += ". This is the MISO-A phase";
    cell.setTooltip(tooltip);
}

void PortSummaryComponent::paint(Graphics& g)
{
    g.setColour(Colours::lightgrey);
    g.fillRoundedRectangle(5.0f, 0.0f, 58.0f,
                           static_cast<float>(getHeight()), 4.0f);
    g.setColour(active_ ? Colours::black : Colours::grey);
    g.setFont(FontOptions("Small Text", 15.0f, Font::plain));
    g.drawText(portName_, 8, 2, 14, 15, Justification::left, false);
}

SampleRateInterface::SampleRateInterface(ChangeCallback onChange)
    : onChange_(std::move(onChange))
{
    rateSelection_ = std::make_unique<ComboBox>("Sample Rate");
    for (uint32_t sampleRate : kSupportedSampleRatesHz)
        rateSelection_->addItem(formatSampleRate(sampleRate), static_cast<int>(sampleRate));
    rateSelection_->setBounds(0, 12, 80, 18);
    rateSelection_->addListener(this);
    addAndMakeVisible(rateSelection_.get());
}

void SampleRateInterface::paint(Graphics& g)
{
    g.setColour(Colours::darkgrey);
    g.setFont(FontOptions("Small Text", 10.0f, Font::plain));
    g.drawText("Sample Rate", 0, 0, 80, 11, Justification::left, false);
}

void SampleRateInterface::setSelectedRate(uint32_t sampleRateHz)
{
    if (sampleRateHz == 0U)
        return;
    suppressCallback_ = true;
    rateSelection_->setSelectedId(static_cast<int>(sampleRateHz), dontSendNotification);
    suppressCallback_ = false;
}

void SampleRateInterface::setControlEnabled(bool enabled)
{
    rateSelection_->setEnabled(enabled);
}

void SampleRateInterface::comboBoxChanged(ComboBox* comboBox)
{
    if (! suppressCallback_ && comboBox == rateSelection_.get() && onChange_)
        onChange_(static_cast<uint32_t>(rateSelection_->getSelectedId()));
}

BandwidthInterface::BandwidthInterface(ChangeCallback onChange)
    : onChange_(std::move(onChange))
{
    lowerSelection_ = std::make_unique<Label>("LowerBandwidth", "0.1");
    lowerSelection_->setEditable(true, false, false);
    lowerSelection_->addListener(this);
    // Match the reference Acquisition Board geometry. Keeping the fields
    // inside the 80 px parent prevents their editor outlines from being
    // clipped into the neighbouring section.
    lowerSelection_->setBounds(25, 10, 50, 18);
    addAndMakeVisible(lowerSelection_.get());

    upperSelection_ = std::make_unique<Label>("UpperBandwidth", "7500");
    upperSelection_->setEditable(true, false, false);
    upperSelection_->addListener(this);
    upperSelection_->setBounds(25, 29, 50, 18);
    addAndMakeVisible(upperSelection_.get());

    lookAndFeelChanged();
}

void BandwidthInterface::paint(Graphics& g)
{
    g.setColour(Colours::darkgrey);
    g.setFont(FontOptions("Small Text", 10.0f, Font::plain));
    g.drawText("Bandwidth", 0, 0, 80, 10, Justification::left, false);
    g.drawText("Low:", 0, 10, 24, 18, Justification::left, false);
    g.drawText("High:", 0, 29, 24, 18, Justification::left, false);
}

void BandwidthInterface::lookAndFeelChanged()
{
    if (lowerSelection_ == nullptr || upperSelection_ == nullptr)
        return;
    for (Label* label : { lowerSelection_.get(), upperSelection_.get() })
        label->setColour(Label::textColourId, Colours::darkgrey);
}

void BandwidthInterface::setValues(uint32_t lowerMilliHz, uint32_t upperHz)
{
    if (lowerMilliHz == 0U || upperHz == 0U)
        return;
    lowerHz_ = static_cast<double>(lowerMilliHz) / 1000.0;
    upperHz_ = static_cast<double>(upperHz);
    suppressCallback_ = true;
    lowerSelection_->setText(formatLowerBandwidth(lowerMilliHz),
                             dontSendNotification);
    upperSelection_->setText(String(upperHz), dontSendNotification);
    suppressCallback_ = false;
}

void BandwidthInterface::setControlEnabled(bool enabled)
{
    lowerSelection_->setEnabled(enabled);
    upperSelection_->setEnabled(enabled);
}

void BandwidthInterface::labelTextChanged(Label* label)
{
    if (suppressCallback_ || (label != lowerSelection_.get() && label != upperSelection_.get()))
        return;

    const double requested = label->getText().getDoubleValue();
    const bool lowerChanged = label == lowerSelection_.get();
    const double candidateLower = lowerChanged ? requested : lowerHz_;
    const double candidateUpper = lowerChanged ? upperHz_ : requested;
    const bool valid = std::isfinite(candidateLower) && std::isfinite(candidateUpper) &&
                       candidateLower >= 0.1 && candidateLower <= 500.0 &&
                       candidateUpper >= 100.0 && candidateUpper <= 20000.0 &&
                       candidateLower < candidateUpper;
    if (! valid)
    {
        lowerSelection_->setText(
            formatLowerBandwidth(static_cast<uint32_t>(std::llround(lowerHz_ * 1000.0))),
            dontSendNotification);
        upperSelection_->setText(String(static_cast<uint32_t>(std::llround(upperHz_))),
                                 dontSendNotification);
        CoreServices::sendStatusMessage("Analog bandwidth value out of range.");
        return;
    }

    lowerHz_ = candidateLower;
    upperHz_ = candidateUpper;
    if (onChange_)
        onChange_(lowerHz_, upperHz_);
}

NCLPEditor::NCLPEditor(GenericProcessor* parentNode, DataThreadPlugin* node_)
    : VisualizerEditor(parentNode, "NCLP Settings", editorLayout::width),
      asyncAccess_(node_ != nullptr ? node_->asyncAccess() : nullptr),
      selectedPage_(NCLPCanvasPage::Impedance)
{
    for (size_t port = 0; port < portSummaries.size(); ++port)
    {
        const String portName = String::charToString(
            static_cast<juce_wchar>('A' + static_cast<int>(port)));

        portSummaries[port] = std::make_unique<PortSummaryComponent>(portName);
        applyBounds(*portSummaries[port], editorLayout::ports[port]);
        addAndMakeVisible(portSummaries[port].get());
    }

    connectButton = std::make_unique<UtilityButton>("CONNECT");
    applyBounds(*connectButton, editorLayout::connect);
    styleUtilityButton(*connectButton);
    connectButton->setTooltip("Open or re-open the NCLP control connection");
    connectButton->addListener(this);
    addAndMakeVisible(connectButton.get());

    checkButton = std::make_unique<UtilityButton>("CHECK");
    applyBounds(*checkButton, editorLayout::check);
    styleUtilityButton(*checkButton, 8.5f);
    checkButton->setTooltip("Check control connection and board status");
    checkButton->addListener(this);
    addAndMakeVisible(checkButton.get());

    rescanButton = std::make_unique<UtilityButton>("RESCAN");
    applyBounds(*rescanButton, editorLayout::rescan);
    styleUtilityButton(*rescanButton);
    rescanButton->setTooltip("Detect connected Intan chips again");
    rescanButton->addListener(this);
    addAndMakeVisible(rescanButton.get());

    resetButton = std::make_unique<UtilityButton>("RESET");
    applyBounds(*resetButton, editorLayout::reset);
    styleUtilityButton(*resetButton);
    resetButton->setTooltip("Reset acquisition and output controls, disable stimulation and sync. Rescan afterward.");
    resetButton->addListener(this);
    addAndMakeVisible(resetButton.get());

    sampleRateInterface = std::make_unique<SampleRateInterface>(
        [this](uint32_t rate) { sampleRateChanged(rate); });
    applyBounds(*sampleRateInterface, editorLayout::sampleRate);
    addAndMakeVisible(sampleRateInterface.get());

    bandwidthInterface = std::make_unique<BandwidthInterface>(
        [this](double low, double high) { bandwidthChanged(low, high); });
    applyBounds(*bandwidthInterface, editorLayout::bandwidth);
    addAndMakeVisible(bandwidthInterface.get());

    // Match the Acquisition Board convention: the button reads +3 CH when
    // each detected physical headstage contributes AUX1/2/3, and OFF when
    // only amplifier channels are published.
    auxTitleLabel = std::make_unique<Label>("AuxTitle", "AUX");
    applyBounds(*auxTitleLabel, editorLayout::auxTitle);
    auxTitleLabel->setJustificationType(Justification::centredLeft);
    styleCaption(*auxTitleLabel, 10.0f);
    addAndMakeVisible(auxTitleLabel.get());

    auxButton = std::make_unique<UtilityButton>("+3 CH");
    applyBounds(*auxButton, editorLayout::aux);
    styleUtilityButton(*auxButton);
    auxButton->setClickingTogglesState(true);
    auxButton->setToggleState(true, dontSendNotification);
    auxButton->setTooltip(
        "Publish AUX1/2/3 for each detected physical headstage. OFF publishes "
        "amplifier channels only. This setting can change only while stopped.");
    auxButton->addListener(this);
    addAndMakeVisible(auxButton.get());

    dspButton = std::make_unique<UtilityButton>("DSP");
    applyBounds(*dspButton, editorLayout::dsp);
    styleUtilityButton(*dspButton, 8.5f);
    dspButton->setClickingTogglesState(true);
    dspButton->setTooltip("Toggle the Intan on-chip DSP high-pass filter");
    dspButton->addListener(this);
    addAndMakeVisible(dspButton.get());

    dspRequestedTitle = std::make_unique<Label>("DspRequestedTitle", "Req.");
    applyBounds(*dspRequestedTitle, editorLayout::dspRequestedTitle);
    dspRequestedTitle->setJustificationType(Justification::centred);
    styleCaption(*dspRequestedTitle, 8.0f);
    addAndMakeVisible(dspRequestedTitle.get());

    dspActualTitle = std::make_unique<Label>("DspActualTitle", "Act.");
    applyBounds(*dspActualTitle, editorLayout::dspActualTitle);
    dspActualTitle->setJustificationType(Justification::centred);
    styleCaption(*dspActualTitle, 8.0f);
    addAndMakeVisible(dspActualTitle.get());

    dspRequestedValue = std::make_unique<Label>("DspRequestedValue", "-");
    applyBounds(*dspRequestedValue, editorLayout::dspRequested);
    dspRequestedValue->setEditable(true, false, false);
    styleReadout(*dspRequestedValue);
    dspRequestedValue->setTooltip("Requested DSP high-pass cutoff in Hz");
    dspRequestedValue->addListener(this);
    addAndMakeVisible(dspRequestedValue.get());

    dspActualValue = std::make_unique<Label>("DspActualValue", "-");
    applyBounds(*dspActualValue, editorLayout::dspActual);
    styleReadout(*dspActualValue);
    dspActualValue->setTooltip("Nearest DSP cutoff realizable by the Intan chip");
    addAndMakeVisible(dspActualValue.get());

    // Page selectors all target one persistent Visualizer canvas. Keeping the
    // impedance page alive preserves its measurement worker and scroll state.
    algoButton = std::make_unique<UtilityButton>("ALGO");
    applyBounds(*algoButton, editorLayout::algo);
    styleUtilityButton(*algoButton);
    algoButton->setTooltip("Configure the ripple FIR and PWT detector");
    algoButton->addListener(this);
    addAndMakeVisible(algoButton.get());

    outButton = std::make_unique<UtilityButton>("OUT");
    applyBounds(*outButton, editorLayout::out);
    styleUtilityButton(*outButton);
    outButton->setTooltip("Configure Intan sync, TTL routing, and DAC playback");
    outButton->addListener(this);
    addAndMakeVisible(outButton.get());

    impedanceButton = std::make_unique<UtilityButton>("IMPEDANCE");
    applyBounds(*impedanceButton, editorLayout::impedance);
    styleUtilityButton(*impedanceButton, 8.5f);
    impedanceButton->setTooltip("Show impedance measurement settings");
    impedanceButton->addListener(this);
    addAndMakeVisible(impedanceButton.get());
    updateCanvasPageButtons();

    ttlFastSettleTitle = std::make_unique<Label>("TtlFastSettleTitle", "TTL Settle");
    applyBounds(*ttlFastSettleTitle, editorLayout::ttlTitle);
    ttlFastSettleTitle->setJustificationType(Justification::centredLeft);
    styleCaption(*ttlFastSettleTitle, 9.0f);
    ttlFastSettleTitle->setTooltip("Amplifier fast-settle TTL input");
    addAndMakeVisible(ttlFastSettleTitle.get());

    // '-' disables settle. Display the hardware/protocol channel numbers
    // directly as TTL0..TTL15 so the UI does not hide a one-based conversion.
    ttlFastSettleCombo = std::make_unique<ComboBox>("TtlFastSettle");
    ttlFastSettleCombo->addItem("-", 1);
    for (int channel = 0; channel < 16; ++channel)
        ttlFastSettleCombo->addItem("TTL" + String(channel), channel + 2);
    applyBounds(*ttlFastSettleCombo, editorLayout::ttl);
    ttlFastSettleCombo->setTooltip(
        "Select the TTL input that enables amplifier fast settle; '-' disables it. "
        "Only select a line that is LOW while idle: a HIGH line holds all "
        "amplifiers in fast settle.");
    ttlFastSettleCombo->addListener(this);
    addAndMakeVisible(ttlFastSettleCombo.get());

    statusLabel = std::make_unique<Label>("NCLPStatus", "Disconnected");
    applyBounds(*statusLabel, editorLayout::status);
    styleReadout(*statusLabel);
    addAndMakeVisible(statusLabel.get());

    packetCountersLabel = std::make_unique<Label>(
        "PacketCounters", "Lost 0 | Drop 0");
    applyBounds(*packetCountersLabel, editorLayout::counters);
    styleReadout(*packetCountersLabel);
    packetCountersLabel->setTooltip(
        "UDP packets received; sequence gaps; kernel, late/duplicate, malformed, "
        "listener-queue, and Open Ephys FIFO drops");
    addAndMakeVisible(packetCountersLabel.get());

    if (asyncAccess_ != nullptr)
    {
        (void) asyncAccess_->run([this](DataThreadPlugin& backend)
        {
            latestSnapshot_ = backend.sessionSnapshot();
        });
    }
    updateSnapshot(latestSnapshot_);
    startTimerHz(4);
}

NCLPEditor::~NCLPEditor()
{
    stopTimer();
}

void NCLPEditor::paint(Graphics& g)
{
    VisualizerEditor::paint(g);
}

void NCLPEditor::lookAndFeelChanged()
{
    VisualizerEditor::lookAndFeelChanged();
    repaint();
}

void NCLPEditor::buttonClicked(Button* button)
{
    if (button == impedanceButton.get())
    {
        showCanvasPage(NCLPCanvasPage::Impedance);
        return;
    }
    if (button == algoButton.get())
    {
        showCanvasPage(NCLPCanvasPage::Algorithm);
        return;
    }
    if (button == outButton.get())
    {
        showCanvasPage(NCLPCanvasPage::Output);
        return;
    }

    if (asyncAccess_ == nullptr)
        return;

    if (button == connectButton.get())
    {
        if (latestSnapshot_.connected || latestSnapshot_.streaming)
        {
            runAsyncCommand("Reconnecting...", [](DataThreadPlugin& backend)
            {
                (void) backend.reconnectDevice(false);
            }, true);
        }
        else
        {
            runAsyncCommand("Connecting...", [](DataThreadPlugin& backend)
            {
                (void) backend.connectDevice(false);
            }, true);
        }
    }
    else if (button == checkButton.get())
    {
        runAsyncCommand("Checking connection...", [](DataThreadPlugin& backend)
        {
            (void) backend.checkConnection();
        });
    }
    else if (button == rescanButton.get())
    {
        runAsyncScanCommand(
            "Scanning headstages...",
            [](DataThreadPlugin& backend,
               const DataThreadPlugin::ScanProgressCallback& progressCallback)
        {
            (void) backend.scanDevice(progressCallback);
        }, true);
    }
    else if (button == resetButton.get())
    {
        runAsyncCommand("Resetting board...", [](DataThreadPlugin& backend)
        {
            (void) backend.resetDevice();
        }, true);
    }
    else if (button == auxButton.get())
    {
        const bool enabled = auxButton->getToggleState();
        auxButton->setLabel(enabled ? "+3 CH" : "OFF");
        runAsyncCommand(enabled ? "Enabling AUX channels..."
                                : "Disabling AUX channels...",
                        [enabled](DataThreadPlugin& backend)
        {
            (void) backend.setAuxChannelsEnabled(enabled);
        }, true);
    }
    else if (button == dspButton.get())
    {
        const bool enabled = dspButton->getToggleState();
        const uint32_t cutoff = requestedDspMilliHz();
        if (cutoff == 0U)
        {
            setStatusText("DSP cutoff must be greater than zero");
            updateConfigControls(latestSnapshot_.config);
            return;
        }

        runAsyncCommand("Updating DSP high-pass...", [enabled, cutoff](DataThreadPlugin& backend)
        {
            (void) backend.setDsp(enabled, cutoff);
        });
    }
}

void NCLPEditor::comboBoxChanged(ComboBox* comboBox)
{
    if (suppressConfigCallbacks || asyncAccess_ == nullptr)
        return;

    if (comboBox != ttlFastSettleCombo.get())
        return;

    const int selectedId = ttlFastSettleCombo->getSelectedId();
    const bool enabled = selectedId > 1;
    const uint32_t zeroBasedChannel = enabled
        ? static_cast<uint32_t>(selectedId - 2)
        : 0U;
    runAsyncCommand("Changing TTL fast settle...",
                    [enabled, zeroBasedChannel](DataThreadPlugin& backend)
    {
        (void) backend.setTtlFastSettle(enabled, zeroBasedChannel);
    });
}

void NCLPEditor::sampleRateChanged(uint32_t sampleRateHz)
{
    if (suppressConfigCallbacks || asyncAccess_ == nullptr || sampleRateHz == 0U)
        return;
    runAsyncScanCommand(
        "Changing sample rate...",
        [sampleRateHz](
            DataThreadPlugin& backend,
            const DataThreadPlugin::ScanProgressCallback& progressCallback)
    {
        (void) backend.setSampleRate(sampleRateHz, progressCallback);
    }, true);
}

void NCLPEditor::bandwidthChanged(double lowerHz, double upperHz)
{
    if (suppressConfigCallbacks || asyncAccess_ == nullptr || ! std::isfinite(lowerHz) ||
        ! std::isfinite(upperHz) || lowerHz <= 0.0 || lowerHz >= upperHz)
        return;
    const uint32_t requestedLowerMilliHz =
        static_cast<uint32_t>(std::llround(lowerHz * 1000.0));
    const uint32_t requestedUpperHz =
        static_cast<uint32_t>(std::llround(upperHz));
    uint32_t lowerMilliHz = 0U;
    uint32_t upperWholeHz = 0U;
    if (! quantizeAnalogBandwidth(requestedLowerMilliHz, requestedUpperHz,
                                  lowerMilliHz, upperWholeHz))
    {
        setStatusText("Nearest bandwidth presets overlap: " +
                      formatLowerBandwidth(lowerMilliHz) + "-" +
                      String(upperWholeHz) + " Hz");
        updateConfigControls(latestSnapshot_.config);
        return;
    }

    const bool quantized = lowerMilliHz != requestedLowerMilliHz ||
                           upperWholeHz != requestedUpperHz;
    if (quantized)
        bandwidthInterface->setValues(lowerMilliHz, upperWholeHz);

    const String busyText = quantized
        ? "Applying nearest bandwidth " + formatLowerBandwidth(lowerMilliHz) +
              "-" + String(upperWholeHz) + " Hz..."
        : "Changing analog bandwidth...";
    runAsyncCommand(busyText,
                    [lowerMilliHz, upperWholeHz](DataThreadPlugin& backend)
    {
        (void) backend.setBandwidth(lowerMilliHz, upperWholeHz);
    });
}

void NCLPEditor::labelTextChanged(Label* label)
{
    if (suppressConfigCallbacks || label != dspRequestedValue.get() ||
        asyncAccess_ == nullptr)
        return;

    const uint32_t cutoff = requestedDspMilliHz();
    if (cutoff == 0U)
    {
        setStatusText("DSP cutoff must be greater than zero");
        updateConfigControls(latestSnapshot_.config);
        return;
    }

    const bool enabled = dspButton->getToggleState();
    runAsyncCommand("Changing DSP cutoff...", [enabled, cutoff](DataThreadPlugin& backend)
    {
        (void) backend.setDsp(enabled, cutoff);
    });
}

void NCLPEditor::startAcquisition()
{
    acquisitionIsActive = true;
    refreshControls();
    if (impedanceCanvas != nullptr)
        impedanceCanvas->beginAnimation();
}

void NCLPEditor::stopAcquisition()
{
    acquisitionIsActive = false;
    if (asyncAccess_ != nullptr)
    {
        SessionSnapshot snapshot = latestSnapshot_;
        // ProcessorGraph invokes editorStopAcquisition() before the backend
        // stop. Never hold up Stop behind an in-flight CHECK/STATUS command.
        if (asyncAccess_->tryRun([&snapshot](DataThreadPlugin& backend)
            {
                snapshot = backend.sessionSnapshot();
            }))
            latestSnapshot_ = snapshot;
    }
    refreshControls();
    if (impedanceCanvas != nullptr)
        impedanceCanvas->endAnimation();
}

Visualizer* NCLPEditor::createNewCanvas()
{
    impedanceCanvas = new NCLPImpedanceCanvas(asyncAccess_);
    impedanceCanvas->updateSnapshot(latestSnapshot_);
    impedanceCanvas->setPage(selectedPage_);
    return impedanceCanvas;
}

void NCLPEditor::saveVisualizerEditorParameters(XmlElement* xml)
{
    if (xml == nullptr || asyncAccess_ == nullptr)
        return;

    int scheme = 1;
    bool auxEnabled = true;
    if (asyncAccess_->run([&scheme, &auxEnabled](DataThreadPlugin& backend)
        {
            scheme = backend.channelNamingScheme();
            auxEnabled = backend.auxChannelsEnabled();
        }))
    {
        xml->setAttribute("Channel_Naming_Scheme", scheme);
        xml->setAttribute("AUXsOn", auxEnabled);
    }
}

void NCLPEditor::loadVisualizerEditorParameters(XmlElement* xml)
{
    if (xml == nullptr || asyncAccess_ == nullptr)
        return;

    const int scheme = xml->getIntAttribute("Channel_Naming_Scheme", 1);
    const bool requestedAuxEnabled = xml->getBoolAttribute("AUXsOn", true);
    bool auxChanged = false;
    SessionSnapshot snapshot = latestSnapshot_;
    const bool loaded = asyncAccess_->run(
        [scheme, requestedAuxEnabled, &auxChanged, &snapshot](DataThreadPlugin& backend)
    {
        const bool previousAuxEnabled = backend.auxChannelsEnabled();
        (void) backend.setChannelNamingScheme(scheme == 2 ? 2 : 1, false);
        (void) backend.setAuxChannelsEnabled(requestedAuxEnabled);
        auxChanged = previousAuxEnabled != backend.auxChannelsEnabled();
        snapshot = backend.sessionSnapshot();
    });
    if (loaded)
        updateSnapshot(snapshot, true);
    if (loaded && auxChanged)
        CoreServices::updateSignalChain(this);
    if (impedanceCanvas != nullptr)
        impedanceCanvas->updateSettings();
}

void NCLPEditor::timerCallback()
{
    if (asyncAccess_ == nullptr || localCommandInFlight.load())
        return;

    SessionSnapshot snapshot = latestSnapshot_;
    // Never wait on the JUCE message thread. A long SCAN/INIT owns AsyncAccess
    // until it finishes; missing a status tick is preferable to freezing every
    // editor interaction behind that operation.
    if (asyncAccess_->tryRun([&snapshot](DataThreadPlugin& backend)
        {
            snapshot = backend.sessionSnapshot();
        }))
        updateSnapshot(snapshot);
}

void NCLPEditor::setStatusText(const String& text, bool broadcast)
{
    const String displayed = text.isNotEmpty() ? text : "NCLP";
    statusLabel->setText(displayed, dontSendNotification);
    statusLabel->setTooltip(displayed);
    if (broadcast && text.isNotEmpty())
        CoreServices::sendStatusMessage(text);
}

void NCLPEditor::updateSnapshot(const SessionSnapshot& snapshot,
                                bool forceConfigRefresh)
{
    const bool configChanged = ! configControlsInitialized_ ||
                               ! sameAcquisitionConfig(latestSnapshot_.config,
                                                       snapshot.config);
    latestSnapshot_ = snapshot;
    const bool auxEnabled = snapshot.sampleMode == SampleMode::Aux;
    auxButton->setToggleState(auxEnabled, dontSendNotification);
    auxButton->setLabel(auxEnabled ? "+3 CH" : "OFF");
    updatePortSummaries(snapshot.topology, snapshot.scanValid);
    updateScanProgress(snapshot.connected ? snapshot.progress
                                          : ProgressSnapshot{});
    // The timer polls counters/state at 4 Hz. Rewriting an editable Label on
    // every poll destroys its TextEditor while the user is typing. Only copy
    // configuration into the widgets when hardware readback actually changes,
    // or after a command completes (the forced refresh also rolls back a
    // rejected value to the authoritative board value).
    if (forceConfigRefresh || configChanged)
    {
        updateConfigControls(snapshot.config);
        configControlsInitialized_ = true;
    }
    connectButton->setLabel(snapshot.connected || snapshot.streaming ? "RECONNECT" : "CONNECT");
    // Keep the operation-specific text (for example, "Scanning...") visible
    // while the timer continues to refresh counters and topology.
    if (! localCommandInFlight.load())
        setStatusText(connectionStatusText(snapshot), false);
    const String counters = packetCounterText(snapshot);
    packetCountersLabel->setText(counters, dontSendNotification);
    const ParserStats& stats = snapshot.parserStats;
    const uint64_t queue = snapshot.queueOverruns;
    String diagnostics =
        "RX " + String(static_cast<int64>(stats.packets)) +
        " | Lost " + String(static_cast<int64>(stats.lostPackets)) +
        " | KDrop " + String(static_cast<int64>(snapshot.kernelDroppedPackets)) +
        " | Late " + String(static_cast<int64>(stats.latePackets + stats.duplicatePackets)) +
        " | Bad " + String(static_cast<int64>(stats.rejectedPackets)) +
        " | QDrop " + String(static_cast<int64>(queue)) +
        " | OEDrop " + String(static_cast<int64>(snapshot.consumerDroppedFrames)) +
        " | TsGap " + String(static_cast<int64>(stats.timestampGaps)) +
        " | TsMiss " + String(static_cast<int64>(stats.timestampMissing)) +
        "\nLost is a sequence gap. Drop is the compact total of malformed, late, "
        "queue, kernel-socket, and Open Ephys FIFO drops.";

    diagnostics += "\nUDP SO_RCVBUF: requested " +
                   formatMebibytes(snapshot.requestedReceiveBufferBytes);
    if (snapshot.receiveBufferQuerySucceeded)
        diagnostics += ", effective " +
                       formatMebibytes(snapshot.actualReceiveBufferBytes);
    else
        diagnostics += ", effective size not queried";
    if (! snapshot.receiveBufferWarning.empty())
        diagnostics += "\nWARNING: " + String(snapshot.receiveBufferWarning);

    diagnostics += "\nDecoded ring: " +
                   formatSamplesAndMilliseconds(snapshot.queuedFrames,
                                                snapshot.config.sampleRateHz) +
                   " / " +
                   String(static_cast<int64>(snapshot.queueCapacityFrames)) +
                   " frames | high " +
                   formatSamplesAndMilliseconds(snapshot.queuedFramesHighWater,
                                                snapshot.config.sampleRateHz);
    diagnostics += "\nOpen Ephys FIFO: " +
                   formatSamplesAndMilliseconds(snapshot.openEphysFifoSamples,
                                                snapshot.config.sampleRateHz) +
                   " / " +
                   String(static_cast<int64>(snapshot.openEphysFifoCapacitySamples)) +
                   " samples | high " +
                   formatSamplesAndMilliseconds(snapshot.openEphysFifoHighWater,
                                                snapshot.config.sampleRateHz);
    if (snapshot.dataGapEvents > 0U)
    {
        diagnostics += "\nDATA GAP events " +
                       String(static_cast<int64>(snapshot.dataGapEvents)) +
                       " | last missing " +
                       String(static_cast<int64>(snapshot.lastDataGapMissingSamples)) +
                       " before hardware timestamp " +
                       String(static_cast<int64>(snapshot.lastDataGapBeforeTimestamp));
    }
    packetCountersLabel->setTooltip(diagnostics);
    refreshControls();

    if (impedanceCanvas != nullptr)
        impedanceCanvas->updateSnapshot(snapshot);
}

void NCLPEditor::refreshControls()
{
    const bool streaming = acquisitionIsActive || latestSnapshot_.streaming;
    const bool busy = localCommandInFlight.load() || latestSnapshot_.commandBusy;
    const bool connected = latestSnapshot_.connected;
    const bool mutableEnabled = connected && ! streaming && ! busy;
    // If Open Ephys has stopped locally but firmware STOP remained
    // unconfirmed, Reconnect is the recovery action even when TCP still looks
    // connected. Keep it disabled only while local acquisition is active.
    const bool recoveryReconnect =
        latestSnapshot_.streaming && ! acquisitionIsActive;

    connectButton->setEnabledState((! streaming || recoveryReconnect) && ! busy);
    checkButton->setEnabledState(! busy);
    rescanButton->setEnabledState(mutableEnabled);
    resetButton->setEnabledState(mutableEnabled);
    auxButton->setEnabledState(! streaming && ! busy &&
                               ! latestSnapshot_.impedanceRunning);
    dspButton->setEnabledState(mutableEnabled);
    sampleRateInterface->setControlEnabled(mutableEnabled);
    bandwidthInterface->setControlEnabled(mutableEnabled);
    ttlFastSettleCombo->setEnabled(mutableEnabled);
    dspRequestedValue->setEnabled(mutableEnabled);
    // Page navigation is always local and non-blocking, including while SCAN
    // owns the backend. The canvas uses cached state when a fresh snapshot is
    // temporarily unavailable.
    algoButton->setEnabledState(true);
    outButton->setEnabledState(true);
    impedanceButton->setEnabledState(true);
}

void NCLPEditor::updateConfigControls(const AcquisitionConfig& config)
{
    suppressConfigCallbacks = true;

    sampleRateInterface->setSelectedRate(config.sampleRateHz);
    bandwidthInterface->setValues(config.analogLowerMilliHz, config.analogUpperHz);

    const int ttlSelection = config.ttlSettleEnabled && config.ttlSettleChannel < 16U
        ? static_cast<int>(config.ttlSettleChannel) + 2
        : 1;
    ttlFastSettleCombo->setSelectedId(ttlSelection, dontSendNotification);

    dspButton->setToggleState(config.dspEnabled, dontSendNotification);
    dspButton->setLabel("DSP");
    dspRequestedValue->setText(
        config.dspRequestedMilliHz != 0U
            ? String(static_cast<double>(config.dspRequestedMilliHz) / 1000.0, 3)
            : "-",
        dontSendNotification);
    dspActualValue->setText(
        config.dspActualMilliHz != 0U
            ? String(static_cast<double>(config.dspActualMilliHz) / 1000.0, 3)
            : "-",
        dontSendNotification);

    suppressConfigCallbacks = false;
}

void NCLPEditor::updatePortSummaries(const Topology& topology, bool scanValid)
{
    for (size_t port = 0; port < portSummaries.size(); ++port)
    {
        const size_t first = port * 2U;
        portSummaries[port]->setLanes(topology.lanes[first], topology.lanes[first + 1U],
                                      summaryForPort(port, topology, scanValid),
                                      scanValid && topology.valid);
    }
}

void NCLPEditor::updateScanProgress(const ProgressSnapshot& progress)
{
    latestSnapshot_.progress = progress;
}

void NCLPEditor::showCanvasPage(NCLPCanvasPage page)
{
    selectedPage_ = page;
    updateCanvasPageButtons();

    checkForCanvas();
    if (impedanceCanvas == nullptr)
        return;

    impedanceCanvas->setPage(page);

    const bool windowVisible = dataWindow != nullptr && dataWindow->isVisible();
    if (! isOpenInTab && ! windowVisible)
        addTab();
    editorWasClicked();
}

void NCLPEditor::updateCanvasPageButtons()
{
    impedanceButton->setToggleState(
        selectedPage_ == NCLPCanvasPage::Impedance,
        dontSendNotification);
    algoButton->setToggleState(selectedPage_ == NCLPCanvasPage::Algorithm,
                               dontSendNotification);
    outButton->setToggleState(selectedPage_ == NCLPCanvasPage::Output,
                              dontSendNotification);
}

void NCLPEditor::runAsyncCommand(const String& busyText,
                                 AsyncWork work,
                                 bool updateSignalChain)
{
    runAsyncCommandImpl(
        busyText,
        [work = std::move(work)](
            DataThreadPlugin& backend,
            const DataThreadPlugin::ScanProgressCallback&) mutable
        {
            work(backend);
        },
        false,
        updateSignalChain);
}

void NCLPEditor::runAsyncScanCommand(const String& busyText,
                                     ScanAsyncWork work,
                                     bool updateSignalChain)
{
    runAsyncCommandImpl(busyText, std::move(work), true, updateSignalChain);
}

void NCLPEditor::runAsyncCommandImpl(const String& busyText,
                                     ScanAsyncWork work,
                                     bool reportsScanProgress,
                                     bool updateSignalChain)
{
    const std::shared_ptr<DataThreadPlugin::AsyncAccess> access = asyncAccess_;
    if (access == nullptr || localCommandInFlight.exchange(true))
        return;

    if (reportsScanProgress)
        updateScanProgress({});
    setStatusText(busyText);
    refreshControls();

    Component::SafePointer<NCLPEditor> safeThis(this);
    Thread::launch([safeThis, access, work = std::move(work),
                    reportsScanProgress, updateSignalChain]() mutable
    {
        DataThreadPlugin::ScanProgressCallback progressCallback;
        if (reportsScanProgress)
        {
            progressCallback = [safeThis](const ProgressSnapshot& progress)
            {
                MessageManager::callAsync([safeThis, progress]() mutable
                {
                    if (safeThis != nullptr)
                        safeThis->updateScanProgress(progress);
                });
            };
        }

        SessionSnapshot snapshot;
        String status;
        const bool invoked = access->run([&](DataThreadPlugin& backend)
            {
                work(backend, progressCallback);
                snapshot = backend.sessionSnapshot();
                status = backend.lastStatusText();
            });

        MessageManager::callAsync(
            [safeThis, snapshot, status, reportsScanProgress,
             updateSignalChain, invoked]() mutable
        {
            if (safeThis == nullptr)
                return;

            safeThis->localCommandInFlight = false;
            if (! invoked)
            {
                safeThis->setStatusText("Command cancelled: source was removed");
                if (reportsScanProgress)
                    safeThis->updateScanProgress({});
                safeThis->refreshControls();
                return;
            }
            safeThis->updateSnapshot(snapshot, true);
            if (status.isNotEmpty())
                safeThis->setStatusText(status);
            if (updateSignalChain)
                CoreServices::updateSignalChain(safeThis.getComponent());
        });
    });
}

uint32_t NCLPEditor::requestedDspMilliHz() const
{
    const double hz = dspRequestedValue->getText().getDoubleValue();
    if (! std::isfinite(hz) || hz <= 0.0 || hz > 4294967.0)
        return 0U;
    return static_cast<uint32_t>(std::llround(hz * 1000.0));
}

String NCLPEditor::summaryForPort(size_t portIndex,
                                  const Topology& topology,
                                  bool scanValid) const
{
    const String port = String::charToString(
        static_cast<juce_wchar>('A' + static_cast<int>(portIndex)));
    if (! scanValid || ! topology.valid)
        return port + ": Not scanned";

    const size_t firstIndex = portIndex * 2U;
    const LaneDetection& first = topology.lanes[firstIndex];
    const LaneDetection& second = topology.lanes[firstIndex + 1U];
    const uint32_t total = first.channelCount + second.channelCount;
    const String dash = String::fromUTF8(" \xe2\x80\x94 ");

    if (! first.detected && ! second.detected)
        return port + ": No headstage";
    if (first.detected && second.detected && first.chip == second.chip)
        return port + ": 2" + String::fromUTF8("\xc3\x97") + " " + chipName(first) +
               dash + String(static_cast<int>(total)) + " ch";

    String chips;
    if (first.detected)
        chips = chipName(first);
    if (second.detected)
    {
        if (chips.isNotEmpty())
            chips += " + ";
        chips += chipName(second);
    }
    return port + ": " + chips + dash + String(static_cast<int>(total)) + " ch";
}

String NCLPEditor::connectionStatusText(const SessionSnapshot& snapshot) const
{
    if (! snapshot.lastError.empty())
        return String(snapshot.lastError);
    if (! snapshot.connected)
        return "Disconnected";
    if (snapshot.streaming)
        return "Streaming | " + formatSampleRate(snapshot.config.sampleRateHz);
    if (snapshot.impedanceRunning)
        return "Measuring impedance";
    if (snapshot.initialized)
        return "Ready | " + formatSampleRate(snapshot.config.sampleRateHz);
    if (snapshot.scanValid)
        return "Scanned | Init required";
    return "Rescan required";
}

String NCLPEditor::packetCounterText(const SessionSnapshot& snapshot) const
{
    const ParserStats& stats = snapshot.parserStats;
    const uint64_t queue = snapshot.queueOverruns;
    const uint64_t late = stats.latePackets + stats.duplicatePackets;
    const uint64_t drops = snapshot.kernelDroppedPackets + late + stats.rejectedPackets +
                           queue + snapshot.consumerDroppedFrames;
    return "Lost " + String(static_cast<int64>(stats.lostPackets)) +
           " | Drop " + String(static_cast<int64>(drops));
}

} // namespace nclp
