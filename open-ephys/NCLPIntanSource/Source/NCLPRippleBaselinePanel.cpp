#include "NCLPRippleBaselinePanel.h"

#include "NCLPRippleAnalysis.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace nclp
{
using namespace juce;

namespace
{
const Colour background(0xff161e28);
const Colour panel(0xff202b38);
const Colour textColour(0xffe6edf5);
const Colour muted(0xffa7b6c7);
const Colour accent(0xffa1dfbb);
const Colour errorColour(0xffffb09c);
constexpr double minimumWindowMs = 1.0 / 1000.0;
constexpr double maximumWindowMs = 10.0;

String compact(double value, int decimalPlaces = 6)
{
    if (!std::isfinite(value)) return "-";
    if (value == 0.0) return "0";
    return String(value, decimalPlaces)
        .trimCharactersAtEnd("0")
        .trimCharactersAtEnd(".");
}

bool parseUnsigned(const TextEditor& editor, uint32_t& value)
{
    const std::string input = editor.getText().trim().toStdString();
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    return !input.empty() && parsed.ec == std::errc{} &&
           parsed.ptr == input.data() + input.size();
}

bool parseNonnegativeDecimal(const TextEditor& editor, double& value)
{
    const std::string input = editor.getText().trim().toStdString();
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(),
                                       value, std::chars_format::fixed);
    return !input.empty() && parsed.ec == std::errc{} &&
           parsed.ptr == input.data() + input.size() && std::isfinite(value) &&
           value >= 0.0;
}

String q16Hex(uint32_t value)
{
    return "0x" + String::toHexString(static_cast<int>(value)).paddedLeft('0', 8).toUpperCase();
}

class PsdPlot final : public Component
{
public:
    void setData(std::vector<double> frequencies, std::vector<double> relativeDb)
    {
        frequencies_ = std::move(frequencies);
        relativeDb_ = std::move(relativeDb);
        repaint();
    }

    void clear()
    {
        frequencies_.clear();
        relativeDb_.clear();
        repaint();
    }

    void paint(Graphics& g) override
    {
        g.fillAll(panel.darker(0.18f));
        const Rectangle<float> plot = getLocalBounds().toFloat().reduced(58.0f, 34.0f)
            .withTrimmedTop(10.0f);

        g.setColour(textColour);
        g.setFont(FontOptions(15.0f).withStyle("Bold"));
        g.drawText("Raw-channel PSD", 14, 8, getWidth() - 28, 22,
                   Justification::centredLeft);

        g.setColour(muted);
        g.setFont(FontOptions(11.5f));
        g.drawText("Frequency (Hz, log scale)",
                   roundToInt(plot.getX()), getHeight() - 24,
                   roundToInt(plot.getWidth()), 18, Justification::centred);
        g.saveState();
        g.addTransform(AffineTransform::rotation(-MathConstants<float>::halfPi,
                                                  15.0f,
                                                  plot.getCentreY()));
        g.drawText("Power (dB relative to maximum)",
                   roundToInt(15.0f - plot.getHeight() * 0.5f),
                   roundToInt(plot.getCentreY() - 9.0f),
                   roundToInt(plot.getHeight()), 18,
                   Justification::centred);
        g.restoreState();

        if (frequencies_.size() != relativeDb_.size() || frequencies_.empty())
        {
            g.setColour(muted);
            g.drawText("Collect a baseline to display the normalized spectrum.",
                       plot.toNearestInt(), Justification::centred);
            g.setColour(Colour(0xff415266));
            g.drawRect(plot, 1.0f);
            return;
        }

        double minimumFrequency = std::numeric_limits<double>::infinity();
        double maximumFrequency = 0.0;
        for (double frequency : frequencies_)
        {
            if (std::isfinite(frequency) && frequency > 0.0)
            {
                minimumFrequency = std::min(minimumFrequency, frequency);
                maximumFrequency = std::max(maximumFrequency, frequency);
            }
        }
        if (!std::isfinite(minimumFrequency) || maximumFrequency <= minimumFrequency)
        {
            g.setColour(errorColour);
            g.drawText("PSD frequency data is unavailable.", plot.toNearestInt(),
                       Justification::centred);
            return;
        }

        constexpr double minimumDb = -120.0;
        constexpr double maximumDb = 0.0;
        const double logMinimum = std::log10(minimumFrequency);
        const double logMaximum = std::log10(maximumFrequency);
        const auto xFor = [&](double frequency)
        {
            return plot.getX() + static_cast<float>(
                (std::log10(frequency) - logMinimum) /
                (logMaximum - logMinimum)) * plot.getWidth();
        };
        const auto yFor = [&](double db)
        {
            const double clipped = jlimit(minimumDb, maximumDb, db);
            return plot.getBottom() - static_cast<float>(
                (clipped - minimumDb) / (maximumDb - minimumDb)) * plot.getHeight();
        };

        g.setFont(FontOptions(10.5f));
        for (int db = static_cast<int>(minimumDb); db <= 0; db += 20)
        {
            const float y = yFor(static_cast<double>(db));
            g.setColour(Colour(0xff344353));
            g.drawHorizontalLine(roundToInt(y), plot.getX(), plot.getRight());
            g.setColour(muted);
            g.drawText(String(db), 4, roundToInt(y) - 8, 48, 16,
                       Justification::centredRight);
        }

        const int firstDecade = static_cast<int>(std::floor(logMinimum));
        const int lastDecade = static_cast<int>(std::ceil(logMaximum));
        for (int decade = firstDecade; decade <= lastDecade; ++decade)
        {
            const double scale = std::pow(10.0, static_cast<double>(decade));
            for (int multiplier : {1, 2, 5})
            {
                const double frequency = scale * static_cast<double>(multiplier);
                if (frequency < minimumFrequency || frequency > maximumFrequency)
                    continue;
                const float x = xFor(frequency);
                g.setColour(Colour(0xff344353));
                g.drawVerticalLine(roundToInt(x), plot.getY(), plot.getBottom());
                if (multiplier == 1)
                {
                    g.setColour(muted);
                    const String label = frequency >= 1000.0
                        ? compact(frequency / 1000.0, 1) + "k"
                        : compact(frequency, frequency < 10.0 ? 1 : 0);
                    g.drawText(label, roundToInt(x) - 24,
                               roundToInt(plot.getBottom()) + 3, 48, 15,
                               Justification::centred);
                }
            }
        }

        Path spectrum;
        bool started = false;
        for (size_t index = 0; index < frequencies_.size(); ++index)
        {
            const double frequency = frequencies_[index];
            const double db = relativeDb_[index];
            if (!std::isfinite(frequency) || frequency <= 0.0 ||
                !std::isfinite(db))
                continue;
            const float x = xFor(frequency);
            const float y = yFor(db);
            if (!started)
            {
                spectrum.startNewSubPath(x, y);
                started = true;
            }
            else
            {
                spectrum.lineTo(x, y);
            }
        }
        g.setColour(accent);
        g.strokePath(spectrum, PathStrokeType(1.6f));
        g.setColour(Colour(0xff566a7e));
        g.drawRect(plot, 1.0f);
    }

private:
    std::vector<double> frequencies_;
    std::vector<double> relativeDb_;
};

String baselineUnavailableReason(const SessionSnapshot& snapshot)
{
    if (!snapshot.connected) return "Connect to the board first.";
    if (snapshot.commandBusy) return "Wait for the current board command.";
    if (!snapshot.scanValid || !snapshot.topology.valid ||
        snapshot.topology.channels.empty())
        return "Scan the headstages first.";
    if (!snapshot.initialized) return "Initialize the detected headstages first.";
    if (snapshot.config.sampleRateHz != kRippleAnalysisInputRateHz)
        return "Select 30 kS/s before collecting.";
    if (snapshot.streaming || snapshot.routeUdp || snapshot.listenerRunning ||
        snapshot.controlState == ControlState::Streaming ||
        snapshot.controlState == ControlState::Draining)
        return "Stop Open Ephys acquisition before background collection.";
    if (snapshot.outputs.valid && snapshot.outputs.rippleEnabled())
        return "Disable the ripple detector before background collection.";
    if (snapshot.controlState != ControlState::Ready)
        return "Wait for the board to become ready.";
    return {};
}

bool captureMatchesTopology(const RippleAnalysisCapture& capture,
                            const Topology& topology)
{
    if (!topology.valid ||
        capture.globalChannelByDetectedChannel.size() !=
            topology.channels.size())
        return false;
    for (size_t index = 0; index < topology.channels.size(); ++index)
    {
        if (capture.globalChannelByDetectedChannel[index] !=
            topology.channels[index].globalChannel)
            return false;
    }
    return true;
}
} // namespace

struct NCLPRippleBaselinePanel::Completion
{
    enum class Kind { Capture, ChannelAnalysis } kind = Kind::Capture;
    std::shared_ptr<RippleAnalysisCapture> capture;
    uint32_t detectedChannel = 0U;
    uint64_t connectionGeneration = 0U;
    uint64_t resetGeneration = 0U;
    uint8_t physicalMask = 0U;
    uint16_t logicalMask = 0U;
    uint32_t packedChipIds = 0U;
    uint32_t layoutId = 0U;
    size_t channelCount = 0U;
    RippleAnalysisResult result;
};

class NCLPRippleBaselinePanel::Content final : public Component
{
public:
    explicit Content(NCLPRippleBaselinePanel& owner)
        : owner_(owner), progressBar_(progress_)
    {
        configureEditor(durationSeconds_, "10", "0123456789.");
        configureEditor(firTaps_, "129", "0123456789");
        configureEditor(lowHz_, "150", "0123456789.");
        configureEditor(highHz_, "250", "0123456789.");
        configureEditor(windowMs_, "4", "0123456789.");

        filterKind_.addItem("Minimum-phase FIR", 1);
        filterKind_.addItem("Linear-phase FIR", 2);
        filterKind_.addItem("Fixed Butterworth IIR", 3);
        filterKind_.setSelectedId(1, dontSendNotification);
        filterKind_.setColour(ComboBox::backgroundColourId, background);
        filterKind_.setColour(ComboBox::textColourId, textColour);
        filterKind_.onChange = [this]
        {
            const bool iir = filterKind_.getSelectedId() == 3;
            if (iir)
            {
                lowHz_.setText("150", false);
                highHz_.setText("250", false);
            }
            firTaps_.setEnabled(!iir && !owner_.analysisRunning());
            lowHz_.setEnabled(!iir && !owner_.analysisRunning());
            highHz_.setEnabled(!iir && !owner_.analysisRunning());
            if (!running_) owner_.analysisDraftChanged();
        };
        addAndMakeVisible(filterKind_);

        displayChannel_.setColour(ComboBox::backgroundColourId, background);
        displayChannel_.setColour(ComboBox::textColourId, textColour);
        displayChannel_.setName("Displayed channel");
        displayChannel_.setTooltip(
            "Select which detected channel to display. Collection always records every detected channel.");
        displayChannel_.onChange = [this]
        {
            updateChannelMapping();
            if (!updatingChannels_ && displayChannel_.getSelectedId() > 0)
                owner_.displayChannelSelected(
                    static_cast<uint32_t>(displayChannel_.getSelectedId() - 1));
        };
        addAndMakeVisible(displayChannel_);

        const std::array<String, 6> names = {
            "Displayed channel", "Duration (s)", "Filter design",
            "FIR taps", "Low / high cutoff (Hz)", "PWT window (ms)"
        };
        for (size_t index = 0; index < labels_.size(); ++index)
        {
            labels_[index].setText(names[index], dontSendNotification);
            labels_[index].setColour(Label::textColourId, muted);
            labels_[index].setFont(FontOptions(12.0f));
            addAndMakeVisible(labels_[index]);
        }

        channelMapping_.setColour(Label::textColourId, muted);
        channelMapping_.setFont(FontOptions(12.5f));
        addAndMakeVisible(channelMapping_);

        configureButton(collect_, "Collect all channels", [this] { owner_.startAnalysis(); });
        configureButton(cancel_, "Cancel", [this] { owner_.cancelAnalysis(); });
        cancel_.setEnabled(false);

        status_.setColour(Label::textColourId, muted);
        status_.setFont(FontOptions(13.0f));
        addAndMakeVisible(status_);

        progressBar_.setColour(ProgressBar::foregroundColourId, accent);
        progressBar_.setColour(ProgressBar::backgroundColourId, background);
        addAndMakeVisible(progressBar_);

        for (Label* result : {&mean_, &sigma_, &details_})
        {
            result->setColour(Label::textColourId, textColour);
            result->setFont(FontOptions(result == &details_ ? 12.5f : 14.0f));
            addAndMakeVisible(*result);
        }
        mean_.setText("Mean: -", dontSendNotification);
        sigma_.setText("Sigma: -", dontSendNotification);
        details_.setText("Collection includes every detected channel; only the selected channel is shown. Results are not applied.",
                         dontSendNotification);
        addAndMakeVisible(plot_);
    }

    bool buildRequest(RippleAnalysisRequest& request, String& error) const
    {
        uint32_t channel = 0;
        uint32_t taps = 0;
        double durationSeconds = 0.0;
        double lowHz = 0.0;
        double highHz = 0.0;
        double windowMs = 0.0;

        if (displayChannel_.getSelectedId() <= 0 ||
            !owner_.snapshot_.topology.valid ||
            static_cast<size_t>(displayChannel_.getSelectedId() - 1) >=
                owner_.snapshot_.topology.channels.size())
        {
            error = "Detected channel must identify a scanned amplifier channel.";
            return false;
        }
        channel = static_cast<uint32_t>(displayChannel_.getSelectedId() - 1);
        if (!parseNonnegativeDecimal(durationSeconds_, durationSeconds) ||
            durationSeconds < NCLP_RIPPLE_BASELINE_DURATION_MS_MIN / 1000.0 ||
            durationSeconds > NCLP_RIPPLE_BASELINE_DURATION_MS_MAX / 1000.0)
        {
            error = "Duration must be 1 to 600 seconds.";
            return false;
        }
        const double durationMs = std::round(durationSeconds * 1000.0);
        if (durationMs < NCLP_RIPPLE_BASELINE_DURATION_MS_MIN ||
            durationMs > NCLP_RIPPLE_BASELINE_DURATION_MS_MAX)
        {
            error = "Duration is outside the supported millisecond range.";
            return false;
        }
        if (filterKind_.getSelectedId() < 1 || filterKind_.getSelectedId() > 3)
        {
            error = "Select a valid ripple filter.";
            return false;
        }
        const uint32_t filterKind =
            static_cast<uint32_t>(filterKind_.getSelectedId() - 1);
        if (!parseUnsigned(firTaps_, taps) || taps < NCLP_RIPPLE_FIR_TAPS_MIN ||
            taps > NCLP_RIPPLE_FIR_TAPS_MAX)
        {
            error = "FIR taps must be 3 to 256.";
            return false;
        }
        if (filterKind == NCLP_RIPPLE_FILTER_FIXED_IIR)
        {
            lowHz = NCLP_RIPPLE_FIXED_IIR_LOW_MILLIHZ / 1000.0;
            highHz = NCLP_RIPPLE_FIXED_IIR_HIGH_MILLIHZ / 1000.0;
        }
        else if (!parseNonnegativeDecimal(lowHz_, lowHz) ||
                 !parseNonnegativeDecimal(highHz_, highHz) || lowHz <= 0.0 ||
                 lowHz >= highHz || highHz >= 1500.0)
        {
            error = "Filter band must satisfy 0 < low < high < 1500 Hz.";
            return false;
        }
        if (!parseNonnegativeDecimal(windowMs_, windowMs) ||
            windowMs < minimumWindowMs || windowMs > maximumWindowMs)
        {
            error = "PWT window must be 0.001 to 10 ms.";
            return false;
        }
        const uint32_t windowUs = static_cast<uint32_t>(std::round(windowMs * 1000.0));

        request.detectedChannel = channel;
        request.durationMs = static_cast<uint32_t>(durationMs);
        request.welchFftSize = 4096U;
        request.profile.filterKind = filterKind;
        request.profile.firTaps = taps;
        request.profile.lowMilliHz = static_cast<uint32_t>(std::round(lowHz * 1000.0));
        request.profile.highMilliHz = static_cast<uint32_t>(std::round(highHz * 1000.0));
        request.profile.powerWindowSamples = (windowUs * 3U + 999U) / 1000U;
        return true;
    }

    void setDraft(const RippleAnalysisRequest& draft)
    {
        if (running_) return;
        if (draft.detectedChannel < static_cast<uint32_t>(displayChannel_.getNumItems()))
            displayChannel_.setSelectedId(static_cast<int>(draft.detectedChannel + 1U),
                                          dontSendNotification);
        durationSeconds_.setText(compact(draft.durationMs / 1000.0, 3), false);
        const int filterId = draft.profile.filterKind <= NCLP_RIPPLE_FILTER_FIXED_IIR
            ? static_cast<int>(draft.profile.filterKind + 1U)
            : 1;
        filterKind_.setSelectedId(filterId, dontSendNotification);
        firTaps_.setText(String(draft.profile.firTaps), false);
        if (draft.profile.filterKind == NCLP_RIPPLE_FILTER_FIXED_IIR)
        {
            lowHz_.setText("150", false);
            highHz_.setText("250", false);
        }
        else
        {
            lowHz_.setText(compact(draft.profile.lowMilliHz / 1000.0, 3), false);
            highHz_.setText(compact(draft.profile.highMilliHz / 1000.0, 3), false);
        }
        windowMs_.setText(compact(draft.profile.powerWindowSamples / 3.0, 3), false);
        updateChannelMapping();
        refreshEnabled();
    }

    void setSnapshot(const SessionSnapshot& snapshot)
    {
        snapshotAvailable_ = snapshot.connected;
        updateChannelChoices(snapshot);
        if (!draftInitialized_ && snapshot.outputs.valid)
        {
            RippleAnalysisRequest draft;
            draft.detectedChannel = snapshot.outputs.detectedChannelIndex;
            draft.durationMs = NCLP_RIPPLE_BASELINE_DURATION_MS_DEFAULT;
            draft.profile.filterKind = snapshot.outputs.filterKind;
            draft.profile.firTaps = snapshot.outputs.firTaps;
            draft.profile.lowMilliHz = snapshot.outputs.lowMilliHz;
            draft.profile.highMilliHz = snapshot.outputs.highMilliHz;
            draft.profile.powerWindowSamples =
                (snapshot.outputs.windowUs * 3U + 999U) / 1000U;
            setDraft(draft);
            draftInitialized_ = true;
        }
        updateChannelMapping();
        refreshEnabled();
    }

    void setRunning(bool running)
    {
        running_ = running;
        progress_ = running ? 0.0 : progress_;
        if (running)
        {
            plot_.clear();
            mean_.setText("Mean: -", dontSendNotification);
            sigma_.setText("Sigma: -", dontSendNotification);
            details_.setText("Collecting every detected channel privately; Open Ephys acquisition remains stopped.",
                             dontSendNotification);
            setStatus("Starting background capture...", false);
        }
        refreshEnabled();
    }

    void setProgress(uint64_t completed, uint64_t target)
    {
        progress_ = target == 0U ? -1.0
                                : jlimit(0.0, 1.0,
                                         static_cast<double>(completed) /
                                             static_cast<double>(target));
        if (running_)
        {
            status_.setText(target == 0U
                                ? "Preparing fixed-point filter histories..."
                                : "Collecting all channels: " +
                                      String(static_cast<int64>(completed)) + " / " +
                                      String(static_cast<int64>(target)),
                            dontSendNotification);
        }
    }

    void setResult(const RippleAnalysisResult& result, uint32_t detectedChannel,
                   uint32_t capturedChannelCount)
    {
        progress_ = 1.0;
        mean_.setText("Mean: " + compact(result.meanCounts()) + " counts  |  " +
                          compact(result.meanMicrovolts()) + " uV  |  Q16.16 " +
                          q16Hex(result.meanQ16),
                      dontSendNotification);
        sigma_.setText("Sigma: " + compact(result.sigmaCounts()) + " counts  |  " +
                           compact(result.sigmaMicrovolts()) + " uV  |  Q16.16 " +
                           q16Hex(result.sigmaQ16),
                       dontSendNotification);
        details_.setText("Showing detected channel " + String(detectedChannel) +
                             " among " + String(capturedChannelCount) +
                             " captured channels | Analyzed " +
                             String(static_cast<int64>(result.rawSampleCount)) +
                             " raw samples and " +
                             String(static_cast<int64>(result.amplitudeSampleCount)) +
                             " full-window amplitude samples. Results were not applied.",
                         dontSendNotification);
        plot_.setData(result.frequenciesHz, result.relativePowerDb);
        setStatus("Baseline analysis complete.", false);
    }

    void clearResult(const String& message)
    {
        progress_ = 0.0;
        mean_.setText("Mean: -", dontSendNotification);
        sigma_.setText("Sigma: -", dontSendNotification);
        details_.setText(message, dontSendNotification);
        plot_.clear();
    }

    void setAnalyzing(uint32_t detectedChannel)
    {
        running_ = true;
        clearResult("Analyzing detected channel " + String(detectedChannel) +
                    " with the HLS fixed-point model...");
        setStatus("Calculating mean, sigma and normalized PSD...", false);
        refreshEnabled();
    }

    void setStatus(const String& message, bool error)
    {
        status_.setColour(Label::textColourId, error ? errorColour : muted);
        status_.setText(message, dontSendNotification);
    }

    void resized() override
    {
        const int margin = 22;
        const int gap = 12;
        const int width = getWidth() - 2 * margin;
        const int third = (width - 2 * gap) / 3;
        const int half = (width - gap) / 2;
        const int top = 38;

        labels_[0].setBounds(margin, top + 8, third, 18);
        displayChannel_.setBounds(margin, top + 28, third, 28);
        labels_[1].setBounds(margin + third + gap, top + 8, third, 18);
        durationSeconds_.setBounds(margin + third + gap, top + 28, third, 28);
        labels_[2].setBounds(margin + 2 * (third + gap), top + 8, third, 18);
        filterKind_.setBounds(margin + 2 * (third + gap), top + 28, third, 28);

        labels_[3].setBounds(margin, top + 66, third, 18);
        firTaps_.setBounds(margin, top + 86, third, 28);
        labels_[4].setBounds(margin + third + gap, top + 66, third, 18);
        const int bandWidth = (third - 6) / 2;
        lowHz_.setBounds(margin + third + gap, top + 86, bandWidth, 28);
        highHz_.setBounds(margin + third + gap + bandWidth + 6, top + 86,
                          third - bandWidth - 6, 28);
        labels_[5].setBounds(margin + 2 * (third + gap), top + 66, third, 18);
        windowMs_.setBounds(margin + 2 * (third + gap), top + 86, third, 28);

        channelMapping_.setBounds(margin, top + 120, width, 22);
        collect_.setBounds(margin, top + 148, 172, 30);
        cancel_.setBounds(margin + 184, top + 148, 100, 30);
        progressBar_.setBounds(margin + 296, top + 152, width - 296, 22);
        status_.setBounds(margin, top + 182, width, 24);

        mean_.setBounds(margin, top + 214, half, 26);
        sigma_.setBounds(margin + half + gap, top + 214, half, 26);
        details_.setBounds(margin, top + 242, width, 22);
        plot_.setBounds(margin, top + 274, width, getHeight() - top - 292);
    }

    void paint(Graphics& g) override
    {
        g.fillAll(background);
        g.setColour(textColour);
        g.setFont(FontOptions(18.0f).withStyle("Bold"));
        g.drawText("Baseline analysis - all detected channels", 12, 4,
                   getWidth() - 24, 28, Justification::centredLeft);
        g.setColour(panel);
        g.fillRoundedRectangle(Rectangle<float>(12.0f, 38.0f,
                                                static_cast<float>(getWidth() - 24),
                                                264.0f), 8.0f);
    }

private:
    void configureEditor(TextEditor& editor, const String& value,
                         const String& allowed)
    {
        editor.setText(value, false);
        editor.setInputRestrictions(20, allowed);
        editor.setColour(TextEditor::backgroundColourId, background);
        editor.setColour(TextEditor::textColourId, textColour);
        editor.setColour(TextEditor::outlineColourId, Colour(0xff415266));
        editor.setColour(TextEditor::focusedOutlineColourId, accent);
        addAndMakeVisible(editor);
        editor.onTextChange = [this]
        {
            if (!running_) owner_.analysisDraftChanged();
        };
    }

    void configureButton(TextButton& button, const String& title,
                         std::function<void()> action)
    {
        button.setButtonText(title);
        button.onClick = std::move(action);
        button.setColour(TextButton::buttonColourId, Colour(0xff35495b));
        button.setColour(TextButton::textColourOffId, textColour);
        addAndMakeVisible(button);
    }

    void updateChannelMapping()
    {
        const int selectedId = displayChannel_.getSelectedId();
        if (!owner_.snapshot_.topology.valid ||
            selectedId <= 0 ||
            static_cast<size_t>(selectedId - 1) >= owner_.snapshot_.topology.channels.size())
        {
            channelMapping_.setText(snapshotAvailable_
                                        ? "Select a channel in the detected topology."
                                        : "Board topology is unavailable.",
                                    dontSendNotification);
            return;
        }
        const uint32_t selected = static_cast<uint32_t>(selectedId - 1);
        const ChannelAddress& channel = owner_.snapshot_.topology.channels[selected];
        channelMapping_.setText("Detected " + String(selected) + " = " +
                                    String(laneNameForIndex(channel.physicalIndex)) +
                                    " CH" + String(channel.localChannel),
                                dontSendNotification);
    }

    void refreshEnabled()
    {
        const bool iir = filterKind_.getSelectedId() == 3;
        for (Component* component : std::initializer_list<Component*>{
                 &displayChannel_, &durationSeconds_, &filterKind_, &windowMs_})
            component->setEnabled(!running_);
        firTaps_.setEnabled(!running_ && !iir);
        lowHz_.setEnabled(!running_ && !iir);
        highHz_.setEnabled(!running_ && !iir);
        const String unavailable = baselineUnavailableReason(owner_.snapshot_);
        collect_.setEnabled(!running_ && unavailable.isEmpty());
        collect_.setTooltip(unavailable.isEmpty()
                                ? "Collect privately without starting Open Ephys acquisition."
                                : unavailable);
        cancel_.setEnabled(running_);
    }

    NCLPRippleBaselinePanel& owner_;
    std::array<Label, 6> labels_;
    void updateChannelChoices(const SessionSnapshot& snapshot)
    {
        StringArray labels;
        if (snapshot.topology.valid)
        {
            for (size_t index = 0; index < snapshot.topology.channels.size(); ++index)
            {
                const ChannelAddress& channel = snapshot.topology.channels[index];
                labels.add("Detected " + String(static_cast<int>(index)) + " - " +
                           String(laneNameForIndex(channel.physicalIndex)) + " CH" +
                           String(channel.localChannel));
            }
        }
        bool changed = labels.size() != displayChannel_.getNumItems();
        for (int index = 0; !changed && index < labels.size(); ++index)
            changed = displayChannel_.getItemText(index) != labels[index];
        if (!changed) return;

        const int previousId = displayChannel_.getSelectedId();
        updatingChannels_ = true;
        displayChannel_.clear(dontSendNotification);
        for (int index = 0; index < labels.size(); ++index)
            displayChannel_.addItem(labels[index], index + 1);
        int selectedId = previousId;
        if (selectedId <= 0 || selectedId > labels.size())
        {
            const uint32_t detectorChannel = snapshot.outputs.detectedChannelIndex;
            selectedId = detectorChannel < static_cast<uint32_t>(labels.size())
                ? static_cast<int>(detectorChannel + 1U) : (labels.isEmpty() ? 0 : 1);
        }
        displayChannel_.setSelectedId(selectedId, dontSendNotification);
        updatingChannels_ = false;
    }

    TextEditor durationSeconds_, firTaps_, lowHz_, highHz_, windowMs_;
    ComboBox displayChannel_, filterKind_;
    TextButton collect_, cancel_;
    Label channelMapping_, status_, mean_, sigma_, details_;
    double progress_ = 0.0;
    ProgressBar progressBar_;
    PsdPlot plot_;
    bool snapshotAvailable_ = false;
    bool running_ = false;
    bool updatingChannels_ = false;
    bool draftInitialized_ = false;
};

NCLPRippleBaselinePanel::NCLPRippleBaselinePanel(
    std::shared_ptr<DataThreadPlugin::AsyncAccess> asyncAccess)
    : asyncAccess_(std::move(asyncAccess))
{
    content_ = std::make_unique<Content>(*this);
    addAndMakeVisible(*content_);
    startTimerHz(10);
}

NCLPRippleBaselinePanel::~NCLPRippleBaselinePanel()
{
    stopTimer();
    cancelRequested_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
}

void NCLPRippleBaselinePanel::resized()
{
    if (content_ != nullptr)
        content_->setBounds(getLocalBounds());
}

void NCLPRippleBaselinePanel::setDraft(const RippleAnalysisRequest& draft)
{
    if (content_ != nullptr && !analysisRunning()) content_->setDraft(draft);
}

void NCLPRippleBaselinePanel::setSessionSnapshot(const SessionSnapshot& snapshot)
{
    const bool liveIdentityChanged = snapshot_.connected &&
        (!snapshot.connected ||
         snapshot.connectionGeneration != snapshot_.connectionGeneration ||
         snapshot.resetGeneration != snapshot_.resetGeneration ||
         snapshot.topology.valid != snapshot_.topology.valid ||
         snapshot.topology.physicalMask != snapshot_.topology.physicalMask ||
         snapshot.topology.logicalMask != snapshot_.topology.logicalMask ||
         snapshot.topology.packedChipIds != snapshot_.topology.packedChipIds ||
         snapshot.topology.layoutId != snapshot_.topology.layoutId ||
         snapshot.topology.channels.size() != snapshot_.topology.channels.size());
    const bool invalidatesCapture = capture_ != nullptr &&
        (!snapshot.connected || !snapshot.topology.valid ||
         snapshot.connectionGeneration != capturedConnectionGeneration_ ||
         snapshot.resetGeneration != capturedResetGeneration_ ||
         snapshot.topology.physicalMask != capturedPhysicalMask_ ||
         snapshot.topology.logicalMask != capturedLogicalMask_ ||
         snapshot.topology.packedChipIds != capturedPackedChipIds_ ||
         snapshot.topology.layoutId != capturedLayoutId_ ||
         snapshot.topology.channels.size() != capturedChannelCount_ ||
         !captureMatchesTopology(*capture_, snapshot.topology));
    snapshot_ = snapshot;
    if (analysisRunning() && (!snapshot.connected || liveIdentityChanged ||
                              invalidatesCapture))
        cancelRequested_.store(true, std::memory_order_release);
    if (invalidatesCapture)
    {
        capture_.reset();
        cachedResults_.clear();
        if (!analysisRunning() && content_ != nullptr)
        {
            content_->clearResult(
                "Board identity or detected-channel topology changed. Collect again.");
            content_->setStatus("The retained all-channel baseline was cleared.",
                                false);
        }
    }
    if (content_ != nullptr) content_->setSnapshot(snapshot_);
}

bool NCLPRippleBaselinePanel::analysisRunning() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

void NCLPRippleBaselinePanel::timerCallback()
{
    if (content_ != nullptr && analysisRunning() &&
        capturing_.load(std::memory_order_acquire))
        content_->setProgress(completedSamples_.load(std::memory_order_relaxed),
                              targetSamples_.load(std::memory_order_relaxed));
    joinFinishedWorker();
}

void NCLPRippleBaselinePanel::startAnalysis()
{
    if (analysisRunning() || content_ == nullptr) return;
    const String unavailable = baselineUnavailableReason(snapshot_);
    if (unavailable.isNotEmpty())
    {
        content_->setStatus(unavailable, true);
        return;
    }

    RippleAnalysisRequest request;
    String validationError;
    if (!content_->buildRequest(request, validationError))
    {
        content_->setStatus(validationError, true);
        return;
    }
    joinFinishedWorker();
    if (worker_.joinable())
    {
        content_->setStatus("The previous analysis worker is still finishing.", true);
        return;
    }
    if (asyncAccess_ == nullptr)
    {
        content_->setStatus("The NCLP source is unavailable.", true);
        return;
    }

    cancelRequested_.store(false, std::memory_order_release);
    capture_.reset();
    cachedResults_.clear();
    completedSamples_.store(0U, std::memory_order_relaxed);
    targetSamples_.store(requiredRippleRawSamples(request),
                         std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    capturing_.store(true, std::memory_order_release);
    content_->setRunning(true);

    auto access = asyncAccess_;
    const uint64_t connectionGeneration = snapshot_.connectionGeneration;
    const uint64_t resetGeneration = snapshot_.resetGeneration;
    const uint8_t physicalMask = snapshot_.topology.physicalMask;
    const uint16_t logicalMask = snapshot_.topology.logicalMask;
    const uint32_t packedChipIds = snapshot_.topology.packedChipIds;
    const uint32_t layoutId = snapshot_.topology.layoutId;
    const size_t channelCount = snapshot_.topology.channels.size();
    Component::SafePointer<NCLPRippleBaselinePanel> safeThis(this);
    worker_ = std::thread([this, safeThis, access, request,
                           connectionGeneration, resetGeneration, physicalMask,
                           logicalMask, packedChipIds, layoutId, channelCount]
    {
        auto completion = std::make_shared<Completion>();
        completion->kind = Completion::Kind::Capture;
        completion->detectedChannel = request.detectedChannel;
        completion->connectionGeneration = connectionGeneration;
        completion->resetGeneration = resetGeneration;
        completion->physicalMask = physicalMask;
        completion->logicalMask = logicalMask;
        completion->packedChipIds = packedChipIds;
        completion->layoutId = layoutId;
        completion->channelCount = channelCount;
        completion->capture = std::make_shared<RippleAnalysisCapture>();
        bool succeeded = false;
        const bool sourcePresent = access->run([&](DataThreadPlugin& backend)
        {
            succeeded = backend.collectRippleAnalysis(
                request, *completion->capture,
                [this]
                {
                    return cancelRequested_.load(std::memory_order_acquire);
                },
                [this](uint64_t completed, uint64_t target)
                {
                    completedSamples_.store(completed, std::memory_order_relaxed);
                    targetSamples_.store(target, std::memory_order_relaxed);
                });
        });
        capturing_.store(false, std::memory_order_release);
        if (succeeded && completion->capture->success &&
            !cancelRequested_.load(std::memory_order_acquire))
            completion->result = analyzeRippleCapturedChannel(
                *completion->capture, request.detectedChannel,
                [this]
                {
                    return cancelRequested_.load(std::memory_order_acquire);
                });
        running_.store(false, std::memory_order_release);
        MessageManager::callAsync([safeThis, sourcePresent, succeeded, completion]
        {
            if (safeThis != nullptr)
                safeThis->finishAnalysisOnMessageThread(sourcePresent, succeeded,
                                                        completion);
        });
    });
}

void NCLPRippleBaselinePanel::startChannelAnalysis(uint32_t detectedChannel)
{
    if (analysisRunning() || content_ == nullptr || capture_ == nullptr ||
        !capture_->hasDetectedChannel(detectedChannel))
        return;
    joinFinishedWorker();
    if (worker_.joinable())
    {
        content_->setStatus("The previous analysis worker is still finishing.", true);
        return;
    }

    cancelRequested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    capturing_.store(false, std::memory_order_release);
    content_->setAnalyzing(detectedChannel);
    auto capture = capture_;
    Component::SafePointer<NCLPRippleBaselinePanel> safeThis(this);
    worker_ = std::thread([this, safeThis, capture, detectedChannel]
    {
        auto completion = std::make_shared<Completion>();
        completion->kind = Completion::Kind::ChannelAnalysis;
        completion->capture = capture;
        completion->detectedChannel = detectedChannel;
        completion->result = analyzeRippleCapturedChannel(
            *capture, detectedChannel,
            [this]
            {
                return cancelRequested_.load(std::memory_order_acquire);
            });
        const bool succeeded = completion->result.success;
        running_.store(false, std::memory_order_release);
        MessageManager::callAsync([safeThis, succeeded, completion]
        {
            if (safeThis != nullptr)
                safeThis->finishAnalysisOnMessageThread(true, succeeded, completion);
        });
    });
}

void NCLPRippleBaselinePanel::displayChannelSelected(uint32_t detectedChannel)
{
    if (analysisRunning() || content_ == nullptr) return;
    if (capture_ == nullptr || !capture_->hasDetectedChannel(detectedChannel))
    {
        content_->clearResult(
            "Collect a baseline to capture every channel and display this selection.");
        content_->setStatus("No all-channel baseline is retained.", false);
        return;
    }
    if (detectedChannel < cachedResults_.size() &&
        cachedResults_[detectedChannel] != nullptr)
    {
        content_->setResult(*cachedResults_[detectedChannel], detectedChannel,
                            capture_->detectedChannelCount());
        return;
    }
    startChannelAnalysis(detectedChannel);
}

void NCLPRippleBaselinePanel::analysisDraftChanged()
{
    if (analysisRunning()) return;
    capture_.reset();
    cachedResults_.clear();
    if (content_ != nullptr)
    {
        content_->clearResult(
            "Analysis settings changed. Collect again to refresh every channel.");
        content_->setStatus("Analysis settings changed; collect again.", false);
    }
}

void NCLPRippleBaselinePanel::cancelAnalysis()
{
    if (!analysisRunning()) return;
    cancelRequested_.store(true, std::memory_order_release);
    if (content_ != nullptr)
        content_->setStatus(capturing_.load(std::memory_order_acquire)
                                ? "Cancelling background collection..."
                                : "Cancelling channel analysis...",
                            false);
}

void NCLPRippleBaselinePanel::finishAnalysisOnMessageThread(
    bool sourcePresent, bool succeeded, std::shared_ptr<Completion> completion)
{
    if (content_ == nullptr || completion == nullptr) return;
    content_->setRunning(false);
    const RippleAnalysisResult& result = completion->result;
    const bool capturedAllChannels =
        completion->kind == Completion::Kind::Capture && sourcePresent &&
        succeeded && completion->capture != nullptr &&
        completion->capture->success &&
        snapshot_.connected && snapshot_.topology.valid &&
        snapshot_.connectionGeneration == completion->connectionGeneration &&
        snapshot_.resetGeneration == completion->resetGeneration &&
        snapshot_.topology.physicalMask == completion->physicalMask &&
        snapshot_.topology.logicalMask == completion->logicalMask &&
        snapshot_.topology.packedChipIds == completion->packedChipIds &&
        snapshot_.topology.layoutId == completion->layoutId &&
        snapshot_.topology.channels.size() == completion->channelCount &&
        captureMatchesTopology(*completion->capture, snapshot_.topology);
    if (capturedAllChannels)
    {
        capture_ = completion->capture;
        cachedResults_.assign(capture_->detectedChannelCount(), nullptr);
        capturedConnectionGeneration_ = completion->connectionGeneration;
        capturedResetGeneration_ = completion->resetGeneration;
        capturedPhysicalMask_ = completion->physicalMask;
        capturedLogicalMask_ = completion->logicalMask;
        capturedPackedChipIds_ = completion->packedChipIds;
        capturedLayoutId_ = completion->layoutId;
        capturedChannelCount_ = completion->channelCount;
    }
    if (!sourcePresent)
        content_->setStatus("The NCLP source was removed during collection.", true);
    else if (result.cancelled || cancelRequested_.load(std::memory_order_acquire))
        content_->setStatus("Baseline analysis cancelled.", false);
    else if (completion->kind == Completion::Kind::Capture &&
             (!succeeded || completion->capture == nullptr ||
              !completion->capture->success))
        content_->setStatus(
            completion->capture == nullptr || completion->capture->error.empty()
                ? "All-channel baseline capture failed."
                : String(completion->capture->error),
            true);
    else if (!succeeded || !result.success)
        content_->setStatus(result.error.empty()
                                ? "Baseline analysis failed."
                                : String(result.error),
                            true);
    else
    {
        if (capture_ != nullptr &&
            completion->detectedChannel < cachedResults_.size())
            cachedResults_[completion->detectedChannel] =
                std::make_shared<RippleAnalysisResult>(result);
        content_->setResult(result, completion->detectedChannel,
                            capture_ == nullptr ? 0U :
                                capture_->detectedChannelCount());
    }
    content_->setSnapshot(snapshot_);
    joinFinishedWorker();
}

void NCLPRippleBaselinePanel::joinFinishedWorker()
{
    if (worker_.joinable() && !analysisRunning()) worker_.join();
}

} // namespace nclp
