#include "NCLPSession.h"
#include "NCLPSessionSupport.h"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace nclp
{
using namespace session_detail;


Session::Session()
    : commandClient_(sessionConfig_.fpgaIp, sessionConfig_.hostIp)
{
    frameQueue_.resize(sessionConfig_.queueCapacityFrames);
    snapshot_.requestedReceiveBufferBytes = sessionConfig_.receiveBufferBytes;
    snapshot_.queueCapacityFrames = frameQueue_.size();
}

Session::~Session()
{
    stopStreaming();
    disconnect();
}

bool Session::updateConfig(const SessionConfig& config, std::string* error)
{
    if ((config.sampleMode != SampleMode::Aux &&
         config.sampleMode != SampleMode::Vdd) ||
        config.dataPort == 0 || config.queueCapacityFrames == 0 ||
        config.receiveBufferBytes <= 0)
    {
        setError("Invalid session sample-mode, socket, or queue configuration", error);
        return false;
    }

    bool endpointChanged = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (snapshot_.streaming || commandBusy_.load())
        {
            const std::string message = "Network settings are locked while an operation is active";
            snapshot_.lastError = message;
            if (error != nullptr)
                *error = message;
            return false;
        }
        endpointChanged = config.fpgaIp != sessionConfig_.fpgaIp ||
                          config.hostIp != sessionConfig_.hostIp;
        const bool queueSizeChanged =
            config.queueCapacityFrames != sessionConfig_.queueCapacityFrames;
        sessionConfig_ = config;
        snapshot_.requestedReceiveBufferBytes = config.receiveBufferBytes;
        snapshot_.kernelReportedReceiveBufferBytes = 0;
        snapshot_.actualReceiveBufferBytes = 0;
        snapshot_.receiveBufferSetSucceeded = false;
        snapshot_.receiveBufferQuerySucceeded = false;
        snapshot_.receiveBufferClamped = false;
        snapshot_.receiveBufferWarning.clear();
        if (queueSizeChanged)
        {
            frameQueue_.clear();
            frameQueue_.resize(config.queueCapacityFrames);
            queueHead_ = 0;
            queueSize_ = 0;
            wakeAtQueueSize_ = 0;
            snapshot_.queuedFrames = 0;
            snapshot_.queuedFramesHighWater = 0;
            snapshot_.queueCapacityFrames = frameQueue_.size();
        }

        if (endpointChanged)
        {
            resolvedHostIp_.clear();
            snapshot_.connected = false;
            snapshot_.scanValid = false;
            snapshot_.initialized = false;
            snapshot_.topology = {};
        }
    }

    // Never leave an established socket pointed at an endpoint that no longer
    // matches the configured text.  In particular, "auto" remains the saved
    // parameter value; its resolved interface address is stored separately.
    if (endpointChanged)
        commandClient_.disconnect();
    commandClient_.setFpgaIp(config.fpgaIp);
    commandClient_.setHostIp(config.hostIp);
    return true;
}

SessionConfig Session::configSnapshot() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return sessionConfig_;
}

bool Session::connect(std::string* error)
{
    // A dead TCP control path must remain recoverable even when the last
    // authoritative STATUS said that UDP streaming was active.
    if ((commandClient_.isConnected() && ! operationAllowed("Connect", error)) ||
        ! beginBusy("Connect", error))
        return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    return connectImpl(false, error);
}

bool Session::reconnect(std::string* error)
{
    if ((commandClient_.isConnected() && ! operationAllowed("Reconnect", error)) ||
        ! beginBusy("Reconnect", error))
        return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    return connectImpl(true, error);
}

bool Session::connectImpl(bool forceReconnect, std::string* error)
{
    bool recoveringActiveStream = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        recoveringActiveStream = snapshot_.streaming;
    }
    clearError();
    SessionConfig config = configSnapshot();
    std::string resolvedHostIp = config.hostIp;
    if (config.hostIp.empty() || config.hostIp == "0.0.0.0" ||
        config.hostIp == "auto" || config.hostIp == "AUTO")
    {
        resolvedHostIp = autoDetectHostIp(config.fpgaIp);
        if (resolvedHostIp.empty())
        {
            setError("Could not determine the local IPv4 route to " + config.fpgaIp, error);
            return false;
        }
    }

    commandClient_.setFpgaIp(config.fpgaIp);
    commandClient_.setHostIp(resolvedHostIp);

    std::string transportError;
    const bool connected = forceReconnect
        ? commandClient_.reconnect(3000ms, &transportError)
        : commandClient_.connect(3000ms, &transportError);
    if (! connected)
    {
        setError(transportError.empty() ? "TCP connection failed" : transportError, error);
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.connected = false;
        resolvedHostIp_.clear();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.connected = true;
        ++snapshot_.connectionGeneration;
        snapshot_.outputs = {};
        snapshot_.banner = commandClient_.banner();
        resolvedHostIp_ = resolvedHostIp;
    }

    Reply pingReply{};
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        transportError.clear();
        pingReply = commandClient_.ping(1500ms, &transportError);
        if (replySucceeded(pingReply, Command::Ping))
            break;
        if (! replyIsValidFor(pingReply, Command::Ping) ||
            pingReply.status != static_cast<uint32_t>(CommandStatus::SnapshotBusy))
            break;
        std::this_thread::sleep_for(20ms);
    }
    if (! replySucceeded(pingReply, Command::Ping))
    {
        setError(replyError("PING", pingReply, transportError), error);
        return false;
    }

    if (! refreshStatusAndConfiguration(error))
        return false;

    bool hasValidScan = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        hasValidScan = snapshot_.scanValid && snapshot_.topology.valid;
    }
    if (hasValidScan && ! fetchScanPhases(0U, error))
        return false;

    if (recoveringActiveStream)
    {
        bool authoritativeUdpStream = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            authoritativeUdpStream = snapshot_.streaming && snapshot_.routeUdp;
        }
        if (! authoritativeUdpStream)
        {
            const std::string message =
                "TCP control reconnected, but STATUS confirms that the prior UDP "
                "stream is no longer active";
            failListener(message);
            stopListener();
            closeDataSocket();
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                queueHead_ = 0;
                queueSize_ = 0;
                wakeAtQueueSize_ = 0;
                snapshot_.queuedFrames = 0;
            }
            if (error != nullptr)
                error->clear();
            return true;
        }
    }

    clearError();
    return true;
}

void Session::disconnect()
{
    stopStreaming();
    // Disconnect abandons any remaining retry opportunity, so always join the
    // listener before the Session's std::thread members can be destroyed.
    stopListener();
    joinImpedanceThread(true);

    std::lock_guard<std::mutex> operationLock(operationMutex_);
    commandClient_.disconnect();
    closeDataSocket();

    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.connected = false;
    snapshot_.outputs = {};
    snapshot_.scanValid = false;
    snapshot_.initialized = false;
    snapshot_.streaming = false;
    snapshot_.routeUdp = false;
    snapshot_.listenerRunning = false;
    snapshot_.receivedData = false;
    snapshot_.commandBusy = false;
    snapshot_.impedanceRunning = false;
    snapshot_.cancelRequested = false;
    snapshot_.controlState = ControlState::Idle;
    snapshot_.topology = {};
    snapshot_.progress = {};
    queueHead_ = 0;
    queueSize_ = 0;
    wakeAtQueueSize_ = 0;
    snapshot_.queuedFrames = 0;
    snapshot_.queuedFramesHighWater = 0;
    snapshot_.kernelReportedReceiveBufferBytes = 0;
    snapshot_.actualReceiveBufferBytes = 0;
    snapshot_.receiveBufferSetSucceeded = false;
    snapshot_.receiveBufferQuerySucceeded = false;
    snapshot_.receiveBufferClamped = false;
    snapshot_.receiveBufferWarning.clear();
    fatalError_.clear();
    resolvedHostIp_.clear();
    commandBusy_ = false;
    impedanceRunning_ = false;
    cancelRequested_ = false;
}

bool Session::checkConnection(std::string* error)
{
    if (! commandClient_.isConnected())
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            snapshot_.connected = false;
        }
        setError("Board is not connected; use Connect or Reconnect first", error);
        return false;
    }

    std::string transportError;
    Reply pingReply = commandClient_.ping(1500ms, &transportError);
    if (! replySucceeded(pingReply, Command::Ping))
    {
        setError(replyError("PING", pingReply, transportError), error);
        return false;
    }

    if (! refreshStatusAndConfiguration(error))
        return false;
    clearError();
    return true;
}

bool Session::refreshStatus(std::string* error)
{
    std::string transportError;
    Reply reply{};
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        transportError.clear();
        reply = commandClient_.getStatus(1500ms, &transportError);
        if (replySucceeded(reply, Command::GetStatus))
            break;
        if (! replyIsValidFor(reply, Command::GetStatus) ||
            reply.status != static_cast<uint32_t>(CommandStatus::SnapshotBusy))
            break;
        std::this_thread::sleep_for(10ms);
    }
    if (! replySucceeded(reply, Command::GetStatus))
    {
        setError(replyError("STATUS", reply, transportError), error);
        return false;
    }

    const Status status = CommandClient::statusFromReply(reply);
    Topology topology{};
    std::string topologyError;
    bool topologyOk = true;
    if (status.scanValid)
    {
        topologyOk = buildTopology(status.physicalChipMask,
                                   status.logicalStreamMask,
                                   status.packedChipIds,
                                   status.layoutId,
                                   topology,
                                   &topologyError);
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.connected = true;
    snapshot_.scanValid = status.scanValid;
    snapshot_.initialized = status.initialized;
    snapshot_.streaming = status.streaming;
    snapshot_.routeUdp = status.routeUdp;
    snapshot_.controlState = status.state;
    if (status.scanValid && topologyOk)
    {
        if (snapshot_.streaming && snapshot_.topology.valid &&
            (snapshot_.topology.logicalMask != topology.logicalMask ||
             snapshot_.topology.layoutId != topology.layoutId ||
             snapshot_.topology.packedChipIds != topology.packedChipIds))
        {
            fatalError_ = "Headstage topology changed during streaming";
            snapshot_.lastError = fatalError_;
            listenerFailed_ = true;
        }
        else
        {
            if (sameTopologyIdentity(snapshot_.topology, topology))
            {
                for (size_t lane = 0; lane < topology.lanes.size(); ++lane)
                {
                    topology.lanes[lane].selectedPhaseTap =
                        snapshot_.topology.lanes[lane].selectedPhaseTap;
                }
            }
            snapshot_.topology = std::move(topology);
        }
    }
    else if (! status.scanValid && ! snapshot_.streaming)
    {
        snapshot_.topology = {};
    }

    if (! topologyOk)
    {
        snapshot_.lastError = topologyError;
        if (error != nullptr)
            *error = topologyError;
        return false;
    }
    return true;
}

bool Session::refreshConfiguration(std::string* error)
{
    auto readSection = [this](ConfigSection section,
                              Reply& reply,
                              std::string& transportError) -> bool
    {
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            transportError.clear();
            reply = commandClient_.getConfig(section, 1500ms, &transportError);
            if (replySucceeded(reply, Command::GetConfig))
                return true;
            if (! replyIsValidFor(reply, Command::GetConfig) ||
                reply.status != static_cast<uint32_t>(CommandStatus::SnapshotBusy))
                return false;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    };

    Reply core{};
    Reply filters{};
    Reply ttl{};
    std::string transportError;
    if (! readSection(ConfigSection::Core, core, transportError) ||
        ! readSection(ConfigSection::Filters, filters, transportError) ||
        ! readSection(ConfigSection::Ttl, ttl, transportError))
    {
        Reply failed = ! replySucceeded(core, Command::GetConfig) ? core :
                       (! replySucceeded(filters, Command::GetConfig) ? filters : ttl);
        setError(replyError("GET_CONFIG", failed, transportError), error);
        return false;
    }

    AcquisitionConfig config{};
    std::string decodeError;
    if (! CommandClient::configurationFromReplies(core, filters, ttl,
                                                   config, &decodeError))
    {
        setError(decodeError.empty() ? "GET_CONFIG reply decoding failed" : decodeError,
                 error);
        return false;
    }

    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.config = config;
    snapshot_.initialized = config.initialized;
    return true;
}

bool Session::refreshStatusAndConfiguration(std::string* error)
{
    return refreshStatus(error) && refreshConfiguration(error);
}

bool Session::buildTopology(uint32_t physicalMask,
                            uint32_t logicalMask,
                            uint32_t packedChipIds,
                            uint16_t layoutId,
                            Topology& topology,
                            std::string* error)
{
    if (physicalMask == 0 && logicalMask == 0)
    {
        topology = {};
        topology.physicalMask = 0;
        topology.logicalMask = 0;
        topology.packedChipIds = packedChipIds;
        topology.layoutId = layoutId;
        topology.lanes = decodeLaneDetections(packedChipIds);
        topology.valid = true;
        return true;
    }
    return Topology::fromStatus(physicalMask, logicalMask, packedChipIds,
                                layoutId, topology, error);
}

bool Session::reset(std::string* error)
{
    if (!operationAllowed("Reset", error) || !beginBusy("Reset", error))
        return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (!ensureConnected(error)) return false;

    std::string transportError;
    const Reply reply = commandClient_.request(Command::Reset, {}, 15000ms, &transportError);
    if (!replySucceeded(reply, Command::Reset)) {
        // A lost reply can leave the outcome uncertain. Invalidate output
        // readback and recover it through polling before enabling any action.
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            snapshot_.outputs.valid = false;
            snapshot_.outputs.settingsValid = false;
        }
        (void) refreshStatusAndConfiguration();
        setError(replyError("RESET", reply, transportError), error);
        return false;
    }
    stopListener();
    closeDataSocket();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        ++snapshot_.resetGeneration;
        snapshot_.scanValid = false;
        snapshot_.initialized = false;
        snapshot_.topology = {};
        snapshot_.outputs = {};
        snapshot_.progress = {};
        snapshot_.impedanceResults.clear();
        snapshot_.controlState = ControlState::Idle;
        queueHead_ = 0; queueSize_ = 0; wakeAtQueueSize_ = 0;
        fatalError_.clear();
        snapshot_.queuedFrames = 0;
        snapshot_.receivedData = false;
        snapshot_.parserStats = {};
        snapshot_.queueOverruns = 0;
        snapshot_.kernelDroppedPackets = 0;
        snapshot_.unexpectedSourcePackets = 0;
        snapshot_.consumerDroppedFrames = 0;
        snapshot_.queuedFramesHighWater = 0;
    }
    if (!refreshStatusAndConfiguration(error) || !readOutputsImpl(error))
        return false;
    clearError();
    return true;
}

bool Session::scan(std::string* error,
                   const ProgressCallback& progressCallback)
{
    if (! operationAllowed("Rescan", error) || ! beginBusy("Rescan", error))
        return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (! ensureConnected(error))
        return false;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.progress = {};
    }

    auto publishProgress = [this, &progressCallback](
                               const ProgressSnapshot& progress)
    {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            changed = ! sameProgress(snapshot_.progress, progress);
            snapshot_.progress = progress;
        }
        if (changed && progressCallback)
            progressCallback(progress);
    };

    uint32_t activeScanResultId = 0U;
    auto pollProgress = [this, &publishProgress, &activeScanResultId](
                            bool requireActiveScan)
    {
        std::string progressError;
        Reply progressReply{};
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            progressReply = commandClient_.getProgress(1000ms, &progressError);
            if (replySucceeded(progressReply, Command::GetProgress) ||
                ! replyIsValidFor(progressReply, Command::GetProgress) ||
                progressReply.status !=
                    static_cast<uint32_t>(CommandStatus::SnapshotBusy))
                break;
            std::this_thread::sleep_for(10ms);
        }
        if (! replySucceeded(progressReply, Command::GetProgress))
            return;

        const ProgressSnapshot progress =
            CommandClient::progressFromReply(progressReply);
        if (progress.operation != kCmdScan || progress.resultId == 0U)
            return;
        if (requireActiveScan)
        {
            if (progress.state !=
                static_cast<uint32_t>(ControlState::Scanning))
                return;
            if (activeScanResultId == 0U)
                activeScanResultId = progress.resultId;
            else if (progress.resultId != activeScanResultId)
                return;
        }
        else if (activeScanResultId == 0U ||
                 progress.resultId != activeScanResultId)
        {
            // GET_PROGRESS is persistent.  A SCAN rejected before it starts
            // must not republish the terminal snapshot from an earlier scan.
            return;
        }
        publishProgress(progress);
    };

    std::string transportError;
    auto finalReply = std::async(std::launch::async,
                                 [this, &transportError]
    {
        return commandClient_.scan(180s, &transportError);
    });
    while (finalReply.wait_for(200ms) != std::future_status::ready)
        pollProgress(true);

    const Reply reply = finalReply.get();
    // Retain the terminal operation-progress snapshot for status reporting.
    // Phase cells are updated only from the completed per-lane result below;
    // intermediate trial phases are deliberately not presented in the UI.
    pollProgress(false);
    if (! replySucceeded(reply, Command::Scan))
    {
        const bool validScanReply = replyIsValidFor(reply, Command::Scan);
        if (validScanReply &&
            reply.status ==
                static_cast<uint32_t>(CommandStatus::NoChipDetected))
        {
            /* Firmware deliberately remains IDLE for this negative discovery
             * result.  Discard any topology retained from an earlier scan so
             * the UI cannot present a disconnected headstage as usable. */
            std::lock_guard<std::mutex> lock(stateMutex_);
            snapshot_.scanValid = false;
            snapshot_.initialized = false;
            snapshot_.config.initialized = false;
            snapshot_.controlState = ControlState::Idle;
            snapshot_.topology = {};
            snapshot_.impedanceResults.clear();
        }
        else if (validScanReply &&
                 reply.status == static_cast<uint32_t>(
                     CommandStatus::HardwareFailure))
        {
            // A hardware-failed SCAN invalidates calibration in firmware and
            // normally enters FAULT. Refresh that authoritative state so a
            // previous successful topology/phase cannot remain visible or be
            // considered ready. A command rejected before SCAN starts
            // preserves the previous calibration instead.
            std::string refreshError;
            if (! refreshStatusAndConfiguration(&refreshError))
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                snapshot_.scanValid = false;
                snapshot_.initialized = false;
                snapshot_.config.initialized = false;
                snapshot_.topology = {};
                snapshot_.impedanceResults.clear();
            }
        }
        setError(replyError("SCAN", reply, transportError), error);
        return false;
    }

    Topology topology{};
    std::string topologyError;
    if (! buildTopology(reply.data0, reply.data1, reply.data2,
                        static_cast<uint16_t>(reply.data3), topology,
                        &topologyError))
    {
        setError("SCAN returned an invalid topology: " + topologyError, error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.scanValid = true;
        snapshot_.initialized = false;
        snapshot_.topology = std::move(topology);
        snapshot_.impedanceResults.clear();
    }

    if (! refreshStatusAndConfiguration(error))
        return false;
    if (! fetchScanPhases(activeScanResultId, error))
        return false;
    clearError();
    return true;
}

bool Session::init(std::string* error)
{
    if (! operationAllowed("Initialize", error) || ! beginBusy("Initialize", error))
        return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (! ensureConnected(error))
        return false;
    return ensureInitialized(error);
}

bool Session::ensureConnected(std::string* error)
{
    if (commandClient_.isConnected())
        return true;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.connected = false;
    }
    setError("Board is disconnected; use Connect or Reconnect", error);
    return false;
}

bool Session::ensureInitialized(std::string* error)
{
    if (! refreshStatusAndConfiguration(error))
        return false;

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (! snapshot_.scanValid || ! snapshot_.topology.valid ||
            snapshot_.topology.totalChannels == 0)
        {
            const std::string message = "No scanned RHD headstage topology is available";
            snapshot_.lastError = message;
            if (error != nullptr)
                *error = message;
            return false;
        }
        if (snapshot_.initialized)
            return true;
    }

    std::string transportError;
    const Reply reply = commandClient_.init(kInitFirstDetectedStream,
                                             true,
                                             180s,
                                             &transportError);
    if (! replySucceeded(reply, Command::Init))
    {
        setError(replyError("INIT", reply, transportError), error);
        return false;
    }
    if (! refreshStatusAndConfiguration(error))
        return false;

    std::lock_guard<std::mutex> lock(stateMutex_);
    if (! snapshot_.initialized)
    {
        const std::string message = "INIT completed but firmware did not mark the configuration initialized";
        snapshot_.lastError = message;
        if (error != nullptr)
            *error = message;
        return false;
    }
    snapshot_.lastError.clear();
    return true;
}

bool Session::setSampleRate(uint32_t sampleRateHz, std::string* error)
{
    if (! isSupportedSampleRate(sampleRateHz))
    {
        setError("Unsupported sample rate: " + std::to_string(sampleRateHz), error);
        return false;
    }
    if (! operationAllowed("Sample-rate change", error) ||
        ! beginBusy("Sample-rate change", error))
        return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (! ensureConnected(error))
        return false;

    std::string transportError;
    const Reply reply = commandClient_.setRate(sampleRateHz, 15000ms, &transportError);
    if (! replySucceeded(reply, Command::SetRate))
    {
        setError(replyError("SET_RATE", reply, transportError), error);
        return false;
    }
    if (! refreshStatusAndConfiguration(error))
        return false;
    clearError();
    return true;
}

bool Session::setBandwidth(uint32_t analogLowerMilliHz,
                           uint32_t analogUpperHz,
                           std::string* error)
{
    if (analogLowerMilliHz >= analogUpperHz * 1000ULL)
    {
        setError("Analog lower bandwidth must be below the upper bandwidth", error);
        return false;
    }
    if (! operationAllowed("Bandwidth change", error) ||
        ! beginBusy("Bandwidth change", error))
        return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (! ensureConnected(error))
        return false;

    std::string transportError;
    const Reply reply = commandClient_.setBandwidth(analogLowerMilliHz,
                                                     analogUpperHz,
                                                     15000ms,
                                                     &transportError);
    if (! replySucceeded(reply, Command::SetBandwidth))
    {
        setError(replyError("SET_BANDWIDTH", reply, transportError), error);
        return false;
    }
    if (! refreshStatusAndConfiguration(error))
        return false;
    clearError();
    return true;
}

bool Session::setDsp(bool enabled,
                     uint32_t requestedCutoffMilliHz,
                     std::string* error)
{
    if (! operationAllowed("DSP change", error) || ! beginBusy("DSP change", error))
        return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (! ensureConnected(error))
        return false;

    std::string transportError;
    const Reply reply = commandClient_.setDsp(enabled,
                                               requestedCutoffMilliHz,
                                               15000ms,
                                               &transportError);
    if (! replySucceeded(reply, Command::SetDsp))
    {
        setError(replyError("SET_DSP", reply, transportError), error);
        return false;
    }
    if (! refreshStatusAndConfiguration(error))
        return false;
    clearError();
    return true;
}

bool Session::setTtlFastSettle(bool enabled,
                               uint32_t channel,
                               std::string* error)
{
    if (enabled && channel >= 16U)
    {
        setError("TTL fast-settle channel must be in the range TTL0..TTL15", error);
        return false;
    }
    if (! operationAllowed("TTL fast-settle change", error) ||
        ! beginBusy("TTL fast-settle change", error))
        return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (! ensureConnected(error))
        return false;

    std::string transportError;
    const Reply reply = commandClient_.setTtlSettle(enabled,
                                                     enabled ? channel : 0U,
                                                     15000ms,
                                                     &transportError);
    if (! replySucceeded(reply, Command::SetTtlSettle))
    {
        setError(replyError("SET_TTL_SETTLE", reply, transportError), error);
        return false;
    }
    if (! refreshStatusAndConfiguration(error))
        return false;
    clearError();
    return true;
}

bool Session::isCommandBusy() const
{
    return commandBusy_.load();
}

SessionSnapshot Session::snapshot() const
{
    const bool controlConnected = commandClient_.isConnected();
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (snapshot_.connected && ! controlConnected)
        snapshot_.connected = false;
    SessionSnapshot copy = snapshot_;
    copy.sampleMode = sessionConfig_.sampleMode;
    copy.commandBusy = commandBusy_.load();
    copy.impedanceRunning = impedanceRunning_.load();
    copy.cancelRequested = cancelRequested_.load();
    copy.queuedFrames = queueSize_;
    copy.queueCapacityFrames = frameQueue_.size();
    return copy;
}

bool Session::operationAllowed(const char* operation, std::string* error) const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (snapshot_.streaming)
    {
        if (error != nullptr)
            *error = std::string(operation) + " is locked while streaming";
        return false;
    }
    return true;
}

bool Session::beginBusy(const char* operation, std::string* error)
{
    bool expected = false;
    if (! commandBusy_.compare_exchange_strong(expected, true))
    {
        const std::string message = std::string(operation) +
                                    " cannot start while another command is active";
        if (error != nullptr)
            *error = message;
        return false;
    }
    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.commandBusy = true;
    return true;
}

void Session::endBusy()
{
    commandBusy_ = false;
    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.commandBusy = false;
}

void Session::setError(const std::string& message, std::string* error)
{
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.lastError = message;
    }
    if (error != nullptr)
        *error = message;
}

void Session::clearError()
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.lastError.clear();
}

std::string Session::autoDetectHostIp(const std::string& fpgaIp)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return {};

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(kFpgaCommandPort);
    if (::inet_pton(AF_INET, fpgaIp.c_str(), &remote.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) != 0)
    {
        ::close(fd);
        return {};
    }

    sockaddr_in local{};
    socklen_t length = sizeof(local);
    std::string result;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) == 0)
    {
        std::array<char, INET_ADDRSTRLEN> buffer{};
        if (::inet_ntop(AF_INET, &local.sin_addr, buffer.data(), buffer.size()) != nullptr)
            result = buffer.data();
    }
    ::close(fd);
    return result;
}

uint32_t Session::hostOrderIpv4(const std::string& address)
{
    in_addr parsed{};
    if (::inet_pton(AF_INET, address.c_str(), &parsed) != 1)
        return 0;
    return ntohl(parsed.s_addr);
}

std::string Session::replyError(const char* operation,
                                const Reply& reply,
                                const std::string& transportError)
{
    if (! transportError.empty())
        return std::string(operation) + " failed: " + transportError;
    std::ostringstream stream;
    stream << operation << " failed: " << statusName(reply.status)
           << " (status " << reply.status << ")";
    return stream.str();
}

} // namespace nclp
