#include "NCLPControlPage.h"
#include "NCLPImpedanceCanvas.h"
#include "NCLPRippleBaselinePanel.h"

#include <CoreServicesHeader.h>

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nclp
{
namespace
{
// These coordinates intentionally match Acquisition Board's ChannelList.
constexpr int kMinimumContentWidth = 590;
constexpr int kMinimumContentHeight = 200;
constexpr int kColumnWidth = 250;
constexpr int kChannelRowHeight = 22;
constexpr int kChannelRowsTop = 70;
constexpr int kGlobalNamingScheme = 1;
constexpr int kStreamNamingScheme = 2;

uint64_t impedanceKey(size_t physicalIndex, uint32_t localChannel)
{
    return (static_cast<uint64_t>(physicalIndex) << 32U) |
           static_cast<uint64_t>(localChannel);
}

String impedanceValue(const ImpedanceRecord& record)
{
    if (! record.valid)
        return "? Ohm";

    const double magnitudeOhms =
        static_cast<double>(record.magnitudeMilliohms) / 1000.0;
    const double phaseDegrees =
        static_cast<double>(record.phaseMicrodegrees) / 1.0e6;

    // Keep Acquisition Board's display units and precision while correcting
    // its historical 10 kOhm / MOhm threshold typo.
    String magnitude;
    if (magnitudeOhms >= 1.0e6)
        magnitude = String(magnitudeOhms / 1.0e6, 2) + " MOhm";
    else if (magnitudeOhms >= 1.0e3)
        magnitude = String(magnitudeOhms / 1.0e3, 0) + " kOhm";
    else
        magnitude = String(magnitudeOhms, 0) + " Ohm";

    return magnitude + ", " + String(static_cast<int>(phaseDegrees)) + " deg";
}

String impedanceTooltip(const ImpedanceRecord& record)
{
    if (record.valid)
        return record.saturated ? "Valid result; measurement saturated" : "Valid result";
    if (record.timestampError)
        return "Invalid result: timestamp verification failed";
    if (record.verifyError)
        return "Invalid result: measurement verification failed";
    if (record.saturated)
        return "Invalid result: measurement saturated";
    return "Invalid impedance result";
}

bool sameTopology(const Topology& first, const Topology& second)
{
    if (first.valid != second.valid ||
        first.physicalMask != second.physicalMask ||
        first.logicalMask != second.logicalMask ||
        first.layoutId != second.layoutId ||
        first.packedChipIds != second.packedChipIds ||
        first.totalChannels != second.totalChannels ||
        first.headstages.size() != second.headstages.size())
        return false;

    for (size_t index = 0; index < first.headstages.size(); ++index)
    {
        const HeadstageDescriptor& a = first.headstages[index];
        const HeadstageDescriptor& b = second.headstages[index];
        if (a.physicalIndex != b.physicalIndex ||
            a.channelCount != b.channelCount ||
            a.globalChannelBase != b.globalChannelBase ||
            a.chip != b.chip)
            return false;
    }
    return true;
}

String channelName(const HeadstageDescriptor& headstage,
                   uint32_t localChannel,
                   int namingScheme)
{
    if (namingScheme == kStreamNamingScheme)
    {
        return String(laneNameForIndex(headstage.physicalIndex)) + "_CH" +
               String(static_cast<int>(localChannel + 1U));
    }

    return "CH" + String(static_cast<int>(
                      headstage.globalChannelBase + localChannel + 1U));
}

bool hasCompleteImpedanceResults(const SessionSnapshot& snapshot)
{
    if (! snapshot.topology.valid || snapshot.topology.channels.empty() ||
        snapshot.impedanceResults.size() != snapshot.topology.channels.size())
        return false;

    std::unordered_set<uint64_t> expected;
    expected.reserve(snapshot.topology.channels.size());
    for (const ChannelAddress& channel : snapshot.topology.channels)
        expected.insert(impedanceKey(channel.physicalIndex, channel.localChannel));

    if (expected.size() != snapshot.topology.channels.size())
        return false;

    std::unordered_set<uint64_t> seen;
    seen.reserve(snapshot.impedanceResults.size());
    for (const ImpedanceRecord& record : snapshot.impedanceResults)
    {
        const uint64_t key = impedanceKey(record.physicalIndex,
                                          record.localChannel);
        if (expected.find(key) == expected.end() || ! seen.insert(key).second)
            return false;
    }
    return seen.size() == expected.size();
}

String impedanceProgressMessage(const SessionSnapshot& snapshot)
{
    if (snapshot.cancelRequested)
        return "Cancelling impedance measurement...";

    String message = "Measuring impedances";
    const ProgressSnapshot& progress = snapshot.progress;
    if (progress.currentStream >= 0)
        message += " | " + String(laneNameForIndex(
                              static_cast<size_t>(progress.currentStream)));
    if (progress.currentChannel >= 0)
        message += " | Channel " + String(progress.currentChannel + 1);
    if (progress.currentCapRange >= 0)
        message += " | Range " + String(progress.currentCapRange + 1);
    if (progress.totalUnits > 0U)
    {
        message += " | " + String(static_cast<int>(progress.completedUnits)) +
                   "/" + String(static_cast<int>(progress.totalUnits));
    }
    return message;
}
} // namespace

class ImpedanceChannelRow : public Component
{
public:
    ImpedanceChannelRow(const String& channelName_,
                        size_t physicalIndex_,
                        uint32_t localChannel_)
        : physicalIndex(physicalIndex_), localChannel(localChannel_)
    {
        nameLabel = std::make_unique<Label>(channelName_, channelName_);
        nameLabel->setFont(FontOptions("Inter", "Regular", 13.0f));
        nameLabel->setEditable(false);
        addAndMakeVisible(nameLabel.get());

        impedanceLabel = std::make_unique<Label>("Impedance", "? Ohm");
        impedanceLabel->setFont(FontOptions("Fira Code", "Regular", 13.0f));
        impedanceLabel->setEditable(false);
        addAndMakeVisible(impedanceLabel.get());
        refreshColours();
    }

    void resized() override
    {
        nameLabel->setBounds(0, 0, 90, 20);
        impedanceLabel->setBounds(100, 0, 130, 20);
    }

    void lookAndFeelChanged() override
    {
        refreshColours();
    }

    void setImpedance(const ImpedanceRecord* record)
    {
        if (record == nullptr)
        {
            impedanceLabel->setText("? Ohm", dontSendNotification);
            impedanceLabel->setTooltip("Impedance has not been measured");
            impedanceLabel->setColour(Label::textColourId,
                                      findColour(ThemeColours::defaultText));
            return;
        }

        impedanceLabel->setText(impedanceValue(*record), dontSendNotification);
        impedanceLabel->setTooltip(impedanceTooltip(*record));
        impedanceLabel->setColour(Label::textColourId,
                                  findColour(ThemeColours::defaultText));
    }

    size_t physicalIndex;
    uint32_t localChannel;

private:
    void refreshColours()
    {
        if (nameLabel == nullptr || impedanceLabel == nullptr)
            return;
        nameLabel->setColour(
            Label::backgroundColourId,
            findColour(ThemeColours::componentBackground).darker(0.3f));
        nameLabel->setColour(Label::textColourId,
                            findColour(ThemeColours::defaultText));
        impedanceLabel->setColour(Label::textColourId,
                                  findColour(ThemeColours::defaultText));
    }

    std::unique_ptr<Label> nameLabel;
    std::unique_ptr<Label> impedanceLabel;
};

class ImpedanceContent : public Component
{
public:
    void setData(const Topology& topology,
                 const std::vector<ImpedanceRecord>& results,
                 int namingScheme)
    {
        const bool rebuildRequired = ! sameTopology(topology_, topology) ||
                                     namingScheme_ != namingScheme;
        topology_ = topology;
        namingScheme_ = namingScheme;
        if (rebuildRequired)
            rebuildChannels();

        std::unordered_map<uint64_t, const ImpedanceRecord*> resultByChannel;
        for (const ImpedanceRecord& result : results)
            resultByChannel[impedanceKey(result.physicalIndex,
                                         result.localChannel)] = &result;

        for (const std::unique_ptr<ImpedanceChannelRow>& row : channelRows_)
        {
            const auto found = resultByChannel.find(
                impedanceKey(row->physicalIndex, row->localChannel));
            row->setImpedance(found == resultByChannel.end() ? nullptr
                                                              : found->second);
        }
    }

    int requiredWidth() const
    {
        return std::max(kMinimumContentWidth,
                        20 + static_cast<int>(topology_.headstages.size()) *
                                 kColumnWidth);
    }

    int requiredHeight() const
    {
        return kMinimumContentHeight + maxChannels_ * kChannelRowHeight;
    }

    void paint(Graphics& g) override
    {
        g.fillAll(findColour(ThemeColours::componentBackground));
    }

    void lookAndFeelChanged() override
    {
        const Colour textColour = findColour(ThemeColours::defaultText);
        for (const std::unique_ptr<Label>& label : headstageLabels_)
            label->setColour(Label::textColourId, textColour);
        for (const std::unique_ptr<ImpedanceChannelRow>& row : channelRows_)
            row->lookAndFeelChanged();
        repaint();
    }

private:
    void rebuildChannels()
    {
        channelRows_.clear();
        headstageLabels_.clear();
        maxChannels_ = 0;
        if (! topology_.valid)
            return;

        int column = 0;
        for (const HeadstageDescriptor& headstage : topology_.headstages)
        {
            if (headstage.channelCount == 0U)
                continue;

            maxChannels_ = std::max(maxChannels_,
                                    static_cast<int>(headstage.channelCount));
            const int columnX = 10 + column * kColumnWidth;
            const String prefix(laneNameForIndex(headstage.physicalIndex));
            auto heading = std::make_unique<Label>(prefix, prefix);
            heading->setEditable(false);
            heading->setBounds(columnX, 40, kColumnWidth, 25);
            heading->setJustificationType(Justification::centred);
            heading->setColour(Label::textColourId,
                               findColour(ThemeColours::defaultText));
            addAndMakeVisible(heading.get());
            headstageLabels_.push_back(std::move(heading));

            for (uint32_t localChannel = 0;
                 localChannel < headstage.channelCount;
                 ++localChannel)
            {
                auto row = std::make_unique<ImpedanceChannelRow>(
                    channelName(headstage, localChannel, namingScheme_),
                    headstage.physicalIndex,
                    localChannel);
                row->setBounds(columnX,
                               kChannelRowsTop +
                                   static_cast<int>(localChannel) *
                                       kChannelRowHeight,
                               kColumnWidth,
                               kChannelRowHeight);
                addAndMakeVisible(row.get());
                channelRows_.push_back(std::move(row));
            }
            ++column;
        }
    }

    Topology topology_{};
    int namingScheme_ = kGlobalNamingScheme;
    int maxChannels_ = 0;
    std::vector<std::unique_ptr<Label>> headstageLabels_;
    std::vector<std::unique_ptr<ImpedanceChannelRow>> channelRows_;
};

class NCLPImpedanceProgressWindow final : public ThreadWithProgressWindow
{
public:
    NCLPImpedanceProgressWindow(
        NCLPImpedanceCanvas& owner,
        std::shared_ptr<DataThreadPlugin::AsyncAccess> access)
        : ThreadWithProgressWindow("Impedance Measurement", true, true),
          owner_(&owner),
          access_(std::move(access))
    {
        setProgress(-1.0);
        setStatusMessage("Starting impedance measurement...");
    }

    ~NCLPImpedanceProgressWindow() override
    {
        signalThreadShouldExit();
        (void) stopThread(3000);
    }

    void run() override
    {
        if (threadShouldExit())
            return;

        String startStatus;
        const bool started = access_ != nullptr && access_->run(
            [&](DataThreadPlugin& backend)
            {
                startStatus = backend.runImpedance();
                finalSnapshot_ = backend.sessionSnapshot();
            });

        if (! started)
        {
            finalStatus_ = "Could not start impedance measurement: source was removed";
            setStatusMessage(finalStatus_);
            return;
        }

        if (! finalSnapshot_.impedanceRunning)
        {
            finalStatus_ = ! finalSnapshot_.lastError.empty()
                               ? String(finalSnapshot_.lastError)
                               : startStatus;
            setStatusMessage(finalStatus_);
            return;
        }

        while (! threadShouldExit())
        {
            const bool read = access_->run([&](DataThreadPlugin& backend)
            {
                finalSnapshot_ = backend.sessionSnapshot();
            });
            if (! read)
            {
                finalStatus_ = "Impedance measurement source was removed";
                setStatusMessage(finalStatus_);
                return;
            }

            const ProgressSnapshot& progressSnapshot = finalSnapshot_.progress;
            setProgress(progressSnapshot.totalUnits > 0U
                            ? jlimit(0.0,
                                     1.0,
                                     static_cast<double>(progressSnapshot.completedUnits) /
                                         static_cast<double>(progressSnapshot.totalUnits))
                            : -1.0);
            setStatusMessage(impedanceProgressMessage(finalSnapshot_));

            if (! finalSnapshot_.impedanceRunning)
                break;
            Thread::sleep(100);
        }

        if (threadShouldExit())
        {
            String cancelStatus;
            if (access_ != nullptr)
            {
                (void) access_->run([&](DataThreadPlugin& backend)
                {
                    const SessionSnapshot beforeCancel = backend.sessionSnapshot();
                    if (beforeCancel.impedanceRunning &&
                        ! beforeCancel.cancelRequested)
                        cancelStatus = backend.cancelImpedance();
                    finalSnapshot_ = backend.sessionSnapshot();
                });
            }
            finalStatus_ = cancelStatus.isNotEmpty()
                               ? cancelStatus
                               : "Impedance cancellation requested";
            return;
        }

        setProgress(1.0);
        if (! finalSnapshot_.lastError.empty())
            finalStatus_ = String(finalSnapshot_.lastError);
        else if (hasCompleteImpedanceResults(finalSnapshot_))
            finalStatus_ = "Impedance measurement complete";
        else
            finalStatus_ = "Impedance measurement ended without a complete result set";
        setStatusMessage(finalStatus_);
    }

private:
    void threadComplete(bool userPressedCancel) override
    {
        Component::SafePointer<NCLPImpedanceCanvas> safeOwner(owner_);
        const SessionSnapshot snapshot = finalSnapshot_;
        const String status = finalStatus_;
        if (safeOwner != nullptr)
            safeOwner->impedanceProgressFinished(snapshot,
                                                 status,
                                                 userPressedCancel);
        // The owner callback is allowed to delete this object. Do not access
        // members after this point.
    }

    Component::SafePointer<NCLPImpedanceCanvas> owner_;
    std::shared_ptr<DataThreadPlugin::AsyncAccess> access_;
    SessionSnapshot finalSnapshot_{};
    String finalStatus_;
};

NCLPImpedanceCanvas::NCLPImpedanceCanvas(
    std::shared_ptr<DataThreadPlugin::AsyncAccess> asyncAccess)
    : asyncAccess_(std::move(asyncAccess))
{
    content = std::make_unique<ImpedanceContent>();
    algorithmContent = std::make_unique<NCLPControlPage>(NCLPControlPage::Kind::Algorithm);
    outputContent = std::make_unique<NCLPControlPage>(NCLPControlPage::Kind::Output);
    rippleBaselinePanel = std::make_unique<NCLPRippleBaselinePanel>(asyncAccess_);
    algorithmContent->setBaselineAnalysisComponent(rippleBaselinePanel.get());
    for (auto* page : {algorithmContent.get(), outputContent.get()})
    {
        page->onCommand = [this, page](Command command, CommandArgs args) {
            performOutputCommand(page, command, args, false);
        };
        page->onRefresh = [this, page] {
            performOutputCommand(page, Command::RippleStatus, {}, true);
        };
        page->onPollStatus = [this] { pollOutputStatus(); };
    }

    numberingSchemeLabel =
        std::make_unique<Label>("Channel Names:", "Channel Names:");
    numberingSchemeLabel->setFont(FontOptions("Inter", "Semi Bold", 15.0f));
    numberingSchemeLabel->setEditable(false);
    numberingSchemeLabel->setBounds(10, 10, 150, 25);
    content->addAndMakeVisible(numberingSchemeLabel.get());

    numberingScheme = std::make_unique<ComboBox>("numberingScheme");
    numberingScheme->addItem("Global", kGlobalNamingScheme);
    numberingScheme->addItem("Stream-Based", kStreamNamingScheme);
    numberingScheme->setBounds(125, 10, 140, 25);
    numberingScheme->addListener(this);
    content->addAndMakeVisible(numberingScheme.get());

    measureButton = std::make_unique<UtilityButton>("Measure Impedances");
    measureButton->setRadius(3.0f);
    measureButton->setBounds(280, 10, 145, 25);
    measureButton->setFont(FontOptions(14.0f));
    measureButton->setTooltip("Measure every detected amplifier channel");
    measureButton->addListener(this);
    content->addAndMakeVisible(measureButton.get());

    saveButton = std::make_unique<UtilityButton>("Save Impedances");
    saveButton->setRadius(3.0f);
    saveButton->setBounds(430, 10, 145, 25);
    saveButton->setFont(FontOptions(14.0f));
    saveButton->setTooltip("Save the latest complete impedance results");
    saveButton->addListener(this);
    content->addAndMakeVisible(saveButton.get());

    viewport = std::make_unique<Viewport>();
    viewport->setViewedComponent(content.get(), false);
    viewport->setScrollBarsShown(true, true);
    viewport->setScrollBarThickness(10);
    addAndMakeVisible(viewport.get());

    if (asyncAccess_ != nullptr)
    {
        // Canvas creation runs on the JUCE message thread. If SCAN or another
        // command currently owns the backend, use the editor's cached snapshot
        // instead of freezing page creation until that command completes.
        (void) asyncAccess_->tryRun([&](DataThreadPlugin& backend)
        {
            const int backendScheme = backend.channelNamingScheme();
            if (backendScheme == kGlobalNamingScheme ||
                backendScheme == kStreamNamingScheme)
                namingScheme_ = backendScheme;
            latestSnapshot_ = backend.sessionSnapshot();
        });
    }

    suppressNamingCallback_ = true;
    numberingScheme->setSelectedId(namingScheme_, dontSendNotification);
    suppressNamingCallback_ = false;
    lookAndFeelChanged();
    updateSnapshot(latestSnapshot_);
}

NCLPImpedanceCanvas::~NCLPImpedanceCanvas()
{
    // Cancels and joins private capture before the backend lifetime gate can be
    // released by SourceNode teardown.
    if (algorithmContent != nullptr)
        algorithmContent->setBaselineAnalysisComponent(nullptr);
    rippleBaselinePanel.reset();
    // Signals the modal worker before ThreadWithProgressWindow's bounded join.
    // Its loop reacts within 100 ms and requests backend cancellation through
    // the lifetime-gated access object if a measurement is still active.
    impedanceProgressWindow.reset();
}

void NCLPImpedanceCanvas::paint(Graphics& g)
{
    g.fillAll(findColour(ThemeColours::componentBackground));
}

void NCLPImpedanceCanvas::resized()
{
    if (viewport == nullptr)
        return;

    viewport->setBounds(0, 0, getWidth(), getHeight());
    const int viewportWidth =
        std::max(0, viewport->getWidth() - viewport->getScrollBarThickness());
    // A Viewport implements scrolling by moving its viewed component to a
    // negative origin.  Snapshot refreshes call resized(), so resetting that
    // origin with setBounds(0, 0, ...) would snap the view back to the top.
    // Change only the content extent and leave its viewport-owned position
    // intact.
    Component* const activeContent = activePageComponent();
    if (activeContent == nullptr)
        return;

    const int requiredWidth = activePage_ == NCLPCanvasPage::Impedance
                                  ? content->requiredWidth()
                                  : 800;
    const int requiredHeight = activePage_ == NCLPCanvasPage::Impedance
                                   ? content->requiredHeight()
                                   : (activePage_ == NCLPCanvasPage::Algorithm ? algorithmContent->requiredHeight() : outputContent->requiredHeight());
    activeContent->setSize(std::max(viewportWidth, requiredWidth),
                           std::max(viewport->getHeight(), requiredHeight));
}

void NCLPImpedanceCanvas::lookAndFeelChanged()
{
    if (numberingSchemeLabel != nullptr)
        numberingSchemeLabel->setColour(Label::textColourId,
                                        findColour(ThemeColours::defaultText));
    if (content != nullptr)
        content->lookAndFeelChanged();
    if (algorithmContent != nullptr)
        algorithmContent->lookAndFeelChanged();
    if (outputContent != nullptr)
        outputContent->lookAndFeelChanged();
    repaint();
}

void NCLPImpedanceCanvas::refreshState()
{
    if (asyncAccess_ != nullptr)
    {
        SessionSnapshot snapshot = latestSnapshot_;
        int scheme = namingScheme_;
        // Tab selection is synchronous on the JUCE message thread. A missed
        // refresh is harmless because editor snapshots continue to arrive;
        // waiting behind a long hardware command would freeze navigation.
        if (asyncAccess_->tryRun([&](DataThreadPlugin& backend)
            {
                snapshot = backend.sessionSnapshot();
                scheme = backend.channelNamingScheme();
            }))
        {
            if (scheme == kGlobalNamingScheme || scheme == kStreamNamingScheme)
                setNamingScheme(scheme);
            updateSnapshot(snapshot);
        }
    }
    resized();
}

void NCLPImpedanceCanvas::updateSettings()
{
    refreshState();
}

void NCLPImpedanceCanvas::refresh()
{
    repaint();
    if (Component* const activeContent = activePageComponent())
        activeContent->repaint();
}

void NCLPImpedanceCanvas::beginAnimation()
{
    acquisitionActive = true;
    refreshControls();
}

void NCLPImpedanceCanvas::endAnimation()
{
    acquisitionActive = false;
    refreshControls();
}

void NCLPImpedanceCanvas::buttonClicked(Button* button)
{
    if (button == measureButton.get())
        startImpedanceMeasurement();
    else if (button == saveButton.get())
        saveResults();
}

void NCLPImpedanceCanvas::comboBoxChanged(ComboBox* comboBox)
{
    if (suppressNamingCallback_ || comboBox != numberingScheme.get() ||
        asyncAccess_ == nullptr)
        return;

    const int requestedScheme = numberingScheme->getSelectedId();
    if (requestedScheme != kGlobalNamingScheme &&
        requestedScheme != kStreamNamingScheme)
    {
        setNamingScheme(namingScheme_);
        return;
    }

    String status;
    int acceptedScheme = namingScheme_;
    const bool invoked = asyncAccess_->run([&](DataThreadPlugin& backend)
    {
        status = backend.setChannelNamingScheme(requestedScheme);
        acceptedScheme = backend.channelNamingScheme();
    });
    if (! invoked)
    {
        setNamingScheme(namingScheme_);
        CoreServices::sendStatusMessage(
            "Could not change channel names: source was removed");
        return;
    }

    if (acceptedScheme == kGlobalNamingScheme ||
        acceptedScheme == kStreamNamingScheme)
        setNamingScheme(acceptedScheme);
    else
        setNamingScheme(namingScheme_);

    if (status.isNotEmpty())
        CoreServices::sendStatusMessage(status);
}

void NCLPImpedanceCanvas::performOutputCommand(NCLPControlPage* page,
    Command command, CommandArgs args, bool query)
{
    if (outputCommandPending_ || asyncAccess_ == nullptr || !latestSnapshot_.connected)
        return;
    if (query) outputReadRequested_ = false;
    ++outputCommandGeneration_;
    outputCommandPending_ = true;
    updateSnapshot(latestSnapshot_);
    Component::SafePointer<NCLPImpedanceCanvas> owner(this);
    auto access = asyncAccess_;
    std::thread([owner, access, page, command, args, query] {
        SessionSnapshot snapshot;
        String error;
        const bool present = access->run([&](DataThreadPlugin& backend) {
            error = query ? backend.readOutputs() : backend.sendOutputCommand(command, args);
            snapshot = backend.sessionSnapshot();
        });
        if (!present) error = "Source was removed";
        MessageManager::callAsync([owner, page, snapshot, error, query] {
            if (owner == nullptr) return;
            owner->outputCommandPending_ = false;
            if (!query || error.isNotEmpty()) page->commandFinished(error);
            owner->updateSnapshot(snapshot, true);
        });
    }).detach();
}

void NCLPImpedanceCanvas::pollOutputStatus()
{
    if (outputStatusPending_ || outputCommandPending_ || asyncAccess_ == nullptr ||
        !latestSnapshot_.connected || latestSnapshot_.commandBusy)
        return;
    outputStatusPending_ = true;
    const auto generation = outputCommandGeneration_;
    Component::SafePointer<NCLPImpedanceCanvas> owner(this);
    auto access = asyncAccess_;
    std::thread([owner, access, generation] {
        SessionSnapshot snapshot;
        const bool present = access->tryRun([&](DataThreadPlugin& backend) {
            backend.pollOutputStatus();
            snapshot = backend.sessionSnapshot();
        });
        MessageManager::callAsync([owner, snapshot, present, generation] {
            if (owner == nullptr) return;
            owner->outputStatusPending_ = false;
            // A later user command/readback always wins over this poll.
            if (present && !owner->outputCommandPending_ &&
                generation == owner->outputCommandGeneration_)
                owner->updateSnapshot(snapshot);
        });
    }).detach();
}

void NCLPImpedanceCanvas::updateSnapshot(const SessionSnapshot& snapshot, bool refreshOutputSettings)
{
    const bool newConnection = snapshot.connected &&
        (!latestSnapshot_.connected || snapshot.connectionGeneration != latestSnapshot_.connectionGeneration);
    const bool resetChanged = snapshot.resetGeneration != latestSnapshot_.resetGeneration;
    if (snapshot.connected != latestSnapshot_.connected || newConnection || resetChanged)
        ++outputCommandGeneration_;
    if (newConnection)
        outputReadRequested_ = true;
    latestSnapshot_ = snapshot;
    if (rippleBaselinePanel != nullptr)
        rippleBaselinePanel->setSessionSnapshot(snapshot);
    algorithmContent->setState(snapshot, outputCommandPending_, refreshOutputSettings);
    outputContent->setState(snapshot, outputCommandPending_, refreshOutputSettings);
    content->setData(snapshot.topology, snapshot.impedanceResults, namingScheme_);
    refreshControls();
    resized();
    // Request settings once on page opening/reconnection. If another operation
    // owns the backend, defer the read; the page timer handles failure recovery.
    if (outputReadRequested_ && activePage_ != NCLPCanvasPage::Impedance &&
        snapshot.connected && !snapshot.commandBusy && !outputCommandPending_)
        performOutputCommand(activePage_ == NCLPCanvasPage::Algorithm
                                 ? algorithmContent.get() : outputContent.get(),
                             Command::RippleStatus, {}, true);
}

void NCLPImpedanceCanvas::setPage(NCLPCanvasPage newPage)
{
    switch (newPage)
    {
        case NCLPCanvasPage::Impedance:
        case NCLPCanvasPage::Algorithm:
        case NCLPCanvasPage::Output:
            break;
        default:
            return;
    }

    if (newPage == activePage_ || viewport == nullptr)
        return;

    if (activePage_ == NCLPCanvasPage::Impedance)
        impedanceViewPosition_ = viewport->getViewPosition();

    activePage_ = newPage;
    viewport->setViewedComponent(activePageComponent(), false);
    resized();
    viewport->setViewPosition(activePage_ == NCLPCanvasPage::Impedance
                                  ? impedanceViewPosition_
                                  : Point<int>());
    if (activePage_ != NCLPCanvasPage::Impedance)
    {
        outputReadRequested_ = true;
        updateSnapshot(latestSnapshot_);
    }
    repaint();
}

NCLPCanvasPage NCLPImpedanceCanvas::page() const noexcept
{
    return activePage_;
}

Component* NCLPImpedanceCanvas::activePageComponent() const noexcept
{
    switch (activePage_)
    {
        case NCLPCanvasPage::Impedance:
            return content.get();
        case NCLPCanvasPage::Algorithm:
            return algorithmContent.get();
        case NCLPCanvasPage::Output:
            return outputContent.get();
        default:
            return content.get();
    }
}

void NCLPImpedanceCanvas::refreshControls()
{
    const bool streaming = acquisitionActive || latestSnapshot_.streaming;
    const bool hasTopology = latestSnapshot_.scanValid &&
                             latestSnapshot_.topology.valid &&
                             ! latestSnapshot_.topology.headstages.empty();
    const bool operationLocked = latestSnapshot_.commandBusy ||
                                 latestSnapshot_.impedanceRunning ||
                                 latestSnapshot_.cancelRequested ||
                                 impedanceProgressWindow != nullptr;

    // Keep the idle page identical to Acquisition Board; measurement status
    // and cancellation live only in ThreadWithProgressWindow.
    measureButton->setLabel("Measure Impedances");
    measureButton->setTooltip("Measure every detected amplifier channel");
    measureButton->setEnabledState(latestSnapshot_.connected && hasTopology &&
                                   ! streaming && ! operationLocked);
    saveButton->setEnabledState(hasCompleteImpedanceResults(latestSnapshot_) &&
                                ! streaming && ! operationLocked);
    numberingScheme->setEnabled(latestSnapshot_.connected && hasTopology &&
                                ! streaming && ! operationLocked);
}

void NCLPImpedanceCanvas::startImpedanceMeasurement()
{
    const bool hasTopology = latestSnapshot_.scanValid &&
                             latestSnapshot_.topology.valid &&
                             ! latestSnapshot_.topology.headstages.empty();
    if (asyncAccess_ == nullptr || impedanceProgressWindow != nullptr ||
        acquisitionActive || latestSnapshot_.streaming ||
        latestSnapshot_.commandBusy || latestSnapshot_.impedanceRunning ||
        latestSnapshot_.cancelRequested || ! latestSnapshot_.connected ||
        ! hasTopology)
        return;

    impedanceProgressWindow =
        std::make_unique<NCLPImpedanceProgressWindow>(*this, asyncAccess_);
    refreshControls();
    impedanceProgressWindow->launchThread();
}

void NCLPImpedanceCanvas::impedanceProgressFinished(
    const SessionSnapshot& snapshot,
    const String& status,
    bool userPressedCancel)
{
    updateSnapshot(snapshot);
    const String finalStatus = status.isNotEmpty()
                                   ? status
                                   : (userPressedCancel
                                          ? "Impedance cancellation requested"
                                          : "Impedance measurement finished");
    CoreServices::sendStatusMessage(finalStatus);

    // ThreadWithProgressWindow explicitly permits deletion from
    // threadComplete(). This is intentionally the last use of the object.
    impedanceProgressWindow.reset();
    refreshControls();
}

void NCLPImpedanceCanvas::saveResults()
{
    const std::shared_ptr<DataThreadPlugin::AsyncAccess> access = asyncAccess_;
    const File defaultFile =
        File::getSpecialLocation(File::userDocumentsDirectory)
            .getChildFile("nclp_impedances.xml");
    saveChooser = std::make_unique<FileChooser>(
        "Save NCLP impedance results", defaultFile, "*.xml");

    Component::SafePointer<NCLPImpedanceCanvas> safeThis(this);
    constexpr int chooserFlags = FileBrowserComponent::saveMode |
                                 FileBrowserComponent::canSelectFiles |
                                 FileBrowserComponent::warnAboutOverwriting;
    saveChooser->launchAsync(chooserFlags, [safeThis, access](const FileChooser& chooser)
    {
        if (safeThis == nullptr)
            return;
        File output = chooser.getResult();
        if (output == File())
            return;
        if (! output.hasFileExtension("xml"))
            output = output.withFileExtension("xml");

        String error;
        bool saved = false;
        if (access == nullptr || ! access->run([&](DataThreadPlugin& backend)
            {
                const SessionSnapshot snapshot = backend.sessionSnapshot();
                if (! hasCompleteImpedanceResults(snapshot))
                    error = "Impedance result set is incomplete";
                else
                    saved = backend.saveImpedances(output, &error);
            }))
        {
            CoreServices::sendStatusMessage(
                "Could not save impedances: source was removed");
            return;
        }

        if (error.isNotEmpty())
            CoreServices::sendStatusMessage("Could not save impedances: " + error);
        else if (saved)
            CoreServices::sendStatusMessage(
                "Saved impedances to " + output.getFullPathName());
        else
            CoreServices::sendStatusMessage("Could not save impedances");
    });
}

void NCLPImpedanceCanvas::setNamingScheme(int scheme)
{
    if (scheme != kGlobalNamingScheme && scheme != kStreamNamingScheme)
        return;

    namingScheme_ = scheme;
    suppressNamingCallback_ = true;
    numberingScheme->setSelectedId(namingScheme_, dontSendNotification);
    suppressNamingCallback_ = false;
    content->setData(latestSnapshot_.topology,
                     latestSnapshot_.impedanceResults,
                     namingScheme_);
    resized();
}

} // namespace nclp
