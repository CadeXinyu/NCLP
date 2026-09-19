#pragma once
#include "NCLPSession.h"
#include <JuceHeader.h>

namespace nclp
{
// Native JUCE controls shared by the live plugin and the screenshot harness.
// The owner supplies asynchronous transport; this component never opens sockets.
class NCLPControlPage final : public juce::Component, private juce::Timer
{
public:
    enum class Kind { Algorithm, Output };
    explicit NCLPControlPage(Kind kind);
    void paint(juce::Graphics&) override;
    void resized() override;
    void setState(const SessionSnapshot& state, bool pending, bool refreshSettings = false);
    void commandFinished(const juce::String& error);
    void setBaselineAnalysisComponent(juce::Component* component);
    int requiredHeight() const
    {
        return kind_ == Kind::Algorithm
            ? (baselineAnalysisComponent_ != nullptr ? 1510 : 820)
            : 922;
    }
    std::function<void(Command, CommandArgs)> onCommand;
    std::function<void()> onRefresh;
    std::function<void()> onPollStatus;

private:
    void timerCallback() override;
    void configureNumber(juce::TextEditor&, const juce::String& initial);
    void configureChoice(juce::ComboBox&, std::initializer_list<const char*> choices);
    void configureButton(juce::TextButton&, const juce::String&, std::function<void()> action);
    bool send(Command, CommandArgs = {});
    bool number(const juce::TextEditor&, uint32_t& value, uint32_t minimum, uint32_t maximum, const char* label);
    bool buildRippleProfileArgs(CommandArgs&, bool useAppliedThreshold);
    void applyRippleK();
    void detectorToggleClicked();
    void requestDetectorEnabled();
    void requestDetectorDisabled();
    void continueDetectorAction();
    bool detectorProfileMatchesReadback() const;
    juce::String detectorEnableDisabledReason() const;
    bool fp32Number(const juce::TextEditor&, uint32_t&, bool nonnegative, const char*);
    bool rippleMicrovolts(const juce::TextEditor&, uint32_t&, bool nonnegative, const char*);
    void applySync();
    bool buildRoutingArgs(CommandArgs&);
    bool applyRouting();
    void requestArm();
    void requestTest();
    void requestDisarm();
    void cancelQueuedOutputAction();
    void continueQueuedDisarmAction();
    void continueQueuedOutputAction();
    juce::String routingDraftDisabledReason() const;
    void applyRecordedTtl();
    void loadPreset();
    bool scaledNumber(const juce::TextEditor&, double scale, uint32_t& value,
                      uint32_t minimum, uint32_t maximum, const char* label);
    double actualDacRate() const;
    juce::String waveformGuidance() const;
    juce::String waveformDuration() const;
    juce::String armDisabledReason() const;
    void showError(const juce::String&);
    void field(const juce::String&, juce::Component&, int x, int y, int width);
    void card(juce::Graphics&, juce::Rectangle<int>, const juce::String&);
    void updateEnabled();
    void markDirty(juce::Component* field);
    void resetPresetEditors();

    Kind kind_;
    SessionSnapshot state_{};
    bool pending_ = false, filling_ = false;
    bool settingsRefreshPending_ = true;
    unsigned dirty_ = 0, appliedSection_ = 0;
    enum class OutputGoal { None, Arm, Trigger };
    enum class OutputFollowUp { None, Arm, Trigger };
    OutputGoal outputGoal_ = OutputGoal::None;
    OutputFollowUp outputFollowUp_ = OutputFollowUp::None;
    bool disarmRequested_ = false;
    enum class DetectorAction {
        None,
        ApplyingProfile,
        WaitingForProfileReadback,
        Starting,
        WaitingForEnabled,
        Stopping,
        WaitingForStopped
    };
    DetectorAction detectorAction_ = DetectorAction::None;
    CommandArgs detectorProfileArgs_{};
    bool detectorDesiredEnabled_ = false;
    Command lastSentCommand_ = Command::Ping;
    uint32_t lastSentAction_ = 0U;
    bool hasLastSentCommand_ = false;
    juce::String feedback_;
    bool error_ = false;
    struct FieldLabel { juce::String text; juce::Rectangle<int> bounds; };
    std::vector<FieldLabel> labels_;
    juce::TextEditor detectedChannel_, firTaps_, lowHz_, highHz_, windowMs_, refractoryMs_;
    juce::TextEditor muUv_, sigmaUv_, k_;
    juce::TextButton applyRippleK_, refresh_;
    juce::ToggleButton detectorEnabled_;
    juce::Component* baselineAnalysisComponent_ = nullptr;
    juce::ComboBox filterKind_, syncMode_, actionMode_, ttl0_, ttl1_, shape_, channels_, marker_, updateRate_;
    juce::Label duration_;
    juce::TextEditor syncPeriod_, syncHigh_, ttlDuration_, parameter_, minimumA_, maximumA_, minimumB_, maximumB_, repeats_;
    juce::TextButton applySync_, applyRouting_, applyMarker_, loadPreset_, arm_, disarm_, trigger_, clear_;
};
}
