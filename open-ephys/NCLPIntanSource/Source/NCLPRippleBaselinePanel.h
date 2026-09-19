#pragma once

#include "NCLPDataThread.h"

#include <JuceHeader.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace nclp
{

/**
 * Embedded, read-only ripple baseline analysis panel.
 *
 * The panel starts a private board capture through DataThreadPlugin. It never
 * starts the Open Ephys acquisition graph and never writes the calculated
 * baseline back to the detector profile. The owning ripple-detector page keeps
 * this component alive while the visualizer exists.
 */
class NCLPRippleBaselinePanel final : public juce::Component,
                                      private juce::Timer
{
public:
    explicit NCLPRippleBaselinePanel(
        std::shared_ptr<DataThreadPlugin::AsyncAccess> asyncAccess);
    ~NCLPRippleBaselinePanel() override;

    void resized() override;
    void setDraft(const RippleAnalysisRequest& draft);
    void setSessionSnapshot(const SessionSnapshot& snapshot);
    bool analysisRunning() const noexcept;

private:
    class Content;
    struct Completion;

    void timerCallback() override;
    void startAnalysis();
    void startChannelAnalysis(uint32_t detectedChannel);
    void displayChannelSelected(uint32_t detectedChannel);
    void analysisDraftChanged();
    void cancelAnalysis();
    void finishAnalysisOnMessageThread(bool sourcePresent,
                                       bool succeeded,
                                       std::shared_ptr<Completion> completion);
    void joinFinishedWorker();

    std::shared_ptr<DataThreadPlugin::AsyncAccess> asyncAccess_;
    std::unique_ptr<Content> content_;
    SessionSnapshot snapshot_{};
    std::thread worker_;
    std::shared_ptr<RippleAnalysisCapture> capture_;
    std::vector<std::shared_ptr<RippleAnalysisResult>> cachedResults_;
    uint64_t capturedConnectionGeneration_ = 0U;
    uint64_t capturedResetGeneration_ = 0U;
    uint8_t capturedPhysicalMask_ = 0U;
    uint16_t capturedLogicalMask_ = 0U;
    uint32_t capturedPackedChipIds_ = 0U;
    uint32_t capturedLayoutId_ = 0U;
    size_t capturedChannelCount_ = 0U;
    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> capturing_{false};
    std::atomic<uint64_t> completedSamples_{0};
    std::atomic<uint64_t> targetSamples_{0};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NCLPRippleBaselinePanel);
};

} // namespace nclp
