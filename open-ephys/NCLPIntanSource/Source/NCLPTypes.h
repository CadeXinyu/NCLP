#pragma once

#include "../../../software/common/nclp_wire.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nclp
{

constexpr const char* kDefaultFpgaIp = "192.168.0.42";
constexpr const char* kDefaultHostIp = "192.168.0.10";
constexpr uint16_t kFpgaCommandPort = NCLP_CONTROL_PORT;
constexpr uint16_t kHostDataPort = NCLP_DATA_PORT;

constexpr uint32_t kCommandMagic = NCLP_CMD_MAGIC;
constexpr uint32_t kCommandVersion = NCLP_CMD_VERSION;
constexpr size_t kCommandWords = NCLP_CMD_WORDS;
constexpr size_t kCommandArgWords = kCommandWords - 4;
constexpr size_t kReplyWords = NCLP_REPLY_WORDS;

constexpr uint32_t kCmdPing = NCLP_CMD_PING;
constexpr uint32_t kCmdGetStatus = NCLP_CMD_GET_STATUS;
constexpr uint32_t kCmdSetUdpDest = NCLP_CMD_SET_UDP_DEST;
constexpr uint32_t kCmdGetProgress = NCLP_CMD_GET_PROGRESS;
constexpr uint32_t kCmdGetResult = NCLP_CMD_GET_RESULT;
constexpr uint32_t kCmdGetConfig = NCLP_CMD_GET_CONFIG;
constexpr uint32_t kCmdSetRate = NCLP_CMD_SET_RATE;
constexpr uint32_t kCmdSetBandwidth = NCLP_CMD_SET_BANDWIDTH;
constexpr uint32_t kCmdSetDsp = NCLP_CMD_SET_DSP;
constexpr uint32_t kCmdSetTtlSettle = NCLP_CMD_SET_TTL_SETTLE;
constexpr uint32_t kCmdScan = NCLP_CMD_SCAN;
constexpr uint32_t kCmdInit = NCLP_CMD_INIT;
constexpr uint32_t kCmdImpedance = NCLP_CMD_IMPEDANCE;
constexpr uint32_t kCmdReset = NCLP_CMD_RESET;
constexpr uint32_t kCmdCancel = NCLP_CMD_CANCEL;
constexpr uint32_t kCmdStartStream = NCLP_CMD_START_STREAM;
constexpr uint32_t kCmdStopStream = NCLP_CMD_STOP_STREAM;

constexpr uint32_t kInitFirstDetectedStream = NCLP_INIT_FIRST_STREAM;
constexpr uint32_t kStartAuxInputs = NCLP_SAMPLE_MODE_AUX;
constexpr uint32_t kStartAuxVdd = NCLP_SAMPLE_MODE_VDD;

enum class Command : uint32_t
{
    Ping = kCmdPing,
    GetStatus = kCmdGetStatus,
    SetUdpDestination = kCmdSetUdpDest,
    GetProgress = kCmdGetProgress,
    GetResult = kCmdGetResult,
    GetConfig = kCmdGetConfig,
    SetRate = kCmdSetRate,
    SetBandwidth = kCmdSetBandwidth,
    SetDsp = kCmdSetDsp,
    SetTtlSettle = kCmdSetTtlSettle,
    Scan = kCmdScan,
    Init = kCmdInit,
    Impedance = kCmdImpedance,
    Reset = kCmdReset,
    Cancel = kCmdCancel,
    StartStream = kCmdStartStream,
    StopStream = kCmdStopStream,
    StimGet = NCLP_CMD_STIM_GET,
    StimSetAction = NCLP_CMD_STIM_SET_ACTION,
    DacSetConfig = NCLP_CMD_DAC_SET_CONFIG,
    DacSetClock = NCLP_CMD_DAC_SET_CLOCK,
    DacSetIntanTtl = NCLP_CMD_DAC_SET_INTAN_TTL,
    DacWrite = NCLP_CMD_DAC_WRITE,
    DacRead = NCLP_CMD_DAC_READ,
    DacPreset = NCLP_CMD_DAC_PRESET,
    StimControl = NCLP_CMD_STIM_CONTROL,
    StimDiagnostics = NCLP_CMD_STIM_DIAGNOSTICS,
    SetIntanSync = NCLP_CMD_SET_INTAN_SYNC,
    GetIntanSync = NCLP_CMD_GET_INTAN_SYNC,
    RippleGetConfig = NCLP_CMD_RIPPLE_GET_CONFIG,
    RippleApplyProfile = NCLP_CMD_RIPPLE_APPLY_PROFILE,
    RippleBaseline = NCLP_CMD_RIPPLE_BASELINE,
    RippleStatus = NCLP_CMD_RIPPLE_STATUS,
    RippleControl = NCLP_CMD_RIPPLE_CONTROL,
    RippleSetK = NCLP_CMD_RIPPLE_SET_K
};

enum class CommandStatus : uint32_t
{
    Ok = NCLP_COMMAND_STATUS_OK,
    HardwareFailure = NCLP_COMMAND_STATUS_HARDWARE,
    Prerequisite = NCLP_COMMAND_STATUS_PREREQUISITE,
    InvalidArgument = NCLP_COMMAND_STATUS_ARGUMENT,
    Cancelled = NCLP_COMMAND_STATUS_CANCELLED,
    Busy = NCLP_COMMAND_STATUS_BUSY,
    Network = NCLP_COMMAND_STATUS_NETWORK,
    NoResult = NCLP_COMMAND_STATUS_NO_RESULT,
    StaleResult = NCLP_COMMAND_STATUS_STALE_RESULT,
    ResultRange = NCLP_COMMAND_STATUS_RESULT_RANGE,
    SnapshotBusy = NCLP_COMMAND_STATUS_SNAPSHOT_BUSY,
    NotActive = NCLP_COMMAND_STATUS_NOT_ACTIVE,
    NoChipDetected = NCLP_COMMAND_STATUS_NO_CHIP_DETECTED,
    UnknownCommand = NCLP_COMMAND_STATUS_UNKNOWN
};

enum class ControlState : uint8_t
{
    Idle = NCLP_STATE_IDLE,
    Scanning = NCLP_STATE_SCANNING,
    Initializing = NCLP_STATE_INITIALIZING,
    Impedance = NCLP_STATE_IMPEDANCE,
    Ready = NCLP_STATE_READY,
    Streaming = NCLP_STATE_STREAMING,
    Draining = NCLP_STATE_DRAINING,
    Fault = NCLP_STATE_FAULT,
    Cancelling = NCLP_STATE_CANCELLING,
    Restoring = NCLP_STATE_RESTORING
};

enum class SampleMode : uint8_t
{
    Aux = NCLP_SAMPLE_MODE_AUX,
    Vdd = NCLP_SAMPLE_MODE_VDD
};

enum class ConfigSection : uint32_t
{
    Core = NCLP_CONFIG_SECTION_CORE,
    Filters = NCLP_CONFIG_SECTION_FILTERS,
    Ttl = NCLP_CONFIG_SECTION_TTL
};

enum class ResultType : uint32_t
{
    Scan = NCLP_RESULT_TYPE_SCAN,
    Init = NCLP_RESULT_TYPE_INIT,
    Impedance = NCLP_RESULT_TYPE_IMPEDANCE
};

enum class ResultState : uint8_t
{
    Empty = NCLP_RESULT_STATE_EMPTY,
    Running = NCLP_RESULT_STATE_RUNNING,
    Complete = NCLP_RESULT_STATE_COMPLETE,
    Failed = NCLP_RESULT_STATE_FAILED,
    Cancelled = NCLP_RESULT_STATE_CANCELLED
};

constexpr uint32_t kResultIndexMetadata = NCLP_RESULT_INDEX_METADATA;
constexpr uint32_t kConfigFlagInitialized = NCLP_CONFIG_FLAG_INITIALIZED;
constexpr uint32_t kConfigFlagDspEnabled = NCLP_CONFIG_FLAG_DSP_ENABLED;
constexpr uint32_t kConfigFlagTtlSettleEnabled = NCLP_CONFIG_FLAG_TTL_SETTLE_ENABLED;

constexpr std::array<uint32_t, 6> kSupportedSampleRatesHz = {
    5000U, 10000U, 15000U, 20000U, 25000U, 30000U
};

// Exact register presets implemented by init_headstage.c. Keep these in
// ascending order so ties in nearestSupportedValue() resolve predictably to
// the lower cutoff.
constexpr std::array<uint32_t, 25> kSupportedAnalogLowerMilliHz = {
    100U, 250U, 300U, 500U, 750U, 1000U, 1500U, 2000U, 2500U,
    3000U, 5000U, 7500U, 10000U, 15000U, 20000U, 25000U, 30000U,
    50000U, 75000U, 100000U, 150000U, 200000U, 250000U, 300000U,
    500000U
};

constexpr std::array<uint32_t, 17> kSupportedAnalogUpperHz = {
    100U, 150U, 200U, 250U, 300U, 500U, 750U, 1000U, 1500U,
    2000U, 2500U, 3000U, 5000U, 7500U, 10000U, 15000U, 20000U
};

template <size_t Count>
constexpr uint32_t nearestSupportedValue(
    uint32_t requested,
    const std::array<uint32_t, Count>& supported)
{
    static_assert(Count > 0, "A preset table must not be empty");
    uint32_t best = supported[0];
    uint64_t bestDistance = requested > best
        ? static_cast<uint64_t>(requested - best)
        : static_cast<uint64_t>(best - requested);

    for (size_t index = 1; index < Count; ++index)
    {
        const uint32_t candidate = supported[index];
        const uint64_t distance = requested > candidate
            ? static_cast<uint64_t>(requested - candidate)
            : static_cast<uint64_t>(candidate - requested);
        if (distance < bestDistance)
        {
            best = candidate;
            bestDistance = distance;
        }
    }
    return best;
}

/**
 * Quantize a requested analog passband to values accepted by firmware.
 *
 * Returns false if independently selecting the nearest presets collapses or
 * reverses the passband. Outputs are always populated so the caller can report
 * the exact rejected pair.
 */
inline bool quantizeAnalogBandwidth(uint32_t requestedLowerMilliHz,
                                    uint32_t requestedUpperHz,
                                    uint32_t& lowerMilliHz,
                                    uint32_t& upperHz)
{
    lowerMilliHz = nearestSupportedValue(requestedLowerMilliHz,
                                         kSupportedAnalogLowerMilliHz);
    upperHz = nearestSupportedValue(requestedUpperHz,
                                    kSupportedAnalogUpperHz);
    return static_cast<uint64_t>(lowerMilliHz) <
           static_cast<uint64_t>(upperHz) * 1000ULL;
}

static_assert(nearestSupportedValue(260U, kSupportedAnalogLowerMilliHz) == 250U,
              "lower-bandwidth preset quantization changed");
static_assert(nearestSupportedValue(6200U, kSupportedAnalogUpperHz) == 5000U,
              "upper-bandwidth preset quantization changed");

constexpr uint32_t kUdpMagic = NCLP_UDP_MAGIC;
constexpr uint8_t kUdpVersion = NCLP_UDP_VERSION;
constexpr uint8_t kUdpHeaderBytes = NCLP_UDP_HEADER_BYTES;
constexpr uint16_t kUdpFlagModeAux = NCLP_UDP_FLAG_MODE_AUX;
constexpr uint16_t kUdpFlagModeVdd = NCLP_UDP_FLAG_MODE_VDD;
constexpr uint16_t kUdpFlagModeMask = NCLP_UDP_FLAG_MODE_MASK;
constexpr uint16_t kUdpFlagRate5kHz = NCLP_UDP_FLAG_RATE_5KHZ;
constexpr uint16_t kUdpFlagRate10kHz = NCLP_UDP_FLAG_RATE_10KHZ;
constexpr uint16_t kUdpFlagRate15kHz = NCLP_UDP_FLAG_RATE_15KHZ;
constexpr uint16_t kUdpFlagRate20kHz = NCLP_UDP_FLAG_RATE_20KHZ;
constexpr uint16_t kUdpFlagRate25kHz = NCLP_UDP_FLAG_RATE_25KHZ;
constexpr uint16_t kUdpFlagRate30kHz = NCLP_UDP_FLAG_RATE_30KHZ;
constexpr uint16_t kUdpFlagRateMask = NCLP_UDP_FLAG_RATE_MASK;

constexpr size_t kLogicalStreamSlots = NCLP_PL_MAX_LOGICAL_STREAMS;
constexpr size_t kPhysicalLaneCount = 8;
constexpr size_t kPortCount = 4;
constexpr size_t kMaxElectrodeChannels = 512;
constexpr size_t kAuxInputsPerHeadstage = 3;
constexpr size_t kMaxAuxChannels =
    kPhysicalLaneCount * kAuxInputsPerHeadstage;
constexpr size_t kMaxContinuousChannels =
    kMaxElectrodeChannels + kMaxAuxChannels;
constexpr size_t kFramePrefixBytes = 12;
constexpr size_t kAmplifierRows = 32;
constexpr size_t kAuxRows = 3;
constexpr size_t kFrameRows = kAmplifierRows + kAuxRows;
static_assert(kAuxRows == kAuxInputsPerHeadstage,
              "one fixed AUX frame row is required per physical AUX input");
static_assert(kMaxContinuousChannels == 536,
              "maximum NCLP amplifier+AUX channel geometry changed");
constexpr size_t kFrameSlotBytes = 2;
constexpr size_t kFrameTailBytes = 2;
constexpr size_t kUdpFrameBytes =
    kFramePrefixBytes + kFrameRows * kLogicalStreamSlots * kFrameSlotBytes +
    kFrameTailBytes;
constexpr size_t kUdpDatagramBytes = kUdpHeaderBytes + kUdpFrameBytes;
constexpr float kDefaultBitVolts = 0.195f;
// Keep AUX samples as lossless signed counts, matching the Glance plugin's
// unambiguous Open Ephys representation. The original unsigned ADC code and
// physical voltage remain recoverable as:
//   unsigned_code = signed_sample + 32768
//   volts = unsigned_code * kAuxVoltsPerCount
constexpr float kAuxBitVolts = 1.0f;
constexpr float kAuxVoltsPerCount = 2.45f / 65536.0f;

constexpr std::array<uint8_t, 8> kFrameMagicBytes = {
    0x42U, 0x19U, 0x02U, 0x27U, 0x99U, 0x19U, 0x91U, 0xc6U
};
constexpr std::array<const char*, kPhysicalLaneCount> kLaneNames = {
    "A1", "A2", "B1", "B2", "C1", "C2", "D1", "D2"
};
constexpr uint32_t kCablePhaseTapCount = 16U;
constexpr uint32_t kCablePhaseTapLast = kCablePhaseTapCount - 1U;

enum class ChipType : uint8_t
{
    None = 0,
    RHD2132 = 1,
    RHD2216 = 2,
    RHD2164 = 4
};

constexpr uint8_t kChipIdNone = static_cast<uint8_t>(ChipType::None);
constexpr uint8_t kChipIdRHD2132 = static_cast<uint8_t>(ChipType::RHD2132);
constexpr uint8_t kChipIdRHD2216 = static_cast<uint8_t>(ChipType::RHD2216);
constexpr uint8_t kChipIdRHD2164 = static_cast<uint8_t>(ChipType::RHD2164);
constexpr size_t kLaneCount = kPhysicalLaneCount;

inline uint32_t channelCountForChip(ChipType chip)
{
    switch (chip)
    {
        case ChipType::RHD2216: return 16U;
        case ChipType::RHD2132: return 32U;
        case ChipType::RHD2164: return 64U;
        default: return 0U;
    }
}

inline uint32_t channelCountForChipId(uint8_t chipId)
{
    return channelCountForChip(static_cast<ChipType>(chipId));
}

inline const char* chipTypeName(ChipType chip)
{
    switch (chip)
    {
        case ChipType::RHD2216: return "RHD2216";
        case ChipType::RHD2132: return "RHD2132";
        case ChipType::RHD2164: return "RHD2164";
        default: return "None";
    }
}

inline const char* laneNameForIndex(size_t index)
{
    return index < kLaneNames.size() ? kLaneNames[index] : "?";
}

inline uint16_t sampleRateFlag(uint32_t sampleRateHz)
{
    switch (sampleRateHz)
    {
        case 5000U: return kUdpFlagRate5kHz;
        case 10000U: return kUdpFlagRate10kHz;
        case 15000U: return kUdpFlagRate15kHz;
        case 20000U: return kUdpFlagRate20kHz;
        case 25000U: return kUdpFlagRate25kHz;
        case 30000U: return kUdpFlagRate30kHz;
        default: return 0U;
    }
}

inline uint16_t sampleModeFlag(SampleMode mode)
{
    return mode == SampleMode::Vdd ? kUdpFlagModeVdd : kUdpFlagModeAux;
}

using CommandArgs = std::array<uint32_t, kCommandArgWords>;

struct Reply
{
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t command = 0;
    uint32_t sequence = 0;
    uint32_t status = 0;
    uint32_t data0 = 0;
    uint32_t data1 = 0;
    uint32_t data2 = 0;
    uint32_t data3 = 0;
    uint32_t failCount = 0;

    bool validFor(uint32_t expectedCommand, uint32_t expectedSequence) const
    {
        return magic == kCommandMagic && version == kCommandVersion &&
               command == expectedCommand && sequence == expectedSequence;
    }

    bool ok() const { return status == static_cast<uint32_t>(CommandStatus::Ok); }
};

struct LaneDetection
{
    bool detected = false;
    uint8_t chipId = kChipIdNone;
    ChipType chip = ChipType::None;
    uint32_t channelCount = 0;
    // Effective selected cable-delay tap returned by the retained SCAN
    // result. For RHD2164 this is max(MISO-A, MISO-B); for other chips it is
    // the MISO-A tap. A negative value means that no selected phase is known.
    int32_t selectedPhaseTap = -1;

    bool hasSelectedPhase() const
    {
        return detected && selectedPhaseTap >= 0 &&
               selectedPhaseTap <= static_cast<int32_t>(kCablePhaseTapLast);
    }
};

inline std::array<LaneDetection, kLaneCount> decodeLaneDetections(
    uint32_t packedChipIds)
{
    std::array<LaneDetection, kLaneCount> lanes{};
    for (size_t lane = 0; lane < lanes.size(); ++lane)
    {
        const uint8_t chipId = static_cast<uint8_t>(
            (packedChipIds >> (lane * 4U)) & 0x0fU);
        lanes[lane].chipId = chipId;
        lanes[lane].chip = static_cast<ChipType>(chipId);
        lanes[lane].channelCount = channelCountForChipId(chipId);
        lanes[lane].detected = lanes[lane].channelCount != 0U;
    }
    return lanes;
}

struct HeadstageDescriptor
{
    size_t physicalIndex = 0;
    size_t portIndex = 0;
    size_t slotInPort = 0;
    ChipType chip = ChipType::None;
    uint32_t channelCount = 0;
    uint32_t globalChannelBase = 0;
    uint16_t logicalMask = 0;
};

struct ChannelAddress
{
    size_t globalChannel = 0;
    size_t physicalIndex = 0;
    uint32_t localChannel = 0;
    uint8_t logicalSlot = 0;
    uint8_t amplifierRow = 0;
};

struct AuxChannelAddress
{
    size_t globalAuxChannel = 0;
    size_t physicalIndex = 0;
    uint8_t logicalSlot = 0;
    uint8_t auxInput = 0;
    uint8_t frameRow = 0;
};

struct Topology
{
    uint8_t physicalMask = 0;
    uint16_t logicalMask = 0;
    uint16_t layoutId = 0;
    uint32_t packedChipIds = 0;
    uint32_t totalChannels = 0;
    uint32_t totalAuxChannels = 0;
    std::array<LaneDetection, kLaneCount> lanes{};
    std::vector<HeadstageDescriptor> headstages;
    std::vector<ChannelAddress> channels;
    std::vector<AuxChannelAddress> auxChannels;
    bool valid = false;

    static bool fromStatus(uint32_t physicalMask,
                           uint32_t logicalMask,
                           uint32_t packedChipIds,
                           uint16_t layoutId,
                           Topology& destination,
                           std::string* error = nullptr);
};

inline size_t continuousChannelCount(const Topology& topology,
                                     SampleMode mode)
{
    // VDD reuses the three AUX frame rows but has different scaling and only
    // one valid result per physical chip. The Open Ephys source currently
    // publishes physical auxiliary inputs only in AUX mode.
    return static_cast<size_t>(topology.totalChannels) +
           (mode == SampleMode::Aux
                ? static_cast<size_t>(topology.totalAuxChannels)
                : 0U);
}

struct Status
{
    uint32_t physicalChipMask = 0;
    uint32_t logicalStreamMask = 0;
    uint32_t packedChipIds = 0;
    uint16_t layoutId = 0;
    uint32_t streamError = 0;
    ControlState state = ControlState::Idle;
    bool streaming = false;
    bool routeUdp = false;
    bool auxVdd = false;
    bool scanValid = false;
    bool initialized = false;
    bool impedanceValid = false;
};

struct AcquisitionConfig
{
    uint32_t generation = 0;
    uint32_t sampleRateHz = 0;
    uint32_t flags = 0;
    uint32_t analogLowerMilliHz = 0;
    uint32_t analogUpperHz = 0;
    uint32_t dspRequestedMilliHz = 0;
    uint32_t dspActualMilliHz = 0;
    uint32_t dspCode = 0;
    uint32_t ttlSettleChannel = 0;
    bool initialized = false;
    bool dspEnabled = false;
    bool ttlSettleEnabled = false;
};

struct ProgressSnapshot
{
    uint32_t operation = 0;
    uint32_t state = 0;
    uint32_t resultId = 0;
    uint32_t completedUnits = 0;
    uint32_t totalUnits = 0;
    uint32_t status = 0;
    uint32_t failCount = 0;
    int32_t currentStream = -1;
    int32_t currentChannel = -1;
    int32_t currentCapRange = -1;
};

struct ResultDescriptor
{
    uint32_t resultId = 0;
    uint32_t resultType = 0;
    uint32_t state = 0;
    uint32_t operationStatus = 0;
    uint32_t availableRecords = 0;
    uint32_t totalRecords = 0;
    uint32_t recordBytes = 0;
    uint32_t failCount = 0;
};

struct ResultRecord
{
    std::array<uint32_t, 4> words{};
    uint32_t failCount = 0;
};

struct UdpHeader
{
    uint32_t magic = 0;
    uint8_t version = 0;
    uint8_t headerBytes = 0;
    uint16_t flags = 0;
    uint32_t sessionId = 0;
    uint32_t sequence = 0;
    uint64_t firstTimestamp = 0;
    uint16_t logicalStreamMask = 0;
    uint16_t frameBytes = 0;
    uint16_t frameCount = 0;
    uint16_t layoutId = 0;
};

struct DecodedFrame
{
    std::array<float, kMaxContinuousChannels> samples{};
    // Preserve the exact signed ADC counts for algorithms that must mirror
    // the FPGA fixed-point datapath.  `samples` is the Open Ephys uV view;
    // converting it back to counts would add an avoidable float round trip.
    std::array<int16_t, kMaxElectrodeChannels> amplifierCounts{};
    size_t channelCount = 0;
    size_t amplifierChannelCount = 0;
    size_t auxChannelCount = 0;
    uint64_t timestamp = 0;
    uint16_t ttl = 0;
    uint32_t sessionId = 0;
    uint32_t sequence = 0;
    uint32_t missingBefore = 0;
    uint64_t timestampMissingBefore = 0;
};

enum class DecodeDisposition
{
    Accepted,
    Duplicate,
    Late,
    Rejected,
    Fatal
};

struct ParserStats
{
    uint64_t packets = 0;
    uint64_t frames = 0;
    uint64_t rejectedPackets = 0;
    uint64_t lostPackets = 0;
    uint64_t duplicatePackets = 0;
    uint64_t latePackets = 0;
    uint64_t sequenceGaps = 0;
    uint64_t timestampGaps = 0;
    uint64_t timestampMissing = 0;
    uint64_t lengthErrors = 0;
    uint64_t headerErrors = 0;
    uint64_t flagErrors = 0;
    uint64_t frameErrors = 0;
    uint64_t inactiveSlotErrors = 0;
    uint64_t maskMismatches = 0;
    uint64_t layoutMismatches = 0;
    uint64_t sessionMismatches = 0;
    uint64_t timestampErrors = 0;
};

} // namespace nclp
