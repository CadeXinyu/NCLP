#pragma once

#include "NCLPCommandClient.h"
#include "NCLPFrameParser.h"
#include "NCLPOutputTypes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nclp
{

struct SessionConfig
{
    std::string fpgaIp = kDefaultFpgaIp;
    std::string hostIp = kDefaultHostIp;
    uint16_t dataPort = kHostDataPort;
    SampleMode sampleMode = SampleMode::Aux;
    // At 30 kS/s this retains a little over 0.5 s of decoded data, allowing
    // short Open Ephys scheduling stalls to recover without dropping frames.
    size_t queueCapacityFrames = 16384;
    int receiveBufferBytes = 4 * 1024 * 1024;
};

struct QueuedFrame
{
    std::array<float, kMaxContinuousChannels> samples{};
    std::array<int16_t, kMaxElectrodeChannels> amplifierCounts{};
    size_t channelCount = 0;
    uint64_t timestamp = 0;
    uint16_t ttl = 0;
    uint32_t missingBefore = 0;
    uint64_t timestampMissingBefore = 0;
};

struct ImpedanceRecord
{
    uint32_t globalChannel = 0;
    uint32_t localChannel = 0;
    uint32_t physicalIndex = 0;
    uint32_t capRange = 0;
    bool valid = false;
    bool timestampError = false;
    bool verifyError = false;
    bool saturated = false;
    uint64_t magnitudeMilliohms = 0;
    int32_t phaseMicrodegrees = 0;
};

namespace detail
{

/**
 * Validates the eight retained SCAN records against the active topology and
 * applies each detected lane's effective selected phase. Firmware reports
 * max(MISO-A, MISO-B) for RHD2164 and MISO-A for other chips. Undetected
 * records leave the phase unavailable even if their raw word contains one.
 */
bool applyScanResultPhases(
    Topology& topology,
    const std::array<ResultRecord, kPhysicalLaneCount>& records,
    std::string* error = nullptr);

/**
 * Linux returns twice the user-requested SO_RCVBUF value from getsockopt().
 * Convert that kernel accounting value back to the effective configured size
 * shown to the operator. Non-positive values mean that the query failed.
 */
int effectiveLinuxReceiveBufferBytes(int kernelReportedBytes);

/**
 * Verifies that decoded impedance records belong to the active channel map.
 *
 * Partial record sets are valid (a cancelled measurement may publish them),
 * but every supplied record must identify one unique topology channel exactly.
 */
bool validateImpedanceRecords(const Topology& topology,
                              const std::vector<ImpedanceRecord>& records,
                              std::string* error = nullptr);

} // namespace detail

struct SessionSnapshot
{
    bool connected = false;
    uint64_t connectionGeneration = 0;
    uint64_t resetGeneration = 0;
    bool scanValid = false;
    bool initialized = false;
    bool streaming = false;
    bool routeUdp = false;
    bool listenerRunning = false;
    bool receivedData = false;
    bool commandBusy = false;
    bool impedanceRunning = false;
    bool cancelRequested = false;
    ControlState controlState = ControlState::Idle;
    SampleMode sampleMode = SampleMode::Aux;

    OutputSnapshot outputs{};
    Topology topology{};
    AcquisitionConfig config{};
    ParserStats parserStats{};
    ProgressSnapshot progress{};
    std::vector<ImpedanceRecord> impedanceResults;

    uint64_t queueOverruns = 0;
    uint64_t kernelDroppedPackets = 0;
    uint64_t unexpectedSourcePackets = 0;
    uint64_t consumerDroppedFrames = 0;
    size_t queuedFrames = 0;
    size_t queuedFramesHighWater = 0;
    size_t queueCapacityFrames = 0;
    size_t openEphysFifoSamples = 0;
    size_t openEphysFifoHighWater = 0;
    size_t openEphysFifoCapacitySamples = 0;
    uint64_t dataGapEvents = 0;
    uint64_t lastDataGapMissingSamples = 0;
    uint64_t lastDataGapBeforeTimestamp = 0;
    int requestedReceiveBufferBytes = 0;
    int kernelReportedReceiveBufferBytes = 0;
    // User-visible effective size. On Linux this is half of the raw
    // getsockopt(SO_RCVBUF) value because the kernel doubles it for accounting.
    int actualReceiveBufferBytes = 0;
    bool receiveBufferSetSucceeded = false;
    bool receiveBufferQuerySucceeded = false;
    bool receiveBufferClamped = false;
    std::string receiveBufferWarning;
    std::string banner;
    std::string lastError;
};

class Session
{
public:
    using ProgressCallback = std::function<void(const ProgressSnapshot&)>;

    Session();
    ~Session();

    bool updateConfig(const SessionConfig& config, std::string* error = nullptr);
    SessionConfig configSnapshot() const;

    bool connect(std::string* error = nullptr);
    bool reconnect(std::string* error = nullptr);
    void disconnect();
    bool checkConnection(std::string* error = nullptr);
    bool reset(std::string* error = nullptr);

    bool scan(std::string* error = nullptr,
              const ProgressCallback& progressCallback = {});
    bool init(std::string* error = nullptr);
    bool setSampleRate(uint32_t sampleRateHz, std::string* error = nullptr);
    bool setBandwidth(uint32_t analogLowerMilliHz,
                      uint32_t analogUpperHz,
                      std::string* error = nullptr);
    bool setDsp(bool enabled,
                uint32_t requestedCutoffMilliHz,
                std::string* error = nullptr);
    bool setTtlFastSettle(bool enabled,
                          uint32_t channel,
                          std::string* error = nullptr);

    bool readOutputs(std::string* error = nullptr);
    bool pollOutputStatus(std::string* error = nullptr);
    bool sendOutputCommand(Command command, const CommandArgs& args, std::string* error = nullptr);
    bool setRippleK(float k, std::string* error = nullptr);

    bool runImpedance(std::string* error = nullptr);
    bool cancelImpedance(std::string* error = nullptr);

    bool startStreaming(std::chrono::milliseconds firstPacketTimeout,
                        std::string* error = nullptr);
    void stopStreaming();

    size_t drainFrames(std::vector<QueuedFrame>& out, size_t maxFrames);
    size_t waitAndDrainFrames(std::vector<QueuedFrame>& out,
                              size_t minimumFrames,
                              size_t maxFrames,
                              std::chrono::milliseconds timeout);
    void recordConsumerDrops(uint64_t frames);
    bool consumeFatalError(std::string* error);
    bool isCommandBusy() const;
    SessionSnapshot snapshot() const;

private:
    bool readOutputsImpl(std::string* error, bool statusOnly = false);
    bool connectImpl(bool forceReconnect, std::string* error);
    bool refreshStatus(std::string* error = nullptr);
    bool refreshConfiguration(std::string* error = nullptr);
    bool refreshStatusAndConfiguration(std::string* error = nullptr);
    bool buildTopology(uint32_t physicalMask,
                       uint32_t logicalMask,
                       uint32_t packedChipIds,
                       uint16_t layoutId,
                       Topology& topology,
                       std::string* error);
    bool ensureConnected(std::string* error);
    bool ensureInitialized(std::string* error);
    bool operationAllowed(const char* operation, std::string* error) const;
    bool beginBusy(const char* operation, std::string* error);
    void endBusy();
    void setError(const std::string& message, std::string* error = nullptr);
    void clearError();

    bool openDataSocket(const std::string& bindHostIp, std::string* error);
    void closeDataSocket();
    void stopListener();
    void listenerLoop(uint32_t expectedSourceAddress);
    void failListener(const std::string& message);
    bool reconcileFailedStart(const std::string& startError,
                              std::string* error);

    void impedanceLoop();
    bool fetchScanPhases(uint32_t resultId, std::string* error);
    bool fetchImpedanceResults(uint32_t resultId, std::string* error);
    void joinImpedanceThread(bool disconnectClient);

    static std::string autoDetectHostIp(const std::string& fpgaIp);
    static uint32_t hostOrderIpv4(const std::string& address);
    static std::string replyError(const char* operation,
                                  const Reply& reply,
                                  const std::string& transportError);

    mutable std::mutex stateMutex_;
    mutable std::mutex operationMutex_;
    std::condition_variable dataReadyCv_;

    SessionConfig sessionConfig_{};
    std::string resolvedHostIp_;
    CommandClient commandClient_;
    FrameParser parser_;
    std::vector<QueuedFrame> frameQueue_;
    size_t queueHead_ = 0;
    size_t queueSize_ = 0;
    // Protected by stateMutex_. Zero means no batching consumer is waiting.
    size_t wakeAtQueueSize_ = 0;
    mutable SessionSnapshot snapshot_{};

    int dataSocketFd_ = -1;
    std::thread listenerThread_;
    std::thread impedanceThread_;
    std::atomic<bool> listenerStopRequested_{ false };
    std::atomic<bool> listenerFailed_{ false };
    std::atomic<bool> commandBusy_{ false };
    std::atomic<bool> impedanceRunning_{ false };
    std::atomic<bool> cancelRequested_{ false };
    std::string fatalError_;
};

} // namespace nclp
