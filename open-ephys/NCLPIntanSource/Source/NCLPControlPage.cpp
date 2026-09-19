#include "NCLPControlPage.h"
#include <charconv>
#include <cmath>
#include <limits>

namespace nclp
{
using namespace juce;
namespace
{
const Colour background(0xff161e28), panel(0xff202b38), textColour(0xffe6edf5), muted(0xffa7b6c7), accent(0xffa1dfbb);
constexpr uint32_t maximum = std::numeric_limits<uint32_t>::max();
constexpr uint32_t rates[] = {2000, 2500, 5000, 10000, 20000, 30000};
String hex(uint32_t value) { return "0x" + String::toHexString(value).toUpperCase(); }
String decimal(double value, int places = 6)
{
    if (value == 0) return "0";
    return String(value, places).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
}
String fp32Text(uint32_t bits)
{
    char buffer[48];
    const auto parsed = std::to_chars(buffer, buffer + sizeof buffer, decodeFp32(bits),
                                     std::chars_format::general, std::numeric_limits<float>::max_digits10);
    return parsed.ec == std::errc{} ? String(std::string(buffer, parsed.ptr)) : String("0");
}
bool readDecimal(const TextEditor& editor, double& value)
{
    const auto input = editor.getText().trim().toStdString();
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value, std::chars_format::fixed);
    return !input.empty() && parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() && std::isfinite(value) && value >= 0;
}
bool detectedIndexFromReadback(const Topology& topology,
                               uint32_t detectedChannelIndex,
                               size_t& detectedIndex)
{
    if (!topology.valid || detectedChannelIndex >= topology.channels.size())
        return false;
    detectedIndex = static_cast<size_t>(detectedChannelIndex);
    return true;
}
bool readDetectedIndex(const TextEditor& editor, size_t& detectedIndex)
{
    const auto input = editor.getText().trim().toStdString();
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), detectedIndex);
    return !input.empty() && parsed.ec == std::errc{} &&
           parsed.ptr == input.data() + input.size();
}
}

NCLPControlPage::NCLPControlPage(Kind kind) : kind_(kind)
{
    configureButton(refresh_, "Refresh", [this] { if (onRefresh) onRefresh(); });
    if (kind_ == Kind::Algorithm)
    {
        configureChoice(filterKind_, {"Minimum-phase FIR", "Linear-phase FIR", "Fixed Butterworth IIR"});
        configureNumber(detectedChannel_, "0"); configureNumber(firTaps_, "129");
        configureNumber(lowHz_, "150"); configureNumber(highHz_, "250");
        configureNumber(windowMs_, "4"); configureNumber(refractoryMs_, "1000");
        configureNumber(muUv_, "0"); configureNumber(sigmaUv_, "0.195");
        configureNumber(k_, "4");
        for (auto* editor : {&detectedChannel_, &firTaps_, &refractoryMs_})
            editor->setInputRestrictions(10, "0123456789");
        for (auto* editor : {&lowHz_, &highHz_, &windowMs_}) editor->setInputRestrictions(20, "0123456789.");
        for (auto* editor : {&muUv_, &sigmaUv_, &k_})
            editor->setInputRestrictions(32, "0123456789.+-eE");
        configureButton(applyRippleK_, "Apply K live", [this] { applyRippleK(); });
        addAndMakeVisible(detectorEnabled_);
        detectorEnabled_.setButtonText("Detector disabled");
        detectorEnabled_.setColour(ToggleButton::textColourId, textColour);
        detectorEnabled_.onClick = [this] { detectorToggleClicked(); };
        windowMs_.setTooltip("PWT window in milliseconds, rounded up to 3 kS/s samples. Maximum 10 ms (30 samples).");
        refractoryMs_.setTooltip("Reject subsequent detections after an accepted trigger. 0 disables refractory.");
        muUv_.setTooltip("Ripple-band amplitude mean in microvolts. Detector enable converts it to counts at 0.195 uV/count.");
        sigmaUv_.setTooltip("Ripple-band amplitude standard deviation in microvolts. Must be finite and nonnegative.");
        k_.setTooltip("Threshold multiplier: mu + K*sigma. Apply K live updates between samples without resetting histories.");
        filterKind_.setTooltip("Firmware designs FIR coefficients from the requested band and tap count. The fixed IIR is 150-250 Hz.");
        detectedChannel_.setTooltip("Zero-based amplifier channel among the detected headstages, ordered A1, A2, B1, B2, C1, C2, D1, D2. Firmware maps it to the selected HLS input.");
        detectorEnabled_.setTooltip("Turning on applies channel, filter, timing, mu, sigma and K, then enables the detector.");
        filterKind_.onChange = [this] {
            if (filling_) return;
            if (filterKind_.getSelectedId() == static_cast<int>(NCLP_RIPPLE_FILTER_FIXED_IIR + 1U)) {
                filling_ = true;
                lowHz_.setText(decimal(NCLP_RIPPLE_FIXED_IIR_LOW_MILLIHZ / 1000.0), false);
                highHz_.setText(decimal(NCLP_RIPPLE_FIXED_IIR_HIGH_MILLIHZ / 1000.0), false);
                filling_ = false;
            }
            markDirty(&filterKind_);
        };

    }
    else
    {
        configureChoice(syncMode_, {"Off", "Periodic", "Recording gate"});
        configureChoice(actionMode_, {"Off", "Timed TTL pulse", "DAC waveform"});
        configureChoice(ttl0_, {"Off", "Stim trigger pulse", "Intan sync", "Trigger monitor"});
        configureChoice(ttl1_, {"Off", "Stim trigger pulse", "Intan sync", "Trigger monitor"});
        configureChoice(shape_, {"Sine", "Gaussian", "Constant"});
        configureChoice(channels_, {"A", "B", "A + B"});
        configureChoice(updateRate_, {"2 kS/s", "2.5 kS/s", "5 kS/s", "10 kS/s", "20 kS/s", "30 kS/s"});
        addAndMakeVisible(duration_);
        duration_.setColour(Label::textColourId, accent);
        duration_.setFont(FontOptions(17.0f));
        duration_.setTooltip("Waveform duration per accepted trigger, using the actual update rate and repeat count.");
        configureNumber(syncPeriod_, "1000"); configureNumber(syncHigh_, "10");
        configureNumber(ttlDuration_, "0.1"); configureNumber(parameter_, "10");
        configureNumber(minimumA_, "0"); configureNumber(maximumA_, "3.3");
        configureNumber(minimumB_, "0"); configureNumber(maximumB_, "3.3"); configureNumber(repeats_, "1");
        for (auto* editor : {&syncPeriod_, &syncHigh_, &repeats_}) editor->setInputRestrictions(10, "0123456789");
        for (auto* editor : {&minimumA_, &maximumA_, &minimumB_, &maximumB_}) editor->setTooltip("0 to 3.3 V");
        configureChoice(marker_, {"Off"});
        for (unsigned channel = 2; channel <= 15; ++channel)
            marker_.addItem("TTL " + String(channel), static_cast<int>(channel));
        configureButton(applySync_, "Apply sync", [this] { applySync(); });
        configureButton(applyRouting_, "Apply routing", [this] { (void)applyRouting(); });
        configureButton(applyMarker_, "Apply TTL", [this] { applyRecordedTtl(); });
        configureButton(loadPreset_, "Apply preset", [this] { loadPreset(); });
        configureButton(arm_, "Arm", [this] { requestArm(); });
        configureButton(disarm_, "Disarm", [this] { requestDisarm(); });
        configureButton(trigger_, "Test (trigger once)", [this] { requestTest(); });
        configureButton(clear_, "Clear counters & errors", [this] { send(Command::StimControl, {NCLP_STIM_ACTION_CLEAR}); });
        marker_.setTooltip("Record DAC activity on this Intan TTL channel.");
        updateRate_.setTooltip("Updates per second per channel. 2 and 2.5 kS/s store at least 0.4 s in RAM.");
        loadPreset_.setTooltip("Generate and verify the preset, then prepare (Prime) the DAC. Leaves it disarmed.");
        arm_.setTooltip("Enable algorithm and manual triggers. Does not start playback.");
        trigger_.setTooltip("Arm if needed, then emit exactly one test trigger.");
        disarm_.setTooltip("Stop output and disable further triggers.");
        clear_.setTooltip("Clear trigger counts and historical errors without changing settings.");
        ttlDuration_.setTooltip("Physical trigger-pulse duration. Used by timed-TTL stimulation and by the optional TTL companion to a DAC waveform.");
        shape_.onChange = [this] {
            if (filling_) return;
            parameter_.setText(shape_.getSelectedId() == 1 ? "10" : shape_.getSelectedId() == 2 ? "0.01" : "0.1", false);
            markDirty(&shape_);
        };
        actionMode_.onChange = [this] {
            if (filling_) return;
            if (actionMode_.getSelectedId() == 2 &&
                ttl0_.getSelectedId() != 2 && ttl1_.getSelectedId() != 2)
                ttl0_.setSelectedId(2, dontSendNotification);
            double seconds = 0.0;
            if (actionMode_.getSelectedId() == 2 &&
                (!readDecimal(ttlDuration_, seconds) || seconds <= 0.0))
                ttlDuration_.setText("0.1", false);
            if (actionMode_.getSelectedId() == 1) {
                if (ttl0_.getSelectedId() == 2) ttl0_.setSelectedId(1, dontSendNotification);
                if (ttl1_.getSelectedId() == 2) ttl1_.setSelectedId(1, dontSendNotification);
            }
            markDirty(&actionMode_);
        };
        const auto routeChanged = [this](ComboBox* changed) {
            if (filling_) return;
            if (changed->getSelectedId() == 2) {
                if (actionMode_.getSelectedId() == 1)
                    actionMode_.setSelectedId(2, dontSendNotification);
                double seconds = 0.0;
                if (!readDecimal(ttlDuration_, seconds) || seconds <= 0.0)
                    ttlDuration_.setText("0.1", false);
            }
            markDirty(changed);
        };
        ttl0_.onChange = [this, routeChanged] { routeChanged(&ttl0_); };
        ttl1_.onChange = [this, routeChanged] { routeChanged(&ttl1_); };
        ttlDuration_.onTextChange = [this] {
            if (filling_) return;
            if (actionMode_.getSelectedId() == 1 &&
                (ttl0_.getSelectedId() == 2 || ttl1_.getSelectedId() == 2))
                actionMode_.setSelectedId(2, dontSendNotification);
            markDirty(&ttlDuration_);
        };
        channels_.setSelectedId(3, dontSendNotification);
        updateRate_.setSelectedId(2, dontSendNotification);
    }
    filling_ = false; dirty_ = 0;
    startTimer(1000);
    updateEnabled();
}

void NCLPControlPage::setBaselineAnalysisComponent(Component* component)
{
    if (kind_ != Kind::Algorithm || component == baselineAnalysisComponent_)
        return;
    if (baselineAnalysisComponent_ != nullptr)
        removeChildComponent(baselineAnalysisComponent_);
    baselineAnalysisComponent_ = component;
    if (baselineAnalysisComponent_ != nullptr)
        addAndMakeVisible(baselineAnalysisComponent_);
    resized();
}

void NCLPControlPage::configureNumber(TextEditor& editor, const String& initial)
{
    addAndMakeVisible(editor);
    editor.setInputRestrictions(20, "0123456789.");
    editor.setColour(TextEditor::backgroundColourId, background);
    editor.setColour(TextEditor::textColourId, textColour);
    editor.setColour(TextEditor::outlineColourId, Colour(0xff415266));
    editor.setColour(TextEditor::focusedOutlineColourId, accent);
    editor.setText(initial, false);
    editor.onTextChange = [this, &editor] { if (!filling_) markDirty(&editor); };
}
void NCLPControlPage::configureChoice(ComboBox& box, std::initializer_list<const char*> choices)
{
    addAndMakeVisible(box); int id = 1;
    for (auto choice : choices) box.addItem(choice, id++);
    box.setSelectedId(1, dontSendNotification);
    box.setColour(ComboBox::backgroundColourId, background); box.setColour(ComboBox::textColourId, textColour);
    box.onChange = [this, &box] { if (!filling_) markDirty(&box); };
}
void NCLPControlPage::configureButton(TextButton& button, const String& title, std::function<void()> action)
{
    addAndMakeVisible(button); button.setButtonText(title); button.onClick = std::move(action);
    button.setColour(TextButton::buttonColourId, Colour(0xff35495b));
    button.setColour(TextButton::textColourOffId, textColour);
}
bool NCLPControlPage::send(Command command, CommandArgs args)
{
    if (!state_.connected || pending_ || !onCommand) return false;
    lastSentCommand_ = command;
    lastSentAction_ = args[0];
    hasLastSentCommand_ = true;
    appliedSection_ = command == Command::RippleApplyProfile ? (1U | 256U | 512U) :
        command == Command::RippleSetK ? 256U :
        command == Command::SetIntanSync ? 2U : command == Command::StimSetAction ? 4U :
        command == Command::DacPreset ? 8U : command == Command::DacSetIntanTtl ? 32U : 0U;
    feedback_ = "Sending command..."; error_ = false; repaint(); onCommand(command, args);
    return true;
}
void NCLPControlPage::timerCallback()
{
    if (isShowing() && state_.connected && !state_.commandBusy && !pending_ && onPollStatus)
        onPollStatus();
}
bool NCLPControlPage::number(const TextEditor& editor, uint32_t& value, uint32_t minimum, uint32_t max, const char* label)
{
    const auto input = editor.getText().trim().toStdString();
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (input.empty() || parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || value < minimum || value > max)
    { showError(String(label) + " must be " + String(minimum) + " to " + String(max) + "."); return false; }
    return true;
}
bool NCLPControlPage::scaledNumber(const TextEditor& editor, double scale, uint32_t& value,
                                  uint32_t minimum, uint32_t max, const char* label)
{
    double input = 0;
    if (!readDecimal(editor, input) || input > static_cast<double>(max) / scale ||
        std::round(input * scale) < minimum)
    { showError(String(label) + ": " + decimal(minimum / scale, 8) + " to " + decimal(max / scale, 8) + "."); return false; }
    value = static_cast<uint32_t>(std::round(input * scale));
    return true;
}
void NCLPControlPage::showError(const String& message) { feedback_ = message; error_ = true; repaint(); }
bool NCLPControlPage::fp32Number(const TextEditor& editor, uint32_t& bits, bool nonnegative, const char* label)
{
    const auto input = editor.getText().trim().toStdString();
    float value = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value, std::chars_format::general);
    if (input.empty() || parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() ||
        !std::isfinite(value) || (nonnegative && value < 0)) {
        showError(String(label) + (nonnegative ? " must be finite and nonnegative." : " must be finite FP32.")); return false;
    }
    bits = encodeFp32(value); return true;
}
bool NCLPControlPage::rippleMicrovolts(const TextEditor& editor, uint32_t& bits,
                                      bool nonnegative, const char* label)
{
    uint32_t microvoltBits = 0U;
    if (!fp32Number(editor, microvoltBits, nonnegative, label)) return false;
    const float counts = decodeFp32(microvoltBits) / kDefaultBitVolts;
    const float minimumCounts = nonnegative ? 0.0f : -32768.0f;
    if (!std::isfinite(counts) || counts < minimumCounts || counts >= 32768.0f) {
        showError(String(label) + " must convert to a finite Q16.16 detector-count value.");
        return false;
    }
    bits = encodeFp32(counts);
    return true;
}
bool NCLPControlPage::buildRippleProfileArgs(CommandArgs& args,
                                             bool useAppliedThreshold)
{
    uint32_t detectedChannel = 0;
    if (!state_.scanValid || !state_.topology.valid ||
        state_.topology.channels.empty()) {
        showError("Scan and detect at least one amplifier channel first.");
        return false;
    }
    if (!number(detectedChannel_, detectedChannel, 0,
                static_cast<uint32_t>(state_.topology.channels.size() - 1U),
                "Detected channel") ||
        filterKind_.getSelectedId() < 1 || filterKind_.getSelectedId() > 3 ||
        !number(firTaps_, args[4], NCLP_RIPPLE_FIR_TAPS_MIN, NCLP_RIPPLE_FIR_TAPS_MAX, "FIR taps") ||
        !scaledNumber(windowMs_, 1000.0, args[5], RIPPLE_DETECTOR_POWER_WINDOW_US_MIN,
                      RIPPLE_DETECTOR_POWER_WINDOW_US_MAX, "PWT window (ms)") ||
        !number(refractoryMs_, args[6], 0, RIPPLE_DETECTOR_REFRACTORY_PERIOD_MS_MAX, "Refractory (ms)")) return false;
    // Firmware consumes the compact zero-based detected-channel index.  The
    // topology entry is used only to show the physical lane/input mapping.
    args[0] = detectedChannel;
    args[1] = static_cast<uint32_t>(filterKind_.getSelectedId() - 1);
    if (args[1] == NCLP_RIPPLE_FILTER_FIXED_IIR) {
        args[2] = NCLP_RIPPLE_FIXED_IIR_LOW_MILLIHZ;
        args[3] = NCLP_RIPPLE_FIXED_IIR_HIGH_MILLIHZ;
    } else if (!scaledNumber(lowHz_, 1000.0, args[2], 1, 1500000U, "Low cutoff (Hz)") ||
               !scaledNumber(highHz_, 1000.0, args[3], 1, 1500000U, "High cutoff (Hz)")) return false;
    if (args[2] >= args[3] || args[3] >= 1500000U) {
        showError("Filter band must satisfy 0 < low < high < 1500 Hz."); return false;
    }
    if (useAppliedThreshold) {
        args[7] = state_.outputs.muBits;
        args[8] = state_.outputs.sigmaBits;
        args[9] = state_.outputs.kBits;
    } else if (!rippleMicrovolts(muUv_, args[7], false, "Mu") ||
               !rippleMicrovolts(sigmaUv_, args[8], true, "Sigma") ||
               !fp32Number(k_, args[9], true, "K")) return false;
    if (decodeFp32(args[9]) >= 32768.0f) {
        showError("K must fit the detector Q16.16 range."); return false;
    }
    const float theta = decodeFp32(args[7]) + decodeFp32(args[9]) * decodeFp32(args[8]);
    const uint32_t samples = (args[5] * 3U + 999U) / 1000U;
    if (!std::isfinite(theta) || theta < 0 || !std::isfinite(static_cast<float>(samples) * (theta * theta))) {
        showError("Mu + K*sigma must be nonnegative, with a finite squared threshold."); return false;
    }
    return true;
}
void NCLPControlPage::applyRippleK()
{
    CommandArgs args{};
    if (!fp32Number(k_, args[0], true, "K")) return;
    if (decodeFp32(args[0]) >= 32768.0f) {
        showError("K must fit the detector Q16.16 range."); return;
    }
    const auto& out = state_.outputs;
    const float theta = decodeFp32(out.muBits) + decodeFp32(args[0]) * decodeFp32(out.sigmaBits);
    const uint32_t samples = (out.windowUs * 3U + 999U) / 1000U;
    if (!std::isfinite(theta) || theta < 0 || !std::isfinite(static_cast<float>(samples) * (theta * theta))) {
        showError("K must produce a nonnegative, finite squared threshold with the applied mu and sigma."); return;
    }
    send(Command::RippleSetK, args);
}
void NCLPControlPage::detectorToggleClicked()
{
    if (filling_) return;
    if (detectorEnabled_.getToggleState()) requestDetectorEnabled();
    else requestDetectorDisabled();
}

String NCLPControlPage::detectorEnableDisabledReason() const
{
    const auto& out = state_.outputs;
    if (!state_.connected) return "Connect to the board.";
    if (pending_ || state_.commandBusy) return "Waiting for the current command.";
    if (detectorAction_ != DetectorAction::None)
        return "Waiting for the detector state change.";
    if (!out.valid) return "Refresh the board state.";
    if (state_.config.sampleRateHz != 30000U)
        return "Select 30 kS/s before enabling the detector.";
    if (!state_.scanValid || !state_.initialized || !state_.topology.valid ||
        state_.topology.channels.empty())
        return "Scan and initialize the headstages first.";
    if (state_.streaming || state_.routeUdp || state_.listenerRunning ||
        (state_.controlState != ControlState::Idle &&
         state_.controlState != ControlState::Ready))
        return "Stop acquisition so the detector profile can be applied.";
    if (out.rippleBusy()) return "Waiting for the ripple detector to finish updating.";
    if ((out.rippleStatus & RIPPLE_DETECTOR_STATUS_FAULT) != 0U ||
        state_.controlState == ControlState::Fault)
        return "Resolve the detector fault before enabling it.";
    return {};
}

void NCLPControlPage::requestDetectorEnabled()
{
    const String reason = detectorEnableDisabledReason();
    if (reason.isNotEmpty()) {
        filling_ = true;
        detectorEnabled_.setToggleState(false, dontSendNotification);
        filling_ = false;
        showError(reason);
        return;
    }
    if (!buildRippleProfileArgs(detectorProfileArgs_, false)) {
        filling_ = true;
        detectorEnabled_.setToggleState(false, dontSendNotification);
        filling_ = false;
        return;
    }
    detectorDesiredEnabled_ = true;
    detectorAction_ = DetectorAction::ApplyingProfile;
    feedback_ = "Applying detector profile...";
    error_ = false;
    if (!send(Command::RippleApplyProfile, detectorProfileArgs_)) {
        detectorDesiredEnabled_ = false;
        detectorAction_ = DetectorAction::None;
        showError("Could not queue the detector profile.");
    }
}

void NCLPControlPage::requestDetectorDisabled()
{
    detectorDesiredEnabled_ = false;
    feedback_ = (pending_ || state_.commandBusy)
        ? "Disable requested; waiting for the current command..."
        : "Disabling detector...";
    error_ = false;
    continueDetectorAction();
    updateEnabled();
    repaint();
}

bool NCLPControlPage::detectorProfileMatchesReadback() const
{
    const auto& out = state_.outputs;
    if (!out.valid) return false;
    return out.detectedChannelIndex == detectorProfileArgs_[0] &&
           out.filterKind == detectorProfileArgs_[1] &&
           out.lowMilliHz == detectorProfileArgs_[2] &&
           out.highMilliHz == detectorProfileArgs_[3] &&
           out.firTaps == detectorProfileArgs_[4] &&
           out.windowUs == detectorProfileArgs_[5] &&
           out.refractoryMs == detectorProfileArgs_[6] &&
           out.muBits == detectorProfileArgs_[7] &&
           out.sigmaBits == detectorProfileArgs_[8] &&
           out.kBits == detectorProfileArgs_[9];
}

void NCLPControlPage::continueDetectorAction()
{
    if (kind_ != Kind::Algorithm) return;
    if (!state_.connected) {
        detectorDesiredEnabled_ = false;
        detectorAction_ = DetectorAction::None;
        return;
    }
    if (pending_ || state_.commandBusy || !state_.outputs.valid) return;

    if (!detectorDesiredEnabled_) {
        if (!state_.outputs.rippleEnabled()) {
            detectorAction_ = DetectorAction::None;
            return;
        }
        if (detectorAction_ == DetectorAction::Stopping ||
            detectorAction_ == DetectorAction::WaitingForStopped)
            return;
        detectorAction_ = DetectorAction::Stopping;
        if (!send(Command::RippleControl, {NCLP_RIPPLE_ACTION_STOP}))
            detectorAction_ = DetectorAction::None;
        return;
    }

    if (detectorAction_ == DetectorAction::WaitingForProfileReadback) {
        if (!detectorProfileMatchesReadback()) return;
        detectorAction_ = DetectorAction::Starting;
        feedback_ = "Detector profile verified; enabling...";
        if (!send(Command::RippleControl, {NCLP_RIPPLE_ACTION_START})) {
            detectorDesiredEnabled_ = false;
            detectorAction_ = DetectorAction::None;
        }
        return;
    }
    if (detectorAction_ == DetectorAction::WaitingForEnabled &&
        state_.outputs.rippleEnabled()) {
        detectorAction_ = DetectorAction::None;
        feedback_ = "Detector enabled.";
        error_ = false;
        updateEnabled();
        repaint();
    } else if (detectorAction_ == DetectorAction::WaitingForStopped &&
               !state_.outputs.rippleEnabled()) {
        detectorAction_ = DetectorAction::None;
        feedback_ = "Detector disabled.";
        error_ = false;
        updateEnabled();
        repaint();
    }
}

void NCLPControlPage::applySync()
{
    CommandArgs args{}; args[0] = static_cast<uint32_t>(syncMode_.getSelectedId() - 1);
    if (args[0] == NCLP_SYNC_MODE_PERIODIC &&
        (!number(syncPeriod_, args[1], 1, 65535, "Period frames") || !number(syncHigh_, args[2], 1, args[1], "High frames"))) return;
    send(Command::SetIntanSync, args);
}
bool NCLPControlPage::buildRoutingArgs(CommandArgs& args)
{
    args[0] = static_cast<uint32_t>(actionMode_.getSelectedId() - 1);
    args[1] = static_cast<uint32_t>(ttl0_.getSelectedId() - 1);
    args[2] = static_cast<uint32_t>(ttl1_.getSelectedId() - 1);
    args[4] = state_.outputs.markerMask;
    args[5] = 1U; // ARM is the user-facing trigger admission control.
    const bool ttl0Stim = args[1] == TTL_ROUTER_SOURCE_STIM;
    const bool ttl1Stim = args[2] == TTL_ROUTER_SOURCE_STIM;
    if (args[1] != 0 && args[1] == args[2]) {
        showError("A source can drive only one TTL connector.");
        return false;
    }
    if (args[0] == STIM_OUTPUT_MODE_OFF && (ttl0Stim || ttl1Stim)) {
        showError("A stimulation trigger pulse requires Timed TTL pulse or DAC waveform mode.");
        return false;
    }
    if (args[0] == STIM_OUTPUT_MODE_TTL && !ttl0Stim && !ttl1Stim) {
        showError("Select Stim trigger pulse on TTL 0 or TTL 1 for timed-TTL output.");
        return false;
    }
    const bool physicalStimPulse = args[0] == STIM_OUTPUT_MODE_TTL || ttl0Stim || ttl1Stim;
    if (physicalStimPulse &&
        !scaledNumber(ttlDuration_, NCLP_ALGORITHM_CLOCK_HZ, args[3], 1, maximum,
                      "TTL pulse duration (s)"))
        return false;
    if (!physicalStimPulse) args[3] = 0U;
    return true;
}
bool NCLPControlPage::applyRouting()
{
    CommandArgs args{};
    if (!buildRoutingArgs(args)) return false;
    send(Command::StimSetAction, args);
    return true;
}

void NCLPControlPage::cancelQueuedOutputAction()
{
    outputGoal_ = OutputGoal::None;
    outputFollowUp_ = OutputFollowUp::None;
}

void NCLPControlPage::requestArm()
{
    const String reason = armDisabledReason();
    if (reason.isNotEmpty()) {
        showError(reason);
        return;
    }
    outputGoal_ = OutputGoal::Arm;
    if (dirty_ & 4U) {
        if (!applyRouting()) cancelQueuedOutputAction();
        return;
    }
    outputGoal_ = OutputGoal::None;
    send(Command::StimControl, {NCLP_STIM_ACTION_ARM});
}

void NCLPControlPage::requestTest()
{
    if (state_.outputs.armed()) {
        cancelQueuedOutputAction();
        send(Command::StimControl, {NCLP_STIM_ACTION_TRIGGER});
        return;
    }
    const String reason = armDisabledReason();
    if (reason.isNotEmpty()) {
        showError(reason);
        return;
    }
    outputGoal_ = OutputGoal::Trigger;
    if (dirty_ & 4U) {
        if (!applyRouting()) cancelQueuedOutputAction();
        return;
    }
    send(Command::StimControl, {NCLP_STIM_ACTION_ARM});
}

void NCLPControlPage::requestDisarm()
{
    if (!state_.connected || !onCommand) {
        showError("Connect to the board before disarming output.");
        return;
    }
    // DISARM is the safety-priority action.  A click made while ARM, routing,
    // or a test trigger is in flight must not be lost; cancel every local
    // follow-up and send it as soon as the single command channel is free.
    cancelQueuedOutputAction();
    disarmRequested_ = true;
    feedback_ = (pending_ || state_.commandBusy)
        ? "Disarm requested; waiting for the current command..."
        : "Disarming output...";
    error_ = false;
    updateEnabled();
    repaint();
    continueQueuedDisarmAction();
}

void NCLPControlPage::continueQueuedDisarmAction()
{
    if (kind_ != Kind::Output || !disarmRequested_ || pending_ ||
        state_.commandBusy || !state_.connected || !onCommand)
        return;
    disarmRequested_ = false;
    if (!send(Command::StimControl, {NCLP_STIM_ACTION_DISARM})) {
        disarmRequested_ = true;
        feedback_ = "Disarm requested; waiting for the command channel...";
        updateEnabled();
        repaint();
    }
}

void NCLPControlPage::continueQueuedOutputAction()
{
    if (kind_ != Kind::Output || pending_ || state_.commandBusy ||
        outputFollowUp_ == OutputFollowUp::None)
        return;
    const OutputFollowUp followUp = outputFollowUp_;
    outputFollowUp_ = OutputFollowUp::None;
    if (followUp == OutputFollowUp::Arm)
        send(Command::StimControl, {NCLP_STIM_ACTION_ARM});
    else
        send(Command::StimControl, {NCLP_STIM_ACTION_TRIGGER});
}
double NCLPControlPage::actualDacRate() const
{
    const auto clock = state_.outputs.dacClockConfig;
    const auto outputDivide = clock & 255U, inputDivide = (clock >> 8) & 15U, multiply = (clock >> 12) & 255U;
    if (!outputDivide || !inputDivide || !multiply || updateRate_.getSelectedId() < 1 || updateRate_.getSelectedId() > 6) return 0;
    const auto engine = static_cast<uint32_t>(140000000ULL * multiply / (inputDivide * outputDivide));
    const auto requested = rates[updateRate_.getSelectedId() - 1];
    const auto period = (engine + requested / 2U) / requested;
    return period >= 256 && engine <= 40000000 ? static_cast<double>(engine) / period : 0;
}
void NCLPControlPage::loadPreset()
{
    CommandArgs args{};
    if (shape_.getSelectedId() < 1 || shape_.getSelectedId() > 3 ||
        channels_.getSelectedId() < 1 || channels_.getSelectedId() > 3 ||
        updateRate_.getSelectedId() < 1 || updateRate_.getSelectedId() > 6)
    { showError("Select a waveform, channel and update rate."); return; }
    if (dirty_ & 4U) {
        showError("Apply the TTL routing changes before applying a DAC preset.");
        return;
    }
    const bool ttl0Stim = state_.outputs.ttl0Source == TTL_ROUTER_SOURCE_STIM;
    const bool ttl1Stim = state_.outputs.ttl1Source == TTL_ROUTER_SOURCE_STIM;
    if ((ttl0Stim || ttl1Stim) && state_.outputs.ttlWidthCycles == 0U) {
        showError("Set a positive TTL pulse duration and Apply routing before applying a DAC preset.");
        return;
    }
    if (!ttl0Stim && !ttl1Stim && state_.outputs.ttlWidthCycles != 0U) {
        showError("Apply routing to clear the unused TTL pulse duration before applying a DAC preset.");
        return;
    }
    args[0] = static_cast<uint32_t>(shape_.getSelectedId() - 1);
    args[1] = static_cast<uint32_t>(channels_.getSelectedId());
    args[2] = rates[updateRate_.getSelectedId() - 1];
    args[9] = 1U;
    const bool constant = args[0] == NCLP_DAC_PRESET_CONSTANT;
    if (!scaledNumber(parameter_, args[0] == NCLP_DAC_PRESET_SINE ? 1000 : 1000000,
                      args[3], 1, maximum, "Waveform timing")) return;
    if (args[1] & NCLP_DAC_CHANNEL_A) {
        if (!scaledNumber(minimumA_, 1000000, args[4], 0, NCLP_DAC_REFERENCE_UV, "A voltage (V)")) return;
        if (constant) args[5] = args[4];
        else if (!scaledNumber(maximumA_, 1000000, args[5], args[4], NCLP_DAC_REFERENCE_UV, "A maximum (V)")) return;
    }
    if (args[1] & NCLP_DAC_CHANNEL_B) {
        if (!scaledNumber(minimumB_, 1000000, args[6], 0, NCLP_DAC_REFERENCE_UV, "B voltage (V)")) return;
        if (constant) args[7] = args[6];
        else if (!scaledNumber(maximumB_, 1000000, args[7], args[6], NCLP_DAC_REFERENCE_UV, "B maximum (V)")) return;
    }
    if (constant) args[8] = 1;
    else if (!number(repeats_, args[8], 1, maximum, "Repeat count")) return;
    const double rate = actualDacRate();
    if (rate == 0) { showError("DAC clock is unavailable."); return; }
    double samples = 1;
    if (args[0] == NCLP_DAC_PRESET_SINE) {
        samples = std::round(rate * 1000 / args[3]);
        if (samples < NCLP_DAC_SINE_MIN_SAMPLES || samples > STIM_RAM_DEPTH_WORDS)
        { showError("Frequency is outside the displayed range."); return; }
    } else if (args[0] == NCLP_DAC_PRESET_GAUSSIAN) {
        const double half = std::round(4 * args[3] * rate / 1000000);
        if (half < 1 || half > 511) { showError("Sigma is outside the displayed range."); return; }
        samples = 2 * half + 1;
    } else samples = std::round(args[3] * rate / 1000000);
    if (samples < 1 || samples * args[8] > maximum)
    { showError("Duration or repeat count exceeds the playback limit."); return; }
    send(Command::DacPreset, args);
}
String NCLPControlPage::waveformGuidance() const
{
    const double rate = actualDacRate();
    if (rate == 0) return "Connect to read the DAC clock.";
    double parameter = 0;
    if (!readDecimal(parameter_, parameter)) parameter = 0;
    const double scale = shape_.getSelectedId() == 1 ? 1000.0 : 1000000.0;
    parameter = std::round(parameter * scale) / scale;
    if (shape_.getSelectedId() == 1) {
        const double samples = parameter > 0 ? std::round(rate / parameter) : 0;
        String text = "Recommended: " + decimal(rate / 1024, 3) + " to " + decimal(rate / 32, 3) + " Hz";
        if (samples >= 32 && samples <= 1024) text += "   |   Actual: " + decimal(rate / samples, 3) + " Hz";
        return text;
    }
    if (shape_.getSelectedId() == 2) {
        const double half = std::round(4 * parameter * rate);
        String text = "Sigma: " + decimal(1 / (4 * rate)) + " to " + decimal(511 / (4 * rate)) + " s";
        if (half >= 1 && half <= 511) text += "   |   Actual: " + decimal(half / (4 * rate)) + " s";
        return text;
    }
    return "Actual hold: " + waveformDuration() + " s";
}
String NCLPControlPage::waveformDuration() const
{
    const double rate = actualDacRate();
    double parameter = 0;
    const auto kind = shape_.getSelectedId();
    const double scale = kind == 1 ? 1000.0 : 1000000.0;
    if (rate == 0 || !readDecimal(parameter_, parameter) || parameter > maximum / scale) return "-";
    parameter = std::round(parameter * scale) / scale;
    if (parameter == 0) return "-";
    uint32_t repeats = 1;
    if (kind != 3) {
        const auto input = repeats_.getText().trim().toStdString();
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), repeats);
        if (input.empty() || parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || repeats == 0) return "-";
    }
    double samples;
    if (kind == 1) {
        samples = std::round(rate / parameter);
        if (samples < NCLP_DAC_SINE_MIN_SAMPLES || samples > STIM_RAM_DEPTH_WORDS) return "-";
    } else if (kind == 2) {
        const double half = std::round(4 * parameter * rate);
        if (half < 1 || half > 511) return "-";
        samples = 2 * half + 1;
    } else samples = std::round(parameter * rate);
    const double updates = samples * repeats;
    if (updates < 1 || updates > maximum) return "-";
    return decimal(updates / rate);
}
void NCLPControlPage::applyRecordedTtl()
{
    send(Command::DacSetIntanTtl, {marker_.getSelectedId() == 1 ? 0U : static_cast<uint32_t>(marker_.getSelectedId())});
}
void NCLPControlPage::commandFinished(const String& error)
{
    const bool detectorProfileCommand = kind_ == Kind::Algorithm &&
        detectorAction_ == DetectorAction::ApplyingProfile &&
        hasLastSentCommand_ && lastSentCommand_ == Command::RippleApplyProfile;
    const bool detectorStartCommand = kind_ == Kind::Algorithm &&
        detectorAction_ == DetectorAction::Starting &&
        hasLastSentCommand_ && lastSentCommand_ == Command::RippleControl &&
        lastSentAction_ == NCLP_RIPPLE_ACTION_START;
    const bool detectorStopCommand = kind_ == Kind::Algorithm &&
        detectorAction_ == DetectorAction::Stopping &&
        hasLastSentCommand_ && lastSentCommand_ == Command::RippleControl &&
        lastSentAction_ == NCLP_RIPPLE_ACTION_STOP;

    error_ = error.isNotEmpty();
    feedback_ = error_ ? error : appliedSection_ == 8U ? "Preset ready." : "Applied.";
    if (!error_) {
        dirty_ &= ~appliedSection_;
        if (detectorProfileCommand) {
            detectorAction_ = detectorDesiredEnabled_
                ? DetectorAction::WaitingForProfileReadback
                : DetectorAction::None;
            feedback_ = detectorDesiredEnabled_
                ? "Detector profile applied; verifying readback..."
                : "Detector enable cancelled.";
        } else if (detectorStartCommand) {
            detectorAction_ = DetectorAction::WaitingForEnabled;
            feedback_ = detectorDesiredEnabled_
                ? "Detector start accepted; verifying readback..."
                : "Detector start completed; disabling as requested...";
        } else if (detectorStopCommand) {
            detectorAction_ = DetectorAction::WaitingForStopped;
            feedback_ = "Detector stop accepted; verifying readback...";
        }
        if (kind_ == Kind::Output && hasLastSentCommand_) {
            if (lastSentCommand_ == Command::StimSetAction &&
                outputGoal_ != OutputGoal::None)
                outputFollowUp_ = OutputFollowUp::Arm;
            else if (lastSentCommand_ == Command::StimControl &&
                     lastSentAction_ == NCLP_STIM_ACTION_ARM) {
                if (outputGoal_ == OutputGoal::Trigger)
                    outputFollowUp_ = OutputFollowUp::Trigger;
                else
                    cancelQueuedOutputAction();
            } else if (lastSentCommand_ == Command::StimControl &&
                       lastSentAction_ == NCLP_STIM_ACTION_TRIGGER)
                cancelQueuedOutputAction();
        }
    } else {
        if (detectorProfileCommand || detectorStartCommand) {
            detectorDesiredEnabled_ = false;
            detectorAction_ = DetectorAction::None;
        } else if (detectorStopCommand) {
            // Keep the UI truthful after a rejected STOP; the user can toggle
            // it off again to retry once the board is ready.
            detectorDesiredEnabled_ = state_.outputs.rippleEnabled();
            detectorAction_ = DetectorAction::None;
        }
        cancelQueuedOutputAction();
    }
    hasLastSentCommand_ = false;
    settingsRefreshPending_ = true;
    repaint();
}
void NCLPControlPage::setState(const SessionSnapshot& state, bool pending, bool refreshSettings)
{
    const bool rippleThresholdChanged = kind_ == Kind::Algorithm && state.connected &&
        state.outputs.valid && (!state_.outputs.valid ||
        state.outputs.muBits != state_.outputs.muBits ||
        state.outputs.sigmaBits != state_.outputs.sigmaBits);
    const bool rippleChannelMappingChanged = kind_ == Kind::Algorithm &&
        state.connected && state.outputs.valid &&
        (!state_.outputs.valid || state.scanValid != state_.scanValid ||
         state.outputs.detectedChannelIndex !=
             state_.outputs.detectedChannelIndex ||
         state.topology.valid != state_.topology.valid ||
         state.topology.physicalMask != state_.topology.physicalMask ||
         state.topology.packedChipIds != state_.topology.packedChipIds ||
         state.topology.channels.size() != state_.topology.channels.size());
    if (state.resetGeneration != state_.resetGeneration) {
        dirty_ = 0; appliedSection_ = 0;
        cancelQueuedOutputAction();
        disarmRequested_ = false;
        detectorDesiredEnabled_ = false;
        detectorAction_ = DetectorAction::None;
        error_ = false; feedback_ = "Reset complete.";
        settingsRefreshPending_ = true;
    } else if (state.outputs.valid &&
               state.outputs.buttonPressCount > state_.outputs.buttonPressCount) {
        error_ = false; feedback_ = "Disarmed by board button.";
        settingsRefreshPending_ = true;
    }
    state_ = state; pending_ = pending;
    const auto& output = state.outputs;
    if (kind_ == Kind::Algorithm && detectorAction_ == DetectorAction::None)
        detectorDesiredEnabled_ = output.valid && output.rippleEnabled();
    if (!state.connected) settingsRefreshPending_ = true;
    if (rippleThresholdChanged && !(dirty_ & 512U)) {
        // A live threshold readback can change independently. Populate the
        // microvolt drafts without disturbing an in-progress manual edit.
        muUv_.setText(decimal(static_cast<double>(decodeFp32(output.muBits)) * kDefaultBitVolts, 6), false);
        sigmaUv_.setText(decimal(static_cast<double>(decodeFp32(output.sigmaBits)) * kDefaultBitVolts, 6), false);
    }
    // Status polls never replace drafts, selections or caret positions.
    if (state.connected && output.valid &&
        (settingsRefreshPending_ || refreshSettings ||
         rippleChannelMappingChanged))
    {
        filling_ = true;
        if (kind_ == Kind::Algorithm) {
            if (!(dirty_ & 1U)) {
                size_t detectedIndex = 0;
                if (state.scanValid &&
                    detectedIndexFromReadback(state.topology,
                                              output.detectedChannelIndex,
                                              detectedIndex))
                    detectedChannel_.setText(
                        String(static_cast<int>(detectedIndex)), false);
                else
                    detectedChannel_.setText("-", false);
                filterKind_.setSelectedId(static_cast<int>(output.filterKind + 1U), dontSendNotification);
                firTaps_.setText(String(output.firTaps), false);
                lowHz_.setText(decimal(output.lowMilliHz / 1000.0), false);
                highHz_.setText(decimal(output.highMilliHz / 1000.0), false);
                windowMs_.setText(decimal(output.windowUs / 1000.0), false);
                refractoryMs_.setText(String(output.refractoryMs), false);
            }
            if (!(dirty_ & 256U)) {
                k_.setText(fp32Text(output.kBits), false);
            }
            if (!(dirty_ & 512U)) {
                muUv_.setText(decimal(static_cast<double>(decodeFp32(output.muBits)) * kDefaultBitVolts, 6), false);
                sigmaUv_.setText(decimal(static_cast<double>(decodeFp32(output.sigmaBits)) * kDefaultBitVolts, 6), false);
            }
        } else if (kind_ == Kind::Output) {
            if (!(dirty_ & 2U)) {
                syncMode_.setSelectedId(static_cast<int>(output.syncMode + 1), dontSendNotification);
                syncPeriod_.setText(String(output.syncPeriodFrames), false); syncHigh_.setText(String(output.syncHighFrames), false);
            }
            if (!(dirty_ & 4U)) {
                actionMode_.setSelectedId(static_cast<int>(output.actionMode + 1), dontSendNotification);
                ttl0_.setSelectedId(static_cast<int>(output.ttl0Source + 1), dontSendNotification);
                ttl1_.setSelectedId(static_cast<int>(output.ttl1Source + 1), dontSendNotification);
                ttlDuration_.setText(decimal(static_cast<double>(output.ttlWidthCycles) / NCLP_ALGORITHM_CLOCK_HZ, 8), false);
            }
            if (!(dirty_ & 32U)) {
                int selected = 1;
                for (int ttl = 2; ttl <= 15; ++ttl) if (output.markerMask == (1U << (ttl - 2))) selected = ttl;
                marker_.setSelectedId(selected, dontSendNotification);
            }
            if (!(dirty_ & 8U) && !output.presetValid) resetPresetEditors();
            if (!(dirty_ & 8U) && output.presetValid) {
                shape_.setSelectedId(static_cast<int>(output.presetKind + 1), dontSendNotification);
                channels_.setSelectedId(static_cast<int>(output.presetChannels), dontSendNotification);
                for (int index = 0; index < 6; ++index) if (rates[index] == output.presetRateHz) updateRate_.setSelectedId(index + 1, dontSendNotification);
                parameter_.setText(decimal(output.presetParameter / (output.presetKind == NCLP_DAC_PRESET_SINE ? 1000.0 : 1000000.0)), false);
                minimumA_.setText(decimal(output.minimumAuv / 1000000.0), false); maximumA_.setText(decimal(output.maximumAuv / 1000000.0), false);
                minimumB_.setText(decimal(output.minimumBuv / 1000000.0), false); maximumB_.setText(decimal(output.maximumBuv / 1000000.0), false);
                repeats_.setText(String(output.presetRepeats), false);
            }
        }
        filling_ = false;
        settingsRefreshPending_ = false;
    }
    updateEnabled(); repaint();
    continueDetectorAction();
    continueQueuedDisarmAction();
    continueQueuedOutputAction();
}
void NCLPControlPage::resetPresetEditors()
{
    shape_.setSelectedId(1, dontSendNotification);
    channels_.setSelectedId(3, dontSendNotification);
    updateRate_.setSelectedId(2, dontSendNotification);
    parameter_.setText("10", false); repeats_.setText("1", false);
    minimumA_.setText("0", false); maximumA_.setText("3.3", false);
    minimumB_.setText("0", false); maximumB_.setText("3.3", false);
}
void NCLPControlPage::markDirty(Component* field)
{
    if (kind_ == Kind::Algorithm) {
        if (field == &k_) dirty_ |= 256U;
        else if (field == &muUv_ || field == &sigmaUv_) dirty_ |= 512U;
        else dirty_ |= 1U;
    }
    else if (field == &syncMode_ || field == &syncPeriod_ || field == &syncHigh_) dirty_ |= 2U;
    else if (field == &actionMode_ || field == &ttl0_ || field == &ttl1_ || field == &ttlDuration_) dirty_ |= 4U;
    else if (field == &marker_) dirty_ |= 32U;
    else dirty_ |= 8U;
    updateEnabled(); repaint();
}
void NCLPControlPage::updateEnabled()
{
    const bool available = state_.connected && !pending_ && !state_.commandBusy;
    const bool current = available && state_.outputs.valid;
    refresh_.setEnabled(available);
    if (kind_ == Kind::Algorithm) {
        const bool topologyAvailable = state_.scanValid &&
                                       state_.topology.valid &&
                                       !state_.topology.channels.empty();
        const bool detectorIdle = current && !state_.outputs.rippleEnabled() && !state_.outputs.rippleBusy() &&
                                  (state_.outputs.rippleStatus & RIPPLE_DETECTOR_STATUS_FAULT) == 0 &&
                                  state_.controlState != ControlState::Fault;
        const bool acquisitionStopped = !state_.streaming &&
            (state_.controlState == ControlState::Idle || state_.controlState == ControlState::Ready);
        const bool actionPending = detectorAction_ != DetectorAction::None;
        const bool profileConfigurable = detectorIdle && acquisitionStopped &&
                                         !actionPending && topologyAvailable;
        for (Component* component : std::initializer_list<Component*>{&detectedChannel_, &filterKind_, &firTaps_,
            &lowHz_, &highHz_, &windowMs_, &refractoryMs_, &muUv_, &sigmaUv_})
            component->setEnabled(profileConfigurable);
        const bool fir = filterKind_.getSelectedId() != static_cast<int>(NCLP_RIPPLE_FILTER_FIXED_IIR + 1U);
        firTaps_.setEnabled(profileConfigurable && fir);
        lowHz_.setEnabled(profileConfigurable && fir); highHz_.setEnabled(profileConfigurable && fir);
        const bool liveK = current && !actionPending &&
                           state_.controlState != ControlState::Fault;
        k_.setEnabled(liveK); applyRippleK_.setEnabled(liveK);
        const bool shownEnabled = actionPending
            ? detectorDesiredEnabled_
            : current && state_.outputs.rippleEnabled();
        filling_ = true;
        detectorEnabled_.setToggleState(shownEnabled, dontSendNotification);
        detectorEnabled_.setButtonText(
            detectorAction_ == DetectorAction::ApplyingProfile ||
            detectorAction_ == DetectorAction::WaitingForProfileReadback ||
            detectorAction_ == DetectorAction::Starting ||
            detectorAction_ == DetectorAction::WaitingForEnabled
                ? "Enabling detector..."
                : detectorAction_ == DetectorAction::Stopping ||
                  detectorAction_ == DetectorAction::WaitingForStopped
                    ? "Disabling detector..."
                    : shownEnabled ? "Detector enabled" : "Detector disabled");
        filling_ = false;
        const String enableReason = detectorEnableDisabledReason();
        const bool canRequestEnable = !shownEnabled && enableReason.isEmpty();
        const bool canRequestDisable = shownEnabled && state_.connected;
        detectorEnabled_.setEnabled(canRequestEnable || canRequestDisable);
        detectorEnabled_.setTooltip(shownEnabled
            ? "Turn off to stop ripple detection."
            : enableReason.isEmpty()
                ? "Apply the current profile and enable ripple detection."
                : enableReason);
        return;
    }
    const bool stopped = current && !state_.streaming && (state_.controlState == ControlState::Idle || state_.controlState == ControlState::Ready);
    const bool unlocked = current && !state_.outputs.locked();
    for (Component* component : std::initializer_list<Component*>{&syncMode_, &syncPeriod_, &syncHigh_, &applySync_}) component->setEnabled(stopped);
    syncPeriod_.setEnabled(stopped && syncMode_.getSelectedId() == 2); syncHigh_.setEnabled(stopped && syncMode_.getSelectedId() == 2);
    for (Component* component : std::initializer_list<Component*>{&actionMode_, &ttl0_, &ttl1_, &applyRouting_}) component->setEnabled(unlocked);
    const bool timedPulseDraft = actionMode_.getSelectedId() == 2 ||
                                 ttl0_.getSelectedId() == 2 ||
                                 ttl1_.getSelectedId() == 2;
    ttlDuration_.setEnabled(unlocked && timedPulseDraft);
    for (Component* component : std::initializer_list<Component*>{&shape_, &channels_, &updateRate_, &parameter_, &minimumA_, &maximumA_, &minimumB_, &maximumB_, &repeats_, &loadPreset_}) component->setEnabled(unlocked && stopped);
    const bool constant = shape_.getSelectedId() == 3;
    maximumA_.setVisible(!constant); maximumB_.setVisible(!constant); repeats_.setVisible(!constant);
    duration_.setVisible(!constant);
    duration_.setText(waveformDuration(), dontSendNotification);
    minimumA_.setEnabled(unlocked && stopped && (channels_.getSelectedId() & 1)); maximumA_.setEnabled(minimumA_.isEnabled());
    minimumB_.setEnabled(unlocked && stopped && (channels_.getSelectedId() & 2)); maximumB_.setEnabled(minimumB_.isEnabled());
    marker_.setEnabled(unlocked && stopped); applyMarker_.setEnabled(unlocked && stopped);
    const String armReason = armDisabledReason();
    arm_.setEnabled(armReason.isEmpty());
    arm_.setTooltip(armReason.isEmpty() ? "Enable algorithm and manual triggers. Does not start playback." : armReason);
    trigger_.setEnabled(current && !state_.outputs.busy() && !state_.outputs.safeOff() &&
                        (state_.outputs.armed() || armReason.isEmpty()));
    trigger_.setTooltip(state_.outputs.armed()
        ? "Emit exactly one test trigger."
        : armReason.isEmpty()
            ? "Apply pending routing if needed, arm, then emit exactly one test trigger."
            : armReason);
    const bool disarmInFlight = pending_ && hasLastSentCommand_ &&
        lastSentCommand_ == Command::StimControl &&
        lastSentAction_ == NCLP_STIM_ACTION_DISARM;
    // Keep the safety action available while another command is in flight so
    // the click can be latched.  Disable it after one request is queued/sent.
    disarm_.setEnabled(state_.connected && !disarmRequested_ && !disarmInFlight);
    clear_.setEnabled(current && !state_.outputs.armed() && !state_.outputs.busy());
    resized();
}
String NCLPControlPage::routingDraftDisabledReason() const
{
    if (actionMode_.getSelectedId() < 1 || actionMode_.getSelectedId() > 3 ||
        ttl0_.getSelectedId() < 1 || ttl0_.getSelectedId() > 4 ||
        ttl1_.getSelectedId() < 1 || ttl1_.getSelectedId() > 4)
        return "Select a valid stimulation mode and TTL route.";
    const uint32_t mode = static_cast<uint32_t>(actionMode_.getSelectedId() - 1);
    const uint32_t ttl0 = static_cast<uint32_t>(ttl0_.getSelectedId() - 1);
    const uint32_t ttl1 = static_cast<uint32_t>(ttl1_.getSelectedId() - 1);
    const bool ttl0Stim = ttl0 == TTL_ROUTER_SOURCE_STIM;
    const bool ttl1Stim = ttl1 == TTL_ROUTER_SOURCE_STIM;
    if (ttl0 != 0U && ttl0 == ttl1)
        return "A source can drive only one TTL connector.";
    if (mode == STIM_OUTPUT_MODE_OFF)
        return ttl0Stim || ttl1Stim
            ? "Select Timed TTL pulse or DAC waveform for the Stim trigger pulse route."
            : "Select an output mode and apply routing.";
    if (mode == STIM_OUTPUT_MODE_TTL && !ttl0Stim && !ttl1Stim)
        return "Route Stim trigger pulse to TTL 0 or TTL 1.";
    if (mode == STIM_OUTPUT_MODE_TTL || ttl0Stim || ttl1Stim) {
        double seconds = 0.0;
        if (!readDecimal(ttlDuration_, seconds) || seconds <= 0.0 ||
            seconds > static_cast<double>(maximum) / NCLP_ALGORITHM_CLOCK_HZ)
            return "Enter a valid positive TTL pulse duration.";
    }
    return {};
}

String NCLPControlPage::armDisabledReason() const
{
    const auto& out = state_.outputs;
    if (!state_.connected) return "Connect to the board.";
    if (pending_ || state_.commandBusy) return "Waiting for the current command.";
    if (!out.valid) return "Refresh the board state.";
    if (out.safeOff()) return "Release the board stop button.";
    if (state_.controlState == ControlState::Fault) return "Resolve the board fault before arming.";
    if (out.busy()) return "Waiting for output to finish.";
    if (out.armed()) return "Already armed.";
    if (out.locked()) return "Waiting for output control to finish.";
    if (dirty_ & 32U) return "Apply the recorded TTL changes.";
    const bool routingDirty = (dirty_ & 4U) != 0U;
    const uint32_t requestedMode = routingDirty && actionMode_.getSelectedId() >= 1
        ? static_cast<uint32_t>(actionMode_.getSelectedId() - 1)
        : out.actionMode;
    if (routingDirty) {
        const String reason = routingDraftDisabledReason();
        if (reason.isNotEmpty()) return reason;
    } else {
        const bool ttl0Stim = out.ttl0Source == TTL_ROUTER_SOURCE_STIM;
        const bool ttl1Stim = out.ttl1Source == TTL_ROUTER_SOURCE_STIM;
        const uint32_t stimRouteCount = (ttl0Stim ? 1U : 0U) +
                                        (ttl1Stim ? 1U : 0U);
        if (out.actionMode == STIM_OUTPUT_MODE_OFF)
            return "Select an output mode and apply routing.";
        if (out.actionMode == STIM_OUTPUT_MODE_TTL &&
            (stimRouteCount != 1U || out.ttlWidthCycles == 0U))
            return "Apply one Stim trigger pulse route with a positive TTL duration.";
        if (out.actionMode == STIM_OUTPUT_MODE_DAC &&
            ((stimRouteCount == 0U && out.ttlWidthCycles != 0U) ||
             (stimRouteCount == 1U && out.ttlWidthCycles == 0U) ||
             stimRouteCount > 1U))
            return stimRouteCount == 1U
                ? "Set a positive companion TTL duration and Apply routing."
                : "Apply routing to clear the unused TTL pulse duration.";
    }
    if (requestedMode == STIM_OUTPUT_MODE_DAC) {
        if (dirty_ & 8U) return "Apply the DAC preset changes.";
        if (!(out.stimStatus & STIM_STATUS_DAC_PRIMED))
            return state_.streaming ? "Stop acquisition and apply the DAC preset." : "Apply the DAC preset to prepare output.";
    }
    if (!routingDirty && !(out.stimStatus & STIM_STATUS_CONFIG_VALID))
        return "Apply a valid output configuration.";
    return {};
}
void NCLPControlPage::field(const String& label, Component& component, int x, int y, int width)
{
    component.setName(label);
    if (component.isVisible()) labels_.push_back({label, {x, y, width, 20}});
    component.setBounds(x, y + 23, width, 28);
}
void NCLPControlPage::resized()
{
    labels_.clear(); const int width = getWidth() - 48, half = (width - 20) / 2;
    refresh_.setBounds(getWidth() - 130, 28, 104, 28);
    if (kind_ == Kind::Algorithm) {
        const int third = (width - 64) / 3;
        field("Detected channel (0-based)", detectedChannel_, 44, 144, third);
        field("Filter design", filterKind_, 56 + third, 144, third);
        field("FIR taps", firTaps_, 68 + 2 * third, 144, third);
        field("Low cutoff (Hz)", lowHz_, 44, 208, third);
        field("High cutoff (Hz)", highHz_, 56 + third, 208, third);
        field("PWT window (ms)", windowMs_, 68 + 2 * third, 208, third);
        field("Refractory (ms)", refractoryMs_, 44, 272, third);
        field("Baseline mu (uV)", muUv_, 44, 440, third);
        field("Baseline sigma (uV)", sigmaUv_, 56 + third, 440, third);
        field("K", k_, 68 + 2 * third, 440, third);
        applyRippleK_.setBounds(68 + 2 * third, 512, third, 30);
        detectorEnabled_.setBounds(44, 696, 220, 32);
        if (baselineAnalysisComponent_ != nullptr)
            baselineAnalysisComponent_->setBounds(24, 778, width, 690);
        return;
    }
    const int left = 44, right = 64 + half, fw = (half - 50) / 2, third = (width - 64) / 3;
    field("Sync mode", syncMode_, left, 148, half - 40);
    field("Period / frames", syncPeriod_, left, 212, fw); field("High / frames", syncHigh_, left + fw + 10, 212, fw);
    applySync_.setBounds(left, 292, 130, 30);
    field("Stimulation mode", actionMode_, right, 148, half - 40);
    field("TTL 0 source", ttl0_, right, 212, fw); field("TTL 1 source", ttl1_, right + fw + 10, 212, fw);
    field("TTL pulse duration (s)", ttlDuration_, right, 276, fw); applyRouting_.setBounds(right + fw + 10, 299, fw, 28);
    field("Waveform", shape_, left, 416, third); field("DAC channels", channels_, left + third + 12, 416, third);
    field("Update rate / channel", updateRate_, left + 2 * (third + 12), 416, third);
    field(shape_.getSelectedId() == 1 ? "Frequency (Hz)" : shape_.getSelectedId() == 2 ? "Sigma (s)" : "Hold duration (s)", parameter_, left, 482, third);
    field(shape_.getSelectedId() == 1 ? "Cycles per trigger" : "Pulses per trigger", repeats_, left + third + 12, 482, third);
    field("Duration per trigger (s)", duration_, left + 2 * (third + 12), 482, third);
    const int quarter = (width - 76) / 4;
    if (shape_.getSelectedId() == 3) {
        field("A voltage (V)", minimumA_, left, 582, (width - 52) / 2);
        field("B voltage (V)", minimumB_, left + (width - 52) / 2 + 12, 582, (width - 52) / 2);
    } else {
        field("A minimum (V)", minimumA_, left, 582, quarter); field("A maximum (V)", maximumA_, left + quarter + 12, 582, quarter);
        field("B minimum (V)", minimumB_, left + 2 * (quarter + 12), 582, quarter); field("B maximum (V)", maximumB_, left + 3 * (quarter + 12), 582, quarter);
    }
    field("DAC active / Intan TTL", marker_, left, 650, third);
    applyMarker_.setBounds(left + third + 12, 673, 130, 28);
    loadPreset_.setBounds(left + 2 * (third + 12), 673, third, 32);
    const int shortButtonWidth = (width - 64) / 6;
    const int longButtonWidth = (width - 64 - 2 * shortButtonWidth) / 2;
    int x = left;
    for (auto* button : {&arm_, &disarm_, &trigger_, &clear_}) {
        const int buttonWidth = (button == &arm_ || button == &disarm_) ? shortButtonWidth : longButtonWidth;
        button->setBounds(x, 825, buttonWidth, 32); x += buttonWidth + 8;
    }
}
void NCLPControlPage::card(Graphics& g, Rectangle<int> bounds, const String& title)
{
    g.setColour(panel); g.fillRoundedRectangle(bounds.toFloat(), 9.0f);
    g.setColour(textColour); g.setFont(FontOptions(17.0f).withStyle("Bold"));
    g.drawText(title, bounds.reduced(20).removeFromTop(24), Justification::centredLeft);
}
void NCLPControlPage::paint(Graphics& g)
{
    g.fillAll(background); const int width = getWidth() - 48, half = (width - 20)/2;
    g.setColour(textColour); g.setFont(FontOptions(25.0f).withStyle("Bold"));
    g.drawText(kind_ == Kind::Algorithm ? "Ripple detector" : "Output control", 24, 22, width - 120, 34, Justification::centredLeft);
    const bool current = state_.connected && state_.outputs.valid;
    String connection = !state_.connected ? "Disconnected" : !current ? "Settings unavailable" : pending_ ? "Updating..." : dirty_ ? "Unapplied edits" : "Connected";
    g.setFont(FontOptions(13.0f)); g.setColour(current ? accent : muted); g.drawText(connection, 24, 62, width, 22, Justification::centredLeft);
    const auto& out = state_.outputs;
    if (kind_ == Kind::Algorithm) {
        card(g, {24, 104, width, 280}, "Channel, filter and PWT window");
        card(g, {24, 404, width, 170}, "Threshold settings: mu + K*sigma");
        card(g, {24, 594, width, 160}, "Detection status");
        const String run = !current ? "-" :
            (out.rippleStatus & RIPPLE_DETECTOR_STATUS_FAULT) ? "Fault" :
            out.rippleBusy() ? "Updating" : !out.rippleEnabled() ? "Stopped" :
            (out.rippleStatus & RIPPLE_DETECTOR_STATUS_WAITING_STREAM) ? "Waiting for data" :
            (out.rippleStatus & RIPPLE_DETECTOR_STATUS_WARMUP) ? "Warming up" : "Running";
        g.setFont(FontOptions(14.0f)); g.setColour(textColour);
        g.drawText(run + " | Triggers " + (current ? String(out.triggerCount) : "-") +
            " | Accepted " + (current ? String(out.accepted) : "-") +
            " | Rejected " + (current ? String(out.rejected) : "-"),
            44, 630, width - 40, 24, Justification::centredLeft);

        g.setColour(muted); g.setFont(FontOptions(12.0f));
        String mapping;
        size_t draftIndex = 0;
        if (!state_.scanValid || !state_.topology.valid ||
            state_.topology.channels.empty()) {
            mapping = "Scan headstages before selecting a detected channel.";
        } else if (readDetectedIndex(detectedChannel_, draftIndex) &&
                   draftIndex < state_.topology.channels.size()) {
            const ChannelAddress& channel = state_.topology.channels[draftIndex];
            const uint32_t inputId = (static_cast<uint32_t>(channel.logicalSlot) << 5U) |
                                     channel.amplifierRow;
            mapping = "Detected " + String(static_cast<int>(draftIndex)) + " = " +
                String(laneNameForIndex(channel.physicalIndex)) + " CH" +
                String(channel.localChannel) + " | HLS input " + String(inputId) +
                " (" + String(static_cast<int>(channel.logicalSlot)) + ":" +
                String(static_cast<int>(channel.amplifierRow)) + ")";
        } else {
            mapping = "Detected channel is outside the current scanned topology.";
        }
        size_t appliedDetectedIndex = 0;
        if (current && state_.scanValid && state_.topology.valid &&
            !state_.topology.channels.empty() &&
            !detectedIndexFromReadback(state_.topology,
                                       out.detectedChannelIndex,
                                       appliedDetectedIndex))
            mapping = "Applied channel does not match the current detected topology.";
        g.drawText(current && state_.config.sampleRateHz != 30000U
                ? "Select 30 kS/s to use ripple detection or baseline analysis."
                : mapping,
            44, 350, width - 40, 20, Justification::centredLeft);
        g.drawText("Only K is applied live. Turning the detector on applies every current profile and threshold field first.",
            44, 658, width - 40, 20, Justification::centredLeft);
        g.drawText("Baseline analysis below captures every detected channel privately and never writes mu or sigma.",
            44, 676, width - 40, 20, Justification::centredLeft);
    } else {
        card(g, {24, 104, half, 244}, "Intan synchronization"); card(g, {44 + half, 104, half, 244}, "Stimulation and TTL routing");
        card(g, {24, 368, width, 362}, "DAC preset"); card(g, {24, 750, width, 125}, "Playback");
        g.setFont(FontOptions(13.0f)); g.setColour(muted);
        g.drawText(waveformGuidance(), 44, 544, width - 40, 24, Justification::centredLeft);
        g.setColour(out.safeOff() ? Colour(0xffffb09c) : muted);
        const String prepared = out.actionMode == STIM_OUTPUT_MODE_DAC ? ((out.stimStatus & STIM_STATUS_DAC_PRIMED) ? "Preset ready" : "Apply preset") : "";
        String live = !current ? "Board state unavailable" : String(out.safeOff() ? "Board button held" : out.armed() ? "Armed" : "Disarmed") + " | " + (out.busy() ? "Busy" : "Idle") + " | " + prepared + " | Accepted " + String(out.accepted) + " | Rejected " + String(out.rejected);
        if (current && out.errors) live += " | Errors " + hex(out.errors);
        g.drawText(live, 44, 791, width - 40, 24, Justification::centredLeft);
    }
    g.setColour(muted); g.setFont(FontOptions(12.5f));
    for (const auto& label : labels_) g.drawText(label.text, label.bounds, Justification::centredLeft);
    g.setColour(error_ ? Colour(0xffffb09c) : muted); g.setFont(FontOptions(13.0f));
    String footer = state_.connected && !current && !state_.lastError.empty() ? String(state_.lastError) : feedback_;
    if (kind_ == Kind::Output && !error_ && current && !out.armed()) {
        const auto reason = armDisabledReason();
        if (reason.isNotEmpty()) footer = "Arm: " + reason;
    }
    g.drawFittedText(footer, {24, requiredHeight() - 42, width, 36}, Justification::topLeft, 2);
}
} // namespace nclp
