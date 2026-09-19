#include "NCLPDataThread.h"

#include "NCLPEditor.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <unordered_map>

namespace nclp
{
namespace
{
constexpr int kSourceBufferCapacitySamples = 10000;
// juce::AbstractFifo intentionally keeps one slot empty to distinguish full
// from empty, so a DataBuffer constructed with N slots can hold only N - 1.
constexpr int kSourceBufferUsableSamples = kSourceBufferCapacitySamples - 1;
}

DataThreadPlugin::AsyncAccess::AsyncAccess(DataThreadPlugin* owner)
    : owner_(owner)
{
}

bool DataThreadPlugin::AsyncAccess::run(
    const std::function<void(DataThreadPlugin&)>& work)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ == nullptr)
        return false;

    work(*owner_);
    return true;
}

bool DataThreadPlugin::AsyncAccess::tryRun(
    const std::function<void(DataThreadPlugin&)>& work)
{
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (! lock.owns_lock() || owner_ == nullptr)
        return false;

    work(*owner_);
    return true;
}

void DataThreadPlugin::AsyncAccess::invalidateAndWait()
{
    // Taking this mutex waits for an active UI command. Once owner_ is null,
    // queued workers wake up without touching the destroyed DataThreadPlugin.
    std::lock_guard<std::mutex> lock(mutex_);
    owner_ = nullptr;
}

DataThreadPlugin::DataThreadPlugin(SourceNode* sourceNode)
    : DataThread(sourceNode),
      asyncAccess_(std::shared_ptr<AsyncAccess>(new AsyncAccess(this)))
{
}

DataThreadPlugin::~DataThreadPlugin()
{
    // collectRippleAnalysis() runs synchronously under AsyncAccess::run. Ask
    // it to unwind before waiting for that lifetime gate, otherwise removing
    // the source during a long capture would block until the full duration.
    shuttingDown_ = true;
    rippleAnalysisCancelRequested_ = true;
    asyncAccess_->invalidateAndWait();
    stopAcquisition();
    (void) settleHardwareStop(true);
    disconnectDevice();
}

void DataThreadPlugin::registerParameters()
{
    addStringParameter(Parameter::PROCESSOR_SCOPE,
                       "fpga_ip",
                       "FPGA IP",
                       "NCLP FPGA TCP control and UDP source address",
                       kDefaultFpgaIp,
                       true);
    addStringParameter(Parameter::PROCESSOR_SCOPE,
                       "host_ip",
                       "Host IP",
                       "Local IPv4 address for UDP data; use auto or 0.0.0.0 to infer the route",
                       kDefaultHostIp,
                       true);
}

void DataThreadPlugin::updateConfigFromParameters()
{
    if (hasParameter("fpga_ip"))
        fpgaIp_ = getParameter("fpga_ip")->getValueAsString().trim().toStdString();
    if (hasParameter("host_ip"))
        hostIp_ = getParameter("host_ip")->getValueAsString().trim().toStdString();

    SessionConfig config = session_.configSnapshot();
    if (config.fpgaIp == fpgaIp_ && config.hostIp == hostIp_)
        return;

    config.fpgaIp = fpgaIp_;
    config.hostIp = hostIp_;
    std::string ignored;
    (void) session_.updateConfig(config, &ignored);
}

void DataThreadPlugin::parameterValueChanged(Parameter*)
{
    updateConfigFromParameters();
}

bool DataThreadPlugin::foundInputSource()
{
    return session_.snapshot().connected;
}

bool DataThreadPlugin::isReady()
{
    const SessionSnapshot snapshot = session_.snapshot();
    const SampleMode sampleMode = snapshot.sampleMode;
    const size_t expectedChannels = continuousChannelCount(snapshot.topology,
                                                            sampleMode);
    return snapshot.connected && snapshot.scanValid && snapshot.topology.valid &&
           snapshot.topology.totalChannels > 0 && snapshot.initialized &&
           snapshot.controlState == ControlState::Ready &&
           ! snapshot.streaming && ! snapshot.routeUdp &&
           ! snapshot.impedanceRunning && ! snapshot.commandBusy &&
           ! hardwareStopInProgress_.load() &&
           publishedChannelCount_.load() == expectedChannels &&
           publishedSampleRateHz_.load() == snapshot.config.sampleRateHz &&
           publishedTopologyMatches(snapshot.topology, sampleMode);
}

std::unique_ptr<GenericEditor> DataThreadPlugin::createEditor(SourceNode* sourceNode)
{
    auto editor = std::make_unique<NCLPEditor>(sourceNode, this);
    editor->updateSnapshot(session_.snapshot());
    editor->setStatusText(lastStatusText(), false);
    return editor;
}

bool DataThreadPlugin::connectDevice(bool printOutput)
{
    updateConfigFromParameters();
    std::string error;
    const bool connected = session_.connect(&error);
    if (connected && ! acquisitionRunning_.load() &&
        streamOwner_.load() != StreamOwner::RippleAnalysis)
    {
        if (session_.snapshot().streaming)
            session_.stopStreaming();
        const SessionSnapshot stopped = session_.snapshot();
        if (! stopped.streaming && ! stopped.routeUdp &&
            ! stopped.listenerRunning)
            releaseStream(StreamOwner::OpenEphys);
    }
    setLastStatus(connected ? formatStatusSnapshot(session_.snapshot())
                            : String("Connect failed: ") + error,
                  printOutput);
    return connected;
}

bool DataThreadPlugin::reconnectDevice(bool printOutput)
{
    updateConfigFromParameters();
    std::string error;
    const bool connected = session_.reconnect(&error);
    // A successful recovery may rediscover a firmware stream after Open Ephys
    // acquisition was already stopped. Preserve a genuinely live recovery,
    // but stop an orphaned board stream before reporting readiness.
    if (connected && ! acquisitionRunning_.load() &&
        streamOwner_.load() != StreamOwner::RippleAnalysis)
    {
        if (session_.snapshot().streaming)
            session_.stopStreaming();
        const SessionSnapshot stopped = session_.snapshot();
        if (! stopped.streaming && ! stopped.routeUdp &&
            ! stopped.listenerRunning)
            releaseStream(StreamOwner::OpenEphys);
    }
    setLastStatus(connected ? formatStatusSnapshot(session_.snapshot())
                            : String("Reconnect failed: ") + error,
                  printOutput);
    return connected;
}

void DataThreadPlugin::disconnectDevice()
{
    rippleAnalysisCancelRequested_ = true;
    if (acquisitionRunning_)
        stopAcquisition();
    (void) settleHardwareStop(true);
    session_.disconnect();
    streamOwner_ = StreamOwner::None;
    setLastStatus("Disconnected", false);
}

String DataThreadPlugin::checkConnection()
{
    std::string error;
    const bool ok = session_.checkConnection(&error);
    const String status = ok ? String("Check OK | ") + formatStatusSnapshot(session_.snapshot())
                             : String("Check failed: ") + error;
    setLastStatus(status, false);
    return status;
}

String DataThreadPlugin::resetDevice()
{
    std::string error;
    const bool ok = !acquisitionRunning_.load() && session_.reset(&error);
    const String status = ok ? "Reset complete. Rescan to initialize headstages."
                            : String("Reset failed: ") + (error.empty() ? "Stop acquisition first." : error);
    setLastStatus(status, false);
    return status;
}

String DataThreadPlugin::scanDevice(
    const ScanProgressCallback& progressCallback)
{
    std::string error;
    if (! session_.scan(&error, progressCallback))
    {
        const String status = String("SCAN failed: ") + error;
        setLastStatus(status, false);
        return status;
    }

    SessionSnapshot snapshot = session_.snapshot();
    if (snapshot.topology.totalChannels > 0 && ! session_.init(&error))
    {
        const String status = String("SCAN succeeded, INIT failed: ") + error;
        setLastStatus(status, false);
        return status;
    }

    snapshot = session_.snapshot();
    const String status = snapshot.topology.totalChannels == 0
        ? "Scan complete | No RHD headstages detected"
        : String("Scan complete | ") + formatStatusSnapshot(snapshot);
    setLastStatus(status, false);
    return status;
}

String DataThreadPlugin::initDevice()
{
    std::string error;
    const bool ok = session_.init(&error);
    const String status = ok ? formatStatusSnapshot(session_.snapshot())
                             : String("INIT failed: ") + error;
    setLastStatus(status, false);
    return status;
}

String DataThreadPlugin::setSampleRate(
    uint32_t sampleRateHz,
    const ScanProgressCallback& progressCallback)
{
    std::string error;
    if (! session_.setSampleRate(sampleRateHz, &error))
    {
        const String status = String("SET_RATE failed: ") + error;
        setLastStatus(status, false);
        return status;
    }

    // The firmware deliberately invalidates delay calibration when the sample
    // rate changes. Re-scan immediately so Open Ephys receives the new dynamic
    // channel map instead of retaining stale topology.
    if (! session_.scan(&error, progressCallback))
    {
        const String status = String("Rate changed, rescan failed: ") + error;
        setLastStatus(status, false);
        return status;
    }
    SessionSnapshot snapshot = session_.snapshot();
    if (snapshot.topology.totalChannels > 0 && ! session_.init(&error))
    {
        const String status = String("Rate changed and rescanned, INIT failed: ") + error;
        setLastStatus(status, false);
        return status;
    }

    const String status = "Sample rate " + String(static_cast<int>(sampleRateHz)) +
                          " Hz | " + formatStatusSnapshot(session_.snapshot());
    setLastStatus(status, false);
    return status;
}

String DataThreadPlugin::setBandwidth(uint32_t analogLowerMilliHz,
                                      uint32_t analogUpperHz)
{
    std::string error;
    if (! session_.setBandwidth(analogLowerMilliHz, analogUpperHz, &error))
    {
        const String status = String("SET_BANDWIDTH failed: ") + error;
        setLastStatus(status, false);
        return status;
    }

    // SET_BANDWIDTH persists the requested values but deliberately invalidates
    // INIT. Apply the new RHD register image immediately when a usable scanned
    // topology exists, instead of leaving the UI value out of sync with the
    // physical chips until the next acquisition or impedance measurement.
    const SessionSnapshot afterSet = session_.snapshot();
    if (afterSet.scanValid && afterSet.topology.valid &&
        afterSet.topology.totalChannels > 0 && ! session_.init(&error))
    {
        const String status = String("Analog bandwidth saved, but INIT failed: ") + error;
        setLastStatus(status, false);
        return status;
    }

    const String status = String("Analog bandwidth updated | ") +
                          formatStatusSnapshot(session_.snapshot());
    setLastStatus(status, false);
    return status;
}

String DataThreadPlugin::setDsp(bool enabled, uint32_t requestedCutoffMilliHz)
{
    std::string error;
    if (! session_.setDsp(enabled, requestedCutoffMilliHz, &error))
    {
        const String status = String("SET_DSP failed: ") + error;
        setLastStatus(status, false);
        return status;
    }

    // Like bandwidth, DSP configuration is only committed to the headstage by
    // INIT. Keep the stopped-state editor truthful by applying it now.
    const SessionSnapshot afterSet = session_.snapshot();
    if (afterSet.scanValid && afterSet.topology.valid &&
        afterSet.topology.totalChannels > 0 && ! session_.init(&error))
    {
        const String status = String("DSP setting saved, but INIT failed: ") + error;
        setLastStatus(status, false);
        return status;
    }

    const String status = String("DSP high-pass ") +
                          (enabled ? "enabled" : "disabled") + " | " +
                          formatStatusSnapshot(session_.snapshot());
    setLastStatus(status, false);
    return status;
}

String DataThreadPlugin::setTtlFastSettle(bool enabled, uint32_t channel)
{
    std::string error;
    const bool ok = session_.setTtlFastSettle(enabled, channel, &error);
    const String status = ok
        ? String("TTL fast settle ") +
              (enabled ? String("uses TTL") + String(static_cast<int>(channel))
                       : String("disabled")) +
              " | " + formatStatusSnapshot(session_.snapshot())
        : String("SET_TTL_SETTLE failed: ") + error;
    setLastStatus(status, false);
    return status;
}

String DataThreadPlugin::setAuxChannelsEnabled(bool enabled)
{
    const SessionSnapshot snapshot = session_.snapshot();
    if (acquisitionRunning_.load() || snapshot.streaming || snapshot.routeUdp ||
        snapshot.commandBusy || snapshot.impedanceRunning ||
        hardwareStopInProgress_.load())
    {
        const String status =
            "Can't change AUX channels while acquisition or another board operation is active";
        setLastStatus(status, false);
        return status;
    }

    SessionConfig config = session_.configSnapshot();
    const SampleMode requestedMode = enabled ? SampleMode::Aux : SampleMode::Vdd;
    if (config.sampleMode == requestedMode)
    {
        const String status = enabled
            ? "AUX channels already enabled (3 per headstage)"
            : "AUX channels already disabled (amplifier channels only)";
        setLastStatus(status, false);
        return status;
    }

    config.sampleMode = requestedMode;
    std::string error;
    if (! session_.updateConfig(config, &error))
    {
        const String status = String("Could not change AUX channels: ") + error;
        setLastStatus(status, false);
        return status;
    }

    const String status = enabled
        ? "AUX channels enabled (3 per headstage)"
        : "AUX channels disabled (amplifier channels only)";
    setLastStatus(status, false);
    return status;
}

bool DataThreadPlugin::auxChannelsEnabled() const
{
    return session_.configSnapshot().sampleMode == SampleMode::Aux;
}

String DataThreadPlugin::readOutputs()
{
    std::string error;
    return session_.readOutputs(&error) ? String{} : String(error);
}

void DataThreadPlugin::pollOutputStatus()
{
    session_.pollOutputStatus();
}

String DataThreadPlugin::sendOutputCommand(Command command, const CommandArgs& args)
{
    std::string error;
    const bool ok = session_.sendOutputCommand(command, args, &error);
    const String status = ok ? "Output command applied" : String(error);
    setLastStatus(status, true);
    return ok ? String{} : String(error);
}

String DataThreadPlugin::runImpedance()
{
    std::string error;
    const bool ok = session_.runImpedance(&error);
    const String status = ok ? "Impedance measurement started"
                             : String("IMPEDANCE failed: ") + error;
    setLastStatus(status, false);
    return status;
}

String DataThreadPlugin::cancelImpedance()
{
    std::string error;
    const bool ok = session_.cancelImpedance(&error);
    const String status = ok ? "Impedance cancellation requested"
                             : String("CANCEL failed: ") + error;
    setLastStatus(status, false);
    return status;
}

bool DataThreadPlugin::collectRippleAnalysis(
    const RippleAnalysisRequest& request,
    RippleAnalysisCapture& capture,
    const std::function<bool()>& cancel,
    const std::function<void(uint64_t, uint64_t)>& progress)
{
    rippleAnalysisCancelRequested_ = false;
    capture = {};
    capture.request = request;

    std::string validationError;
    const uint64_t targetRawSamples =
        requiredRippleRawSamples(request, &validationError);
    if (targetRawSamples == 0U)
    {
        capture.error = validationError.empty()
            ? "Ripple analysis request is invalid"
            : validationError;
        return false;
    }
    if (targetRawSamples >
        static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    {
        capture.error = "Ripple analysis capture is too large for this host";
        return false;
    }

    if (! acquireStream(StreamOwner::RippleAnalysis))
    {
        capture.error = "The NCLP stream is already owned by Open Ephys or another analysis";
        return false;
    }
    // Reserve ownership before inspecting the STOP worker. This closes the
    // gap where an Open Ephys graph-stop callback could otherwise launch a
    // board STOP between the check and the private capture reservation.
    if (! settleHardwareStop(false))
    {
        capture.error = "The previous board STOP is still finishing";
        releaseStream(StreamOwner::RippleAnalysis);
        return false;
    }

    const auto cancelled = [&]() noexcept
    {
        if (shuttingDown_.load() || rippleAnalysisCancelRequested_.load())
            return true;
        if (! cancel)
            return false;
        try
        {
            return cancel();
        }
        catch (...)
        {
            // A throwing UI callback is treated as cancellation so hardware
            // cleanup still follows the normal private-stream path.
            return true;
        }
    };
    const auto reportProgress = [&](uint64_t current) noexcept
    {
        if (! progress)
            return;
        try
        {
            progress(current, targetRawSamples);
        }
        catch (...)
        {
            // Progress is advisory; never strand a hardware stream because a
            // presentation callback failed.
        }
    };
    const auto sameTopology = [](const Topology& first,
                                 const Topology& second)
    {
        return first.valid == second.valid &&
               first.physicalMask == second.physicalMask &&
               first.logicalMask == second.logicalMask &&
               first.packedChipIds == second.packedChipIds &&
               first.layoutId == second.layoutId &&
               first.totalChannels == second.totalChannels;
    };

    SessionSnapshot before = session_.snapshot();
    if (acquisitionRunning_.load() || isThreadRunning())
        capture.error = "Stop Open Ephys acquisition before collecting baseline";
    else if (! before.connected)
        capture.error = "Connect to the NCLP board before collecting baseline";
    else if (before.commandBusy || before.impedanceRunning ||
             before.cancelRequested)
        capture.error = "Another board operation is active";
    else if (! before.scanValid || ! before.topology.valid ||
             before.topology.channels.empty())
        capture.error = "Scan and detect at least one amplifier channel first";
    else if (! before.initialized || before.controlState != ControlState::Ready)
        capture.error = "Initialize the detected headstages before collecting baseline";
    else if (before.config.sampleRateHz != kRippleAnalysisInputRateHz)
        capture.error = "Ripple analysis requires exactly 30 kS/s";
    else if (before.streaming || before.routeUdp || before.listenerRunning)
        capture.error = "A board stream is already active";
    else if (request.detectedChannel >= before.topology.channels.size())
        capture.error = "Detected channel is outside the scanned topology";

    if (! capture.error.empty())
    {
        releaseStream(StreamOwner::RippleAnalysis);
        return false;
    }

    // Starting the private sample producer also feeds the hardware ripple
    // path. Verify the detector is stopped authoritatively so a background
    // analysis can never emit stimulation triggers.
    std::string outputStatusError;
    if (! session_.pollOutputStatus(&outputStatusError))
    {
        capture.error = outputStatusError.empty()
            ? "Could not verify that the ripple detector is stopped"
            : "Could not verify that the ripple detector is stopped: " +
                  outputStatusError;
        releaseStream(StreamOwner::RippleAnalysis);
        return false;
    }
    before = session_.snapshot();
    if (! before.outputs.valid || before.outputs.rippleEnabled() ||
        before.outputs.rippleBusy() || before.outputs.baselineCollecting())
    {
        capture.error = ! before.outputs.valid
            ? "Ripple detector status is unavailable"
            : "Stop the ripple detector before collecting baseline";
        releaseStream(StreamOwner::RippleAnalysis);
        return false;
    }

    const uint64_t connectionGeneration = before.connectionGeneration;
    const uint64_t resetGeneration = before.resetGeneration;
    const Topology pinnedTopology = before.topology;
    const size_t detectedChannelCount = pinnedTopology.channels.size();
    if (! pinnedTopology.valid || detectedChannelCount == 0U ||
        request.detectedChannel >= detectedChannelCount)
    {
        capture.error =
            "Detected channel topology changed before baseline capture";
        releaseStream(StreamOwner::RippleAnalysis);
        return false;
    }
    std::string storageError;
    if (detectedChannelCount > std::numeric_limits<uint32_t>::max() ||
        requiredRippleCaptureBytes(
            request, static_cast<uint32_t>(detectedChannelCount),
            &storageError) == 0U)
    {
        capture.error = storageError.empty()
            ? "The detected channel count is too large for baseline capture"
            : storageError;
        releaseStream(StreamOwner::RippleAnalysis);
        return false;
    }

    capture.globalChannelByDetectedChannel.reserve(detectedChannelCount);
    std::array<bool, kMaxElectrodeChannels> mappedGlobalChannels{};
    for (const ChannelAddress& address : pinnedTopology.channels)
    {
        const size_t globalChannel = static_cast<size_t>(address.globalChannel);
        if (globalChannel >= pinnedTopology.totalChannels ||
            globalChannel >= kMaxElectrodeChannels ||
            mappedGlobalChannels[globalChannel])
        {
            capture.error =
                "Detected channels have an invalid amplifier mapping";
            releaseStream(StreamOwner::RippleAnalysis);
            return false;
        }
        mappedGlobalChannels[globalChannel] = true;
        capture.globalChannelByDetectedChannel.push_back(
            static_cast<uint32_t>(globalChannel));
    }

    std::vector<QueuedFrame> frames;
    uint64_t rawFramesCaptured = 0U;
    bool privateStreamMayBeActive = false;
    bool captureComplete = false;

    try
    {
        capture.samplesByDetectedChannel.resize(detectedChannelCount);
        for (auto& channelSamples : capture.samplesByDetectedChannel)
            channelSamples.reserve(static_cast<size_t>(targetRawSamples));
        reportProgress(0U);

        if (cancelled())
        {
            capture.cancelled = true;
            capture.error = "Ripple analysis cancelled";
        }
        else
        {
            std::string startError;
            const bool started = session_.startStreaming(
                std::chrono::milliseconds(5000), &startError);
            const SessionSnapshot afterStart = session_.snapshot();
            privateStreamMayBeActive = started || afterStart.streaming ||
                                       afterStart.routeUdp ||
                                       afterStart.listenerRunning;
            if (! started)
            {
                capture.error = startError.empty()
                    ? "Could not start the private baseline stream"
                    : "Could not start the private baseline stream: " +
                          startError;
            }
            else if (afterStart.connectionGeneration != connectionGeneration ||
                     afterStart.resetGeneration != resetGeneration ||
                     afterStart.config.sampleRateHz !=
                         kRippleAnalysisInputRateHz ||
                     ! sameTopology(afterStart.topology, pinnedTopology))
            {
                capture.error =
                    "Board identity or channel topology changed while starting baseline capture";
            }
            else if (afterStart.queueOverruns != 0U)
            {
                capture.error = "The baseline capture queue overran while starting";
            }
            else
            {
                bool haveTimestamp = false;
                uint64_t lastTimestamp = 0U;
                while (rawFramesCaptured < targetRawSamples &&
                       capture.error.empty() && ! cancelled())
                {
                    const uint64_t remaining =
                        targetRawSamples - rawFramesCaptured;
                    const size_t maximumFrames = static_cast<size_t>(
                        std::min<uint64_t>(remaining, 2048U));
                    const size_t minimumFrames =
                        std::min<size_t>(maximumFrames, 256U);
                    const size_t count = session_.waitAndDrainFrames(
                        frames, minimumFrames, maximumFrames,
                        std::chrono::milliseconds(100));

                    std::string fatalError;
                    if (session_.consumeFatalError(&fatalError))
                    {
                        capture.error = fatalError.empty()
                            ? "The UDP baseline stream failed"
                            : fatalError;
                        break;
                    }

                    if (count == 0U)
                    {
                        const SessionSnapshot current = session_.snapshot();
                        if (! current.connected || ! current.streaming ||
                            ! current.routeUdp || ! current.listenerRunning)
                            capture.error =
                                "The private baseline stream stopped before capture completed";
                        continue;
                    }

                    for (const QueuedFrame& frame : frames)
                    {
                        if (frame.channelCount < pinnedTopology.totalChannels ||
                            frame.missingBefore != 0U ||
                            frame.timestampMissingBefore != 0U ||
                            (haveTimestamp &&
                             frame.timestamp != lastTimestamp + 1U))
                        {
                            capture.error =
                                "A UDP sample gap occurred during baseline capture";
                            break;
                        }
                        for (size_t detected = 0U;
                             detected < detectedChannelCount; ++detected)
                        {
                            const size_t globalChannel =
                                capture.globalChannelByDetectedChannel[detected];
                            capture.samplesByDetectedChannel[detected].push_back(
                                frame.amplifierCounts[globalChannel]);
                        }
                        ++rawFramesCaptured;
                        haveTimestamp = true;
                        lastTimestamp = frame.timestamp;
                    }

                    const SessionSnapshot current = session_.snapshot();
                    if (current.connectionGeneration != connectionGeneration ||
                        current.resetGeneration != resetGeneration ||
                        current.config.sampleRateHz !=
                            kRippleAnalysisInputRateHz ||
                        ! sameTopology(current.topology, pinnedTopology) ||
                        current.queueOverruns != 0U)
                    {
                        capture.error = current.queueOverruns != 0U
                            ? "The baseline capture queue overran"
                            : "Board identity or channel topology changed during baseline capture";
                    }
                    reportProgress(rawFramesCaptured);
                }

                if (cancelled() && capture.error.empty())
                {
                    capture.cancelled = true;
                    capture.error = "Ripple analysis cancelled";
                }
                captureComplete = rawFramesCaptured == targetRawSamples &&
                                  capture.error.empty() &&
                                  ! capture.cancelled;
            }
        }
    }
    catch (const std::exception& exception)
    {
        capture.error = std::string("Ripple capture failed: ") +
                       exception.what();
    }
    catch (...)
    {
        capture.error = "Ripple capture failed unexpectedly";
    }

    // Only this call can own RippleAnalysis, so it stops exactly the stream it
    // attempted to start. Open Ephys start is excluded by the owner token.
    if (privateStreamMayBeActive)
    {
        session_.stopStreaming();
        const SessionSnapshot stopped = session_.snapshot();
        if (stopped.streaming || stopped.routeUdp || stopped.listenerRunning)
        {
            if (! capture.error.empty())
                capture.error += "; ";
            capture.error +=
                "Board STOP after baseline capture is unconfirmed; use Reconnect";
            captureComplete = false;
        }
    }
    releaseStream(StreamOwner::RippleAnalysis);
    reportProgress(rawFramesCaptured);

    capture.rawSampleCount = rawFramesCaptured;
    if (! captureComplete)
        return false;
    capture.success = true;
    return true;
}

bool DataThreadPlugin::collectRippleAnalysis(
    const RippleAnalysisRequest& request,
    RippleAnalysisResult& result,
    const std::function<bool()>& cancel,
    const std::function<void(uint64_t, uint64_t)>& progress)
{
    RippleAnalysisCapture capture;
    if (! collectRippleAnalysis(request, capture, cancel, progress))
    {
        result = {};
        result.cancelled = capture.cancelled;
        result.error = capture.error;
        result.rawSampleCount = capture.rawSampleCount;
        return false;
    }

    const auto cancelled = [&]() noexcept
    {
        if (shuttingDown_.load() || rippleAnalysisCancelRequested_.load())
            return true;
        if (! cancel)
            return false;
        try
        {
            return cancel();
        }
        catch (...)
        {
            return true;
        }
    };
    result = analyzeRippleCapturedChannel(
        capture, request.detectedChannel, cancelled);
    return result.success;
}

String DataThreadPlugin::setChannelNamingScheme(int scheme, bool updateSignalChain)
{
    if (scheme != 1 && scheme != 2)
    {
        const String status = "Channel naming scheme must be Global or Stream-Based";
        setLastStatus(status, false);
        return status;
    }

    const SessionSnapshot snapshot = session_.snapshot();
    if (acquisitionRunning_.load() || snapshot.streaming)
    {
        const String status = "Can't change channel names while acquisition is active";
        setLastStatus(status, false);
        return status;
    }

    const bool changed = channelNamingScheme_.exchange(scheme) != scheme;
    const String status = String("Channel names: ") +
                          (scheme == 1 ? "Global" : "Stream-Based");
    setLastStatus(status, false);
    if (changed && updateSignalChain && sn != nullptr)
        CoreServices::updateSignalChain(sn);
    return status;
}

bool DataThreadPlugin::saveImpedances(const File& file, String* error) const
{
    const SessionSnapshot snapshot = session_.snapshot();
    if (snapshot.impedanceResults.empty())
    {
        if (error != nullptr)
            *error = "No impedance results are available";
        return false;
    }

    if (file.hasFileExtension("xml"))
    {
        XmlElement xml("IMPEDANCES");
        const int namingScheme = channelNamingScheme();
        for (const HeadstageDescriptor& headstage : snapshot.topology.headstages)
        {
            XmlElement* headstageXml = xml.createNewChildElement("HEADSTAGE");
            headstageXml->setAttribute("name", laneNameForIndex(headstage.physicalIndex));
            headstageXml->setAttribute("chip", chipTypeName(headstage.chip));

            for (const ImpedanceRecord& record : snapshot.impedanceResults)
            {
                if (record.physicalIndex != headstage.physicalIndex)
                    continue;

                ChannelAddress address{};
                address.globalChannel = record.globalChannel;
                address.physicalIndex = record.physicalIndex;
                address.localChannel = record.localChannel;

                XmlElement* channelXml = headstageXml->createNewChildElement("CHANNEL");
                channelXml->setAttribute("name", channelName(address, namingScheme));
                channelXml->setAttribute("number", static_cast<int>(record.globalChannel));
                channelXml->setAttribute(
                    "magnitude",
                    static_cast<double>(record.magnitudeMilliohms) / 1000.0);
                channelXml->setAttribute(
                    "phase",
                    static_cast<double>(record.phaseMicrodegrees) / 1.0e6);
                channelXml->setAttribute("valid", record.valid);
                channelXml->setAttribute("timestamp_error", record.timestampError);
                channelXml->setAttribute("verify_error", record.verifyError);
                channelXml->setAttribute("saturated", record.saturated);
            }
        }

        if (! file.replaceWithText(xml.toString()))
        {
            if (error != nullptr)
                *error = "Could not write " + file.getFullPathName();
            return false;
        }
        if (error != nullptr)
            error->clear();
        return true;
    }

    const int namingScheme = channelNamingScheme();
    String csv = "global_channel,channel_name,port,headstage,chip,local_channel,magnitude_ohms,phase_degrees,valid,timestamp_error,verify_error,saturated\n";
    for (const ImpedanceRecord& record : snapshot.impedanceResults)
    {
        ChipType chip = ChipType::None;
        if (record.physicalIndex < snapshot.topology.lanes.size())
            chip = snapshot.topology.lanes[record.physicalIndex].chip;
        const char port = static_cast<char>('A' + record.physicalIndex / 2U);
        ChannelAddress address{};
        address.globalChannel = record.globalChannel;
        address.physicalIndex = record.physicalIndex;
        address.localChannel = record.localChannel;
        csv += String(static_cast<int>(record.globalChannel + 1U)) + "," +
               channelName(address, namingScheme) + "," +
               String::charToString(static_cast<juce_wchar>(port)) + "," +
               String(laneNameForIndex(record.physicalIndex)) + "," +
               String(chipTypeName(chip)) + "," +
               String(static_cast<int>(record.localChannel + 1U)) + "," +
               String(static_cast<double>(record.magnitudeMilliohms) / 1000.0, 3) + "," +
               String(static_cast<double>(record.phaseMicrodegrees) / 1.0e6, 6) + "," +
               String(record.valid ? 1 : 0) + "," +
               String(record.timestampError ? 1 : 0) + "," +
               String(record.verifyError ? 1 : 0) + "," +
               String(record.saturated ? 1 : 0) + "\n";
    }

    if (! file.replaceWithText(csv))
    {
        if (error != nullptr)
            *error = "Could not write " + file.getFullPathName();
        return false;
    }
    if (error != nullptr)
        error->clear();
    return true;
}

bool DataThreadPlugin::startAcquisition()
{
    // A completed worker remains joinable until the next lifecycle operation.
    // Reap it here, but reject an immediate restart instead of blocking the
    // Open Ephys graph thread while firmware is still completing STOP.
    if (! settleHardwareStop(false))
    {
        rejectAcquisitionStart(
            "NCLP cannot start: the previous board STOP is still finishing");
        return false;
    }

    if (! acquireStream(StreamOwner::OpenEphys))
    {
        rejectAcquisitionStart(
            "NCLP cannot start: the stream is reserved for background ripple analysis");
        return false;
    }

    updateConfigFromParameters();
    const SessionSnapshot before = session_.snapshot();
    const SampleMode sampleMode = before.sampleMode;
    const size_t expectedChannels = continuousChannelCount(before.topology,
                                                            sampleMode);
    if (before.streaming || before.routeUdp)
    {
        releaseStream(StreamOwner::OpenEphys);
        rejectAcquisitionStart(
            "NCLP cannot start: the previous board stream is still active; "
            "use Reconnect to recover it");
        return false;
    }
    if (! before.initialized || before.controlState != ControlState::Ready)
    {
        releaseStream(StreamOwner::OpenEphys);
        rejectAcquisitionStart(
            "NCLP cannot start: rescan and initialize the headstages first");
        return false;
    }
    if (! before.topology.valid || before.topology.totalChannels == 0 ||
        publishedChannelCount_.load() != expectedChannels ||
        publishedSampleRateHz_.load() != before.config.sampleRateHz ||
        ! publishedTopologyMatches(before.topology, sampleMode))
    {
        releaseStream(StreamOwner::OpenEphys);
        rejectAcquisitionStart(
            "NCLP cannot start: rescan and let the signal chain update first");
        return false;
    }

    // stopAcquisition() deliberately leaves the source FIFO untouched while
    // Open Ephys removes its audio callback. The prior lifecycle worker has
    // now finished, so this is the safe point to discard any previous run.
    {
        std::lock_guard<std::mutex> lock(sourceBufferMutex_);
        for (auto* buffer : sourceBuffers)
            buffer->clear();
    }
    hasBufferedTimestamp_ = false;
    openEphysFifoSamples_.store(0, std::memory_order_relaxed);
    openEphysFifoHighWater_.store(0, std::memory_order_relaxed);
    dataGapEvents_.store(0, std::memory_order_relaxed);
    lastDataGapMissingSamples_.store(0, std::memory_order_relaxed);
    lastDataGapBeforeTimestamp_.store(0, std::memory_order_relaxed);
    lastGapStatusTime_ = {};

    std::string error;
    if (! session_.startStreaming(std::chrono::milliseconds(5000), &error))
    {
        releaseStream(StreamOwner::OpenEphys);
        rejectAcquisitionStart("NCLP stream start failed: " + error);
        return false;
    }

    lastBufferedTimestamp_ = 0;
    acquisitionRunning_ = true;
    if (! startThread())
    {
        acquisitionRunning_ = false;
        beginHardwareStop(true);
        rejectAcquisitionStart("NCLP stream start failed: could not launch the data thread");
        return false;
    }
    publishStatus("NCLP stream started | hardware timestamps preserved");
    const SessionSnapshot started = session_.snapshot();
    if (! started.receiveBufferWarning.empty())
        publishStatus("NCLP WARNING: " + started.receiveBufferWarning);
    return true;
}

bool DataThreadPlugin::stopAcquisition()
{
    const bool wasRunning = acquisitionRunning_.exchange(false);
    if (isThreadRunning())
        signalThreadShouldExit();

    // Open Ephys graph lifecycle callbacks can occur while a modeless
    // baseline panel owns its private stream. They must not STOP that stream.
    if (streamOwner_.load() == StreamOwner::RippleAnalysis)
        return true;

    // Never wait on Open Ephys' message/graph thread. On a decoder failure the
    // DataThread itself can synchronously re-enter this method while holding a
    // MessageManagerLock; waiting here would either self-stop or deadlock. A
    // lifecycle-owned worker waits for the producer to exit, then performs the
    // full board STOP/READY handshake and stale-listener cleanup.
    const SessionSnapshot beforeStop = session_.snapshot();
    beginHardwareStop(wasRunning || beforeStop.streaming || beforeStop.routeUdp);
    return true;
}

void DataThreadPlugin::beginHardwareStop(bool reportStatus)
{
    std::lock_guard<std::mutex> lock(hardwareStopMutex_);

    if (hardwareStopThread_.joinable())
    {
        if (hardwareStopInProgress_.load())
            return;
        hardwareStopThread_.join();
    }

    hardwareStopInProgress_ = true;
    if (reportStatus)
        publishStatus("NCLP local acquisition stopped | finishing board STOP...");

    hardwareStopThread_ = std::thread([this, reportStatus]
    {
        // This wait is intentionally off JUCE's message thread. updateBuffer()
        // is non-blocking apart from a 1 ms idle sleep, and the self-error path
        // can unwind as soon as stopAcquisition() returns.
        if (isThreadRunning())
            (void) waitForThreadToExit(-1);

        session_.stopStreaming();
        const SessionSnapshot afterStop = session_.snapshot();

        if (! afterStop.streaming && ! afterStop.routeUdp &&
            ! afterStop.listenerRunning)
            releaseStream(StreamOwner::OpenEphys);

        if (reportStatus)
        {
            if (afterStop.streaming || afterStop.routeUdp)
            {
                std::string message =
                    "NCLP local acquisition stopped; firmware STOP is unconfirmed";
                if (! afterStop.lastError.empty())
                    message += ": " + afterStop.lastError;
                publishStatus(message);
            }
            else
            {
                publishStatus("NCLP stream stopped");
            }
        }
        hardwareStopInProgress_ = false;
    });
}

bool DataThreadPlugin::settleHardwareStop(bool waitForCompletion)
{
    std::lock_guard<std::mutex> lock(hardwareStopMutex_);
    if (! hardwareStopThread_.joinable())
        return true;
    if (! waitForCompletion && hardwareStopInProgress_.load())
        return false;

    hardwareStopThread_.join();
    return true;
}

bool DataThreadPlugin::acquireStream(StreamOwner owner)
{
    StreamOwner expected = StreamOwner::None;
    return streamOwner_.compare_exchange_strong(expected, owner);
}

void DataThreadPlugin::releaseStream(StreamOwner owner)
{
    StreamOwner expected = owner;
    (void) streamOwner_.compare_exchange_strong(expected, StreamOwner::None);
}

void DataThreadPlugin::updateSettings(OwnedArray<ContinuousChannel>* continuousChannels,
                                      OwnedArray<EventChannel>* eventChannels,
                                      OwnedArray<SpikeChannel>* spikeChannels,
                                      OwnedArray<DataStream>* sourceStreams,
                                      OwnedArray<DeviceInfo>* devices,
                                      OwnedArray<ConfigurationObject>* configurationObjects)
{
    continuousChannels->clear();
    eventChannels->clear();
    spikeChannels->clear();
    sourceStreams->clear();
    devices->clear();
    configurationObjects->clear();

    const SessionSnapshot snapshot = session_.snapshot();
    if (! snapshot.connected || ! snapshot.scanValid || ! snapshot.topology.valid ||
        snapshot.topology.totalChannels == 0 || snapshot.config.sampleRateHz == 0)
    {
        clearPublishedTopology();
        publishedChannelCount_ = 0;
        publishedSampleRateHz_ = 0;
        return;
    }

    DeviceInfo::Settings deviceSettings;
    deviceSettings.name = "NCLP KR260";
    deviceSettings.description = "NCLP Ethernet Intan acquisition board";
    deviceSettings.identifier = "nclp.kr260";
    deviceSettings.manufacturer = "NCLP";
    deviceSettings.serial_number = String(session_.configSnapshot().fpgaIp);
    auto* device = new DeviceInfo(deviceSettings);
    devices->add(device);

    DataStream::Settings streamSettings {
        "NCLP Intan Data",
        "Synchronous amplifier, auxiliary-input, and TTL data from all detected RHD headstages",
        "nclp.intan.data",
        static_cast<float>(snapshot.config.sampleRateHz),
        true
    };
    auto* stream = new DataStream(streamSettings);
    stream->device = device;
    sourceStreams->add(stream);

    std::unordered_map<uint32_t, const ImpedanceRecord*> impedanceByChannel;
    for (const ImpedanceRecord& record : snapshot.impedanceResults)
        impedanceByChannel[record.globalChannel] = &record;

    for (const ChannelAddress& address : snapshot.topology.channels)
    {
        const String name = channelName(address, channelNamingScheme());
        ContinuousChannel::Settings channelSettings {
            ContinuousChannel::ELECTRODE,
            name,
            "NCLP Intan amplifier channel",
            "nclp.intan.ch" + String(static_cast<int>(address.globalChannel + 1U)),
            kDefaultBitVolts,
            stream
        };
        auto* channel = new ContinuousChannel(channelSettings);
        channel->setUnits("uV");

        const auto found = impedanceByChannel.find(static_cast<uint32_t>(address.globalChannel));
        if (found != impedanceByChannel.end())
        {
            const ImpedanceRecord& impedance = *found->second;
            if (impedance.valid)
            {
                channel->impedance.measured = true;
                channel->impedance.magnitude =
                    static_cast<float>(static_cast<double>(impedance.magnitudeMilliohms) / 1000.0);
                channel->impedance.phase =
                    static_cast<float>(static_cast<double>(impedance.phaseMicrodegrees) / 1.0e6);
            }
        }
        continuousChannels->add(channel);
    }

    if (snapshot.sampleMode == SampleMode::Aux)
    {
        // Preserve the exact signed ADC count. AUX inputs are unipolar on the
        // wire, so calling the centered value "volts" would hide the +32768
        // offset needed to recover the physical 0..2.45 V measurement.
        const ContinuousChannel::InputRange auxInputRange {
            -32768.0f,
             32767.0f
        };
        for (const AuxChannelAddress& address : snapshot.topology.auxChannels)
        {
            const String name = auxChannelName(address);
            ContinuousChannel::Settings channelSettings {
                ContinuousChannel::AUX,
                name,
                "NCLP Intan auxiliary ADC input",
                String("nclp.intan.aux.hs") +
                    String(static_cast<int>(address.physicalIndex + 1U)) +
                    ".input" +
                    String(static_cast<int>(address.auxInput + 1U)),
                kAuxBitVolts,
                stream
            };
            auto* channel = new ContinuousChannel(channelSettings);
            channel->setUnits("a.u.");
            channel->inputRange = auxInputRange;
            continuousChannels->add(channel);
        }
    }

    EventChannel::Settings eventSettings {
        EventChannel::TTL,
        "NCLP TTL Input",
        "16-bit TTL state sampled with each NCLP frame",
        "nclp.intan.ttl",
        stream,
        16
    };
    eventChannels->add(new EventChannel(eventSettings));

    publishedChannelCount_ = continuousChannelCount(snapshot.topology,
                                                     snapshot.sampleMode);
    publishedSampleRateHz_ = snapshot.config.sampleRateHz;
    setPublishedTopology(snapshot.topology, snapshot.sampleMode);
}

void DataThreadPlugin::resizeBuffers()
{
    std::lock_guard<std::mutex> lock(sourceBufferMutex_);
    sourceBuffers.clear();
    const int channelCount = static_cast<int>(publishedChannelCount_.load());
    if (channelCount > 0)
        sourceBuffers.add(new DataBuffer(channelCount, kSourceBufferCapacitySamples));
    openEphysFifoSamples_.store(0, std::memory_order_relaxed);
    openEphysFifoHighWater_.store(0, std::memory_order_relaxed);
}

bool DataThreadPlugin::updateBuffer()
{
    std::string fatalError;
    if (session_.consumeFatalError(&fatalError))
    {
        acquisitionRunning_ = false;
        publishStatus("NCLP stream stopped: " + fatalError);
        return false;
    }
    if (! acquisitionRunning_)
        return true;

    const size_t channelCount = publishedChannelCount_.load();
    const uint32_t sampleRateHz = publishedSampleRateHz_.load();
    if (channelCount == 0 || sampleRateHz == 0)
    {
        publishStatus("NCLP stream topology is not published in the signal chain");
        return false;
    }

    // Preserve the smooth delivery behaviour of the earlier NCLP build: move
    // whatever the UDP listener has decoded into Open Ephys as soon as it is
    // available. Twenty milliseconds is an upper bound for one transfer, not
    // a threshold that must be reached before publishing. Waiting for exactly
    // 20 ms of frames makes Linux UDP scheduling jitter visible to the LFP
    // Viewer and can publish queued blocks back-to-back after a short stall.
    const size_t maxTransferFrames = std::max<size_t>(
        1U, (static_cast<size_t>(sampleRateHz) + 49U) / 50U);

    // Keep decoded frames in the larger Session ring if Open Ephys is briefly
    // behind, and never remove more frames than its FIFO can accept.
    int bufferedSamples = 0;
    bool bufferUnavailable = false;
    {
        std::lock_guard<std::mutex> lock(sourceBufferMutex_);
        if (sourceBuffers.isEmpty())
            bufferUnavailable = true;
        else
            bufferedSamples = sourceBuffers[0]->getNumSamples();
    }
    if (bufferUnavailable)
    {
        publishStatus("NCLP stream buffer is not available in the signal chain");
        return false;
    }
    recordOpenEphysFifoDepth(static_cast<size_t>(std::max(0, bufferedSamples)));
    const int freeSamples = std::max(
        0, kSourceBufferUsableSamples - bufferedSamples);
    if (freeSamples == 0)
    {
        Thread::sleep(1);
        return true;
    }

    const size_t maxFrames = std::min(
        maxTransferFrames, static_cast<size_t>(freeSamples));
    const size_t count = session_.drainFrames(frameBatch_, maxFrames);

    // A listener failure may have occurred before or while frames were drained.
    // Do not publish queued samples after a fatal protocol error.
    if (session_.consumeFatalError(&fatalError))
    {
        acquisitionRunning_ = false;
        publishStatus("NCLP stream stopped: " + fatalError);
        return false;
    }
    if (! acquisitionRunning_)
        return true;
    if (count == 0)
    {
        // DataThread::run() calls updateBuffer continuously. Avoid spinning
        // while retaining approximately 1 ms delivery granularity.
        Thread::sleep(1);
        return true;
    }

    for (const QueuedFrame& frame : frameBatch_)
    {
        if (frame.channelCount != channelCount)
        {
            publishStatus("NCLP decoder channel count changed during streaming");
            return false;
        }
    }

    // Open Ephys 1.1 stores a sample number for every FIFO item but exposes
    // only the first number of each processing block. Before crossing a
    // hardware timestamp gap, let the consumer empty the FIFO. The next block
    // then starts at the real post-gap sample number without fabricating NaNs
    // or compressing the discontinuity into the previous block.
    size_t cursor = 0;
    while (cursor < count)
    {
        const bool discontinuity = hasBufferedTimestamp_ &&
            (frameBatch_[cursor].timestamp != lastBufferedTimestamp_ + 1U ||
             frameBatch_[cursor].missingBefore != 0U ||
             frameBatch_[cursor].timestampMissingBefore != 0U);
        if (discontinuity)
        {
            const auto drainDeadline = std::chrono::steady_clock::now() +
                                       std::chrono::milliseconds(100);
            while (acquisitionRunning_.load())
            {
                int pendingSamples = 0;
                bool bufferDisappeared = false;
                {
                    std::lock_guard<std::mutex> lock(sourceBufferMutex_);
                    if (! acquisitionRunning_.load())
                        return true;
                    if (sourceBuffers.isEmpty())
                        bufferDisappeared = true;
                    else
                        pendingSamples = sourceBuffers[0]->getNumSamples();
                }
                if (bufferDisappeared)
                {
                    publishStatus(
                        "NCLP stream buffer disappeared during acquisition");
                    return false;
                }
                recordOpenEphysFifoDepth(
                    static_cast<size_t>(std::max(0, pendingSamples)));
                if (pendingSamples == 0)
                    break;
                if (std::chrono::steady_clock::now() >= drainDeadline)
                {
                    session_.recordConsumerDrops(count - cursor);
                    acquisitionRunning_ = false;
                    publishStatus(
                        "NCLP stream stopped: Open Ephys FIFO did not drain "
                        "before a hardware timestamp gap");
                    return false;
                }
                Thread::sleep(1);
            }
            if (! acquisitionRunning_.load())
                return true;
        }

        size_t end = cursor + 1U;
        while (end < count && frameBatch_[end].missingBefore == 0U &&
               frameBatch_[end].timestampMissingBefore == 0U &&
               frameBatch_[end].timestamp == frameBatch_[end - 1U].timestamp + 1U)
            ++end;
        const size_t segmentCount = end - cursor;

        sampleBlock_.resize(channelCount * segmentCount);
        sampleNumbers_.resize(segmentCount);
        timestamps_.resize(segmentCount);
        eventCodes_.resize(segmentCount);
        for (size_t sample = 0; sample < segmentCount; ++sample)
        {
            const QueuedFrame& frame = frameBatch_[cursor + sample];
            for (size_t channel = 0; channel < channelCount; ++channel)
                sampleBlock_[channel * segmentCount + sample] = frame.samples[channel];
            sampleNumbers_[sample] = static_cast<int64>(frame.timestamp);
            timestamps_[sample] = static_cast<double>(frame.timestamp) /
                                  static_cast<double>(sampleRateHz);
            eventCodes_[sample] = static_cast<uint64>(frame.ttl);
        }

        int written = 0;
        int fifoSamplesAfterWrite = 0;
        bool bufferDisappeared = false;
        {
            std::lock_guard<std::mutex> lock(sourceBufferMutex_);
            // Stop may have raced with the CPU work above. Do not write into a
            // newly rebuilt graph buffer after local acquisition has ended.
            if (! acquisitionRunning_.load())
                return true;
            if (sourceBuffers.isEmpty())
                bufferDisappeared = true;
            else
            {
                written = sourceBuffers[0]->addToBuffer(
                    sampleBlock_.data(), sampleNumbers_.data(), timestamps_.data(),
                    eventCodes_.data(), static_cast<int>(segmentCount));
                fifoSamplesAfterWrite = sourceBuffers[0]->getNumSamples();
            }
        }
        if (bufferDisappeared)
        {
            publishStatus("NCLP stream buffer disappeared during acquisition");
            return false;
        }
        recordOpenEphysFifoDepth(
            static_cast<size_t>(std::max(0, fifoSamplesAfterWrite)));
        if (written > 0)
        {
            if (discontinuity)
                reportDataGap(frameBatch_[cursor]);
            lastBufferedTimestamp_ =
                frameBatch_[cursor + static_cast<size_t>(written) - 1U].timestamp;
            hasBufferedTimestamp_ = true;
        }
        if (written < static_cast<int>(segmentCount))
            session_.recordConsumerDrops(
                segmentCount - static_cast<size_t>(std::max(0, written)));
        cursor = end;
    }
    return true;
}

String DataThreadPlugin::handleConfigMessage(const String& message)
{
    const String command = message.trim();
    if (command.equalsIgnoreCase("connect"))
    {
        (void) connectDevice(false);
        return lastStatusText();
    }
    if (command.equalsIgnoreCase("reconnect"))
    {
        (void) reconnectDevice(false);
        return lastStatusText();
    }
    if (command.equalsIgnoreCase("disconnect"))
    {
        disconnectDevice();
        return lastStatusText();
    }
    if (command.equalsIgnoreCase("ping") || command.equalsIgnoreCase("status") ||
        command.equalsIgnoreCase("check"))
        return checkConnection();
    if (command.equalsIgnoreCase("scan") || command.equalsIgnoreCase("rescan"))
        return scanDevice();
    if (command.equalsIgnoreCase("reset"))
        return resetDevice();
    if (command.equalsIgnoreCase("init"))
        return initDevice();
    return "Unknown NCLP config message: " + command;
}

bool DataThreadPlugin::isCommandBusy() const
{
    return session_.isCommandBusy();
}

String DataThreadPlugin::lastStatusText() const
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    return lastStatus_;
}

SessionSnapshot DataThreadPlugin::sessionSnapshot() const
{
    SessionSnapshot snapshot = session_.snapshot();
    size_t currentFifoSamples =
        openEphysFifoSamples_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(sourceBufferMutex_);
        if (! sourceBuffers.isEmpty())
        {
            currentFifoSamples = static_cast<size_t>(
                std::max(0, sourceBuffers[0]->getNumSamples()));
        }
    }
    snapshot.openEphysFifoSamples = currentFifoSamples;
    snapshot.openEphysFifoHighWater =
        openEphysFifoHighWater_.load(std::memory_order_relaxed);
    snapshot.openEphysFifoCapacitySamples = kSourceBufferUsableSamples;
    snapshot.dataGapEvents = dataGapEvents_.load(std::memory_order_relaxed);
    snapshot.lastDataGapMissingSamples =
        lastDataGapMissingSamples_.load(std::memory_order_relaxed);
    snapshot.lastDataGapBeforeTimestamp =
        lastDataGapBeforeTimestamp_.load(std::memory_order_relaxed);
    return snapshot;
}

void DataThreadPlugin::setLastStatus(const String& message, bool broadcast)
{
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        lastStatus_ = message;
    }
    if (broadcast && message.isNotEmpty())
        CoreServices::sendStatusMessage(message);
}

void DataThreadPlugin::rejectAcquisitionStart(const std::string& message)
{
    acquisitionRunning_ = false;
    publishStatus(message);

    // SourceNode currently discards the bool returned by DataThread::startAcquisition.
    // Defer the global stop until the current start pass has completed so the GUI
    // cannot claim to be acquiring when this hardware source never started.
    MessageManager::callAsync([]
    {
        if (CoreServices::getAcquisitionStatus())
            CoreServices::setAcquisitionStatus(false);
    });
}

void DataThreadPlugin::publishStatus(const std::string& message)
{
    setLastStatus(String(message), true);
}

void DataThreadPlugin::recordOpenEphysFifoDepth(size_t samples)
{
    openEphysFifoSamples_.store(samples, std::memory_order_relaxed);
    size_t highWater = openEphysFifoHighWater_.load(std::memory_order_relaxed);
    while (highWater < samples &&
           ! openEphysFifoHighWater_.compare_exchange_weak(
               highWater, samples, std::memory_order_relaxed))
    {
    }
}

void DataThreadPlugin::reportDataGap(const QueuedFrame& frame)
{
    const uint64_t expectedTimestamp = lastBufferedTimestamp_ + 1U;
    const uint64_t observedTimestampMissing =
        frame.timestamp > expectedTimestamp
            ? frame.timestamp - expectedTimestamp
            : 0U;
    const uint64_t reportedMissing = std::max<uint64_t>(
        static_cast<uint64_t>(frame.missingBefore),
        frame.timestampMissingBefore);
    const uint64_t missing = std::max(observedTimestampMissing,
                                      reportedMissing);
    const uint64_t gapEvents =
        dataGapEvents_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    lastDataGapMissingSamples_.store(missing, std::memory_order_relaxed);
    lastDataGapBeforeTimestamp_.store(frame.timestamp,
                                      std::memory_order_relaxed);

    // Keep every gap in counters/tooltip, but throttle status-bar/log messages
    // so a degraded link cannot amplify its own loss by flooding the GUI.
    const auto now = std::chrono::steady_clock::now();
    if (lastGapStatusTime_.time_since_epoch().count() != 0 &&
        now - lastGapStatusTime_ < std::chrono::seconds(1))
    {
        return;
    }
    lastGapStatusTime_ = now;

    std::string message = "NCLP DATA GAP: ";
    if (missing == 0U)
        message += "timestamp discontinuity";
    else
        message += std::to_string(missing) + " sample(s) missing";
    message += " before hardware timestamp " +
               std::to_string(frame.timestamp) +
               " | gap events " + std::to_string(gapEvents);
    publishStatus(message);
}

String DataThreadPlugin::formatStatusSnapshot(const SessionSnapshot& snapshot) const
{
    if (! snapshot.lastError.empty())
        return String(snapshot.lastError);
    if (! snapshot.connected)
        return "Disconnected";

    String status = snapshot.streaming ? "Streaming" :
                    snapshot.impedanceRunning ? "Measuring impedances" :
                    snapshot.initialized ? "Ready" :
                    snapshot.scanValid ? "Scanned" : "Connected";
    if (snapshot.config.sampleRateHz > 0)
        status += " | " + String(static_cast<int>(snapshot.config.sampleRateHz)) + " Hz";
    if (snapshot.topology.valid)
    {
        status += " | " +
                  String(static_cast<int>(snapshot.topology.totalChannels)) +
                  " amp";
        if (snapshot.sampleMode == SampleMode::Aux &&
            snapshot.topology.totalAuxChannels > 0U)
            status += " + " +
                      String(static_cast<int>(snapshot.topology.totalAuxChannels)) +
                      " aux";
    }
    return status;
}

String DataThreadPlugin::channelName(const ChannelAddress& channel, int namingScheme)
{
    if (namingScheme == 1)
        return "CH" + String(static_cast<int>(channel.globalChannel + 1U));

    return String(laneNameForIndex(channel.physicalIndex)) + "_CH" +
           String(static_cast<int>(channel.localChannel + 1U));
}

String DataThreadPlugin::auxChannelName(const AuxChannelAddress& channel)
{
    return String(laneNameForIndex(channel.physicalIndex)) + "_AUX" +
           String(static_cast<int>(channel.auxInput + 1U));
}

bool DataThreadPlugin::publishedTopologyMatches(const Topology& topology,
                                                SampleMode sampleMode) const
{
    std::lock_guard<std::mutex> lock(publishedTopologyMutex_);
    // layoutId is a per-scan packet generation, not an Open Ephys channel-map
    // identity: firmware advances it even when the same chips are rediscovered.
    // startStreaming() still gives the current layoutId to the UDP parser.
    return publishedTopology_.valid && topology.valid &&
           publishedTopology_.physicalMask == topology.physicalMask &&
           publishedTopology_.logicalMask == topology.logicalMask &&
           publishedTopology_.packedChipIds == topology.packedChipIds &&
           publishedTopology_.totalChannels == topology.totalChannels &&
           publishedTopology_.totalAuxChannels == topology.totalAuxChannels &&
           publishedTopology_.sampleMode == sampleMode;
}

void DataThreadPlugin::clearPublishedTopology()
{
    std::lock_guard<std::mutex> lock(publishedTopologyMutex_);
    publishedTopology_ = {};
}

void DataThreadPlugin::setPublishedTopology(const Topology& topology,
                                            SampleMode sampleMode)
{
    std::lock_guard<std::mutex> lock(publishedTopologyMutex_);
    publishedTopology_.physicalMask = topology.physicalMask;
    publishedTopology_.logicalMask = topology.logicalMask;
    publishedTopology_.packedChipIds = topology.packedChipIds;
    publishedTopology_.totalChannels = topology.totalChannels;
    publishedTopology_.totalAuxChannels = topology.totalAuxChannels;
    publishedTopology_.sampleMode = sampleMode;
    publishedTopology_.valid = topology.valid;
}

} // namespace nclp
