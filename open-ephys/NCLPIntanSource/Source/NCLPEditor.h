#pragma once

#include "NCLPDataThread.h"

#include <VisualizerEditorHeaders.h>

#include <array>
#include <atomic>
#include <functional>

namespace nclp
{

class NCLPImpedanceCanvas;
enum class NCLPCanvasPage : uint8_t;

/**
 * Acquisition Board-style headstage row for one physical port.
 *
 * The first two read-only cells show the detected channel counts. The two
 * matching cells on their right show the effective selected cable-delay phase
 * for physical lanes 1 and 2 of the port.
 */
class PortSummaryComponent : public Component,
                             public SettableTooltipClient
{
public:
    explicit PortSummaryComponent(const String& portName);

    void paint(Graphics& g) override;
    void setLanes(const LaneDetection& first,
                  const LaneDetection& second,
                  const String& fullSummary,
                  bool scanValid);

private:
    void updateCell(UtilityButton& cell,
                    const LaneDetection& lane,
                    const String& laneName,
                    bool scanValid);
    void updatePhaseCell(UtilityButton& cell,
                         const LaneDetection& lane,
                         const String& laneName,
                         bool scanValid);
    String portName_;
    bool active_ = false;
    std::unique_ptr<UtilityButton> firstCell_;
    std::unique_ptr<UtilityButton> secondCell_;
    std::unique_ptr<UtilityButton> firstPhaseCell_;
    std::unique_ptr<UtilityButton> secondPhaseCell_;
};

/** Faithful local equivalent of the reference SampleRateInterface. */
class SampleRateInterface : public Component,
                            private ComboBox::Listener
{
public:
    using ChangeCallback = std::function<void(uint32_t)>;

    explicit SampleRateInterface(ChangeCallback onChange);

    void paint(Graphics& g) override;
    void setSelectedRate(uint32_t sampleRateHz);
    void setControlEnabled(bool enabled);

private:
    void comboBoxChanged(ComboBox* comboBox) override;

    ChangeCallback onChange_;
    std::unique_ptr<ComboBox> rateSelection_;
    bool suppressCallback_ = false;
};

/** Faithful local equivalent of the reference inline BandwidthInterface. */
class BandwidthInterface : public Component,
                           private Label::Listener
{
public:
    using ChangeCallback = std::function<void(double, double)>;

    explicit BandwidthInterface(ChangeCallback onChange);

    void paint(Graphics& g) override;
    void lookAndFeelChanged() override;
    void setValues(uint32_t lowerMilliHz, uint32_t upperHz);
    void setControlEnabled(bool enabled);

private:
    void labelTextChanged(Label* label) override;

    ChangeCallback onChange_;
    std::unique_ptr<Label> lowerSelection_;
    std::unique_ptr<Label> upperSelection_;
    double lowerHz_ = 0.1;
    double upperHz_ = 7500.0;
    bool suppressCallback_ = false;
};

/** Compact board controls plus a persistent multi-page NCLP Visualizer. */
class NCLPEditor : public VisualizerEditor,
                   public Button::Listener,
                   public ComboBox::Listener,
                   public Label::Listener,
                   private Timer
{
public:
    NCLPEditor(GenericProcessor* parentNode, DataThreadPlugin* node);
    ~NCLPEditor() override;

    void buttonClicked(Button* button) override;
    void comboBoxChanged(ComboBox* comboBox) override;
    void labelTextChanged(Label* label) override;

    void startAcquisition() override;
    void stopAcquisition() override;
    Visualizer* createNewCanvas() override;
    void saveVisualizerEditorParameters(XmlElement* xml) override;
    void loadVisualizerEditorParameters(XmlElement* xml) override;

    /** Paint the classic Acquisition Board editor body. */
    void paint(Graphics& g) override;
    void lookAndFeelChanged() override;

    void setStatusText(const String& text, bool broadcast = true);
    void updateSnapshot(const SessionSnapshot& snapshot,
                        bool forceConfigRefresh = false);

private:
    using AsyncWork = std::function<void(DataThreadPlugin&)>;
    using ScanAsyncWork = std::function<void(
        DataThreadPlugin&,
        const DataThreadPlugin::ScanProgressCallback&)>;

    void timerCallback() override;
    void refreshControls();
    void updateConfigControls(const AcquisitionConfig& config);
    void updatePortSummaries(const Topology& topology, bool scanValid);
    void updateScanProgress(const ProgressSnapshot& progress);
    void showCanvasPage(NCLPCanvasPage page);
    void updateCanvasPageButtons();
    void sampleRateChanged(uint32_t sampleRateHz);
    void bandwidthChanged(double lowerHz, double upperHz);
    void runAsyncCommand(const String& busyText,
                         AsyncWork work,
                         bool updateSignalChain = false);
    void runAsyncScanCommand(const String& busyText,
                             ScanAsyncWork work,
                             bool updateSignalChain = false);
    void runAsyncCommandImpl(const String& busyText,
                             ScanAsyncWork work,
                             bool reportsScanProgress,
                             bool updateSignalChain);

    uint32_t requestedDspMilliHz() const;
    String summaryForPort(size_t portIndex,
                          const Topology& topology,
                          bool scanValid) const;
    String connectionStatusText(const SessionSnapshot& snapshot) const;
    String packetCounterText(const SessionSnapshot& snapshot) const;

    std::shared_ptr<DataThreadPlugin::AsyncAccess> asyncAccess_;
    NCLPImpedanceCanvas* impedanceCanvas = nullptr;
    std::atomic<bool> localCommandInFlight{ false };
    bool suppressConfigCallbacks = false;
    bool configControlsInitialized_ = false;
    SessionSnapshot latestSnapshot_{};
    NCLPCanvasPage selectedPage_;

    std::array<std::unique_ptr<PortSummaryComponent>, kPortCount> portSummaries;

    std::unique_ptr<UtilityButton> connectButton;
    std::unique_ptr<UtilityButton> checkButton;
    std::unique_ptr<UtilityButton> rescanButton;
    std::unique_ptr<UtilityButton> resetButton;
    std::unique_ptr<UtilityButton> auxButton;
    std::unique_ptr<UtilityButton> dspButton;
    std::unique_ptr<UtilityButton> algoButton;
    std::unique_ptr<UtilityButton> outButton;
    std::unique_ptr<UtilityButton> impedanceButton;

    std::unique_ptr<ComboBox> ttlFastSettleCombo;

    std::unique_ptr<SampleRateInterface> sampleRateInterface;
    std::unique_ptr<BandwidthInterface> bandwidthInterface;

    std::unique_ptr<Label> dspRequestedTitle;
    std::unique_ptr<Label> dspActualTitle;
    std::unique_ptr<Label> auxTitleLabel;
    std::unique_ptr<Label> dspRequestedValue;
    std::unique_ptr<Label> dspActualValue;
    std::unique_ptr<Label> ttlFastSettleTitle;
    std::unique_ptr<Label> statusLabel;
    std::unique_ptr<Label> packetCountersLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NCLPEditor);
};

} // namespace nclp
