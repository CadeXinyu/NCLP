#include "NCLPSession.h"

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

#include "NCLPSessionSupport.h"

namespace nclp
{
using namespace session_detail;

bool detail::applyScanResultPhases(
    Topology& topology,
    const std::array<ResultRecord, kPhysicalLaneCount>& records,
    std::string* error)
{
    auto fail = [error](const std::string& message)
    {
        if (error != nullptr)
            *error = message;
        return false;
    };

    if (! topology.valid)
        return fail("Cannot apply SCAN phases without a valid topology");

    Topology decoded = topology;
    for (LaneDetection& lane : decoded.lanes)
        lane.selectedPhaseTap = -1;

    for (size_t index = 0; index < records.size(); ++index)
    {
        const ResultRecord& record = records[index];
        const uint32_t info = record.words[0];
        const uint32_t reportedIndex = info & 0x7U;
        const bool reportedDetected = (info & (1U << 3U)) != 0U;
        const bool supported = (info & (1U << 4U)) != 0U;
        const bool amplifiersMatch = (info & (1U << 5U)) != 0U;
        const uint8_t chipId = static_cast<uint8_t>((info >> 8U) & 0xffU);
        const uint32_t amplifierCount = (info >> 16U) & 0xffU;
        const uint32_t selectedPhase = record.words[1];
        const uint32_t logicalMask = record.words[3];
        const LaneDetection& expected = topology.lanes[index];
        const std::string prefix = "SCAN result " + std::to_string(index) + " ";

        if (reportedIndex != index)
            return fail(prefix + "reports the wrong physical lane index");
        if (reportedDetected != expected.detected)
            return fail(prefix + "detection flag does not match the active topology");

        if (! reportedDetected)
        {
            if (logicalMask != 0U)
                return fail(prefix + "has a logical mask for an undetected lane");
            // Firmware retains one record for every lane. Its phase word is
            // meaningless when detected=0 and must never become a UI value.
            continue;
        }

        uint32_t expectedLogicalMask = 1U << (2U * index);
        if (expected.chip == ChipType::RHD2164)
            expectedLogicalMask |= 1U << (2U * index + 1U);

        if (! supported || ! amplifiersMatch || chipId != expected.chipId ||
            amplifierCount != expected.channelCount)
        {
            return fail(prefix + "chip metadata does not match the active topology");
        }
        if (logicalMask != expectedLogicalMask)
            return fail(prefix + "logical mask does not match the active topology");
        if (selectedPhase > kCablePhaseTapLast)
            return fail(prefix + "phase is outside the 0..15 hardware range");

        decoded.lanes[index].selectedPhaseTap =
            static_cast<int32_t>(selectedPhase);
    }

    topology = std::move(decoded);
    if (error != nullptr)
        error->clear();
    return true;
}


bool detail::validateImpedanceRecords(
    const Topology& topology,
    const std::vector<ImpedanceRecord>& records,
    std::string* error)
{
    auto fail = [error](const std::string& message)
    {
        if (error != nullptr)
            *error = message;
        return false;
    };

    if (! topology.valid)
        return fail("Cannot validate impedance results without a valid topology");
    if (topology.totalChannels > kMaxElectrodeChannels ||
        topology.channels.size() != topology.totalChannels)
    {
        return fail("Active topology has an invalid global channel map");
    }

    std::array<const ChannelAddress*, kMaxElectrodeChannels> expectedByGlobal{};
    std::array<bool, kMaxElectrodeChannels> topologyAddressSeen{};
    constexpr size_t channelsPerPhysicalLane = 64U;

    for (const ChannelAddress& channel : topology.channels)
    {
        if (channel.globalChannel >= topology.totalChannels)
            return fail("Active topology contains an out-of-range global channel");
        if (channel.physicalIndex >= kPhysicalLaneCount)
            return fail("Active topology contains an out-of-range physical headstage");

        const LaneDetection& lane = topology.lanes[channel.physicalIndex];
        if (! lane.detected || channel.localChannel >= lane.channelCount ||
            channel.localChannel >= channelsPerPhysicalLane)
        {
            return fail("Active topology contains an out-of-range local channel");
        }
        if (expectedByGlobal[channel.globalChannel] != nullptr)
            return fail("Active topology contains a duplicate global channel");

        const size_t physicalLocalIndex =
            channel.physicalIndex * channelsPerPhysicalLane + channel.localChannel;
        if (topologyAddressSeen[physicalLocalIndex])
            return fail("Active topology contains a duplicate physical/local channel");

        expectedByGlobal[channel.globalChannel] = &channel;
        topologyAddressSeen[physicalLocalIndex] = true;
    }

    std::array<bool, kMaxElectrodeChannels> resultGlobalSeen{};
    std::array<bool, kMaxElectrodeChannels> resultAddressSeen{};
    for (size_t index = 0; index < records.size(); ++index)
    {
        const ImpedanceRecord& record = records[index];
        const std::string prefix = "Impedance result " + std::to_string(index) + " ";

        if (record.globalChannel >= topology.totalChannels)
            return fail(prefix + "has an out-of-range global channel");
        if (record.physicalIndex >= kPhysicalLaneCount)
            return fail(prefix + "has an out-of-range physical headstage");

        const LaneDetection& lane = topology.lanes[record.physicalIndex];
        if (! lane.detected || record.localChannel >= lane.channelCount ||
            record.localChannel >= channelsPerPhysicalLane)
        {
            return fail(prefix + "has an out-of-range local channel");
        }
        if (record.capRange >= 3U)
            return fail(prefix + "has an out-of-range capacitance range");

        const ChannelAddress* expected = expectedByGlobal[record.globalChannel];
        if (expected == nullptr ||
            expected->physicalIndex != record.physicalIndex ||
            expected->localChannel != record.localChannel)
        {
            return fail(prefix + "does not match the active global/physical/local map");
        }
        if (resultGlobalSeen[record.globalChannel])
            return fail(prefix + "duplicates a global channel");

        const size_t physicalLocalIndex =
            record.physicalIndex * channelsPerPhysicalLane + record.localChannel;
        if (resultAddressSeen[physicalLocalIndex])
            return fail(prefix + "duplicates a physical/local channel");

        resultGlobalSeen[record.globalChannel] = true;
        resultAddressSeen[physicalLocalIndex] = true;
    }

    if (error != nullptr)
        error->clear();
    return true;
}

bool Session::runImpedance(std::string* error)
{
    if (! operationAllowed("Impedance test", error) ||
        ! beginBusy("Impedance test", error))
        return false;

    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (impedanceThread_.joinable())
        impedanceThread_.join();

    if (! ensureConnected(error) || ! ensureInitialized(error))
    {
        endBusy();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.impedanceResults.clear();
        snapshot_.progress = {};
        snapshot_.impedanceRunning = true;
        snapshot_.cancelRequested = false;
        snapshot_.lastError.clear();
    }
    cancelRequested_ = false;
    impedanceRunning_ = true;
    impedanceThread_ = std::thread(&Session::impedanceLoop, this);
    return true;
}

void Session::impedanceLoop()
{
    std::string operationError;
    auto finalReply = std::async(std::launch::async, [this, &operationError] {
        return commandClient_.impedance(20min, &operationError);
    });

    bool cancelSent = false;
    while (finalReply.wait_for(200ms) != std::future_status::ready)
    {
        if (cancelRequested_.load() && ! cancelSent)
        {
            std::string cancelError;
            const Reply cancelReply = commandClient_.cancel(1500ms, &cancelError);
            cancelSent = replySucceeded(cancelReply, Command::Cancel) ||
                         (replyIsValidFor(cancelReply, Command::Cancel) &&
                          cancelReply.status ==
                              static_cast<uint32_t>(CommandStatus::NotActive));
            if (! cancelSent && ! cancelError.empty())
                setError("CANCEL failed: " + cancelError);
        }

        std::string progressError;
        Reply progressReply{};
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            progressReply = commandClient_.getProgress(1000ms, &progressError);
            if (replySucceeded(progressReply, Command::GetProgress) ||
                ! replyIsValidFor(progressReply, Command::GetProgress) ||
                progressReply.status != static_cast<uint32_t>(CommandStatus::SnapshotBusy))
                break;
            std::this_thread::sleep_for(10ms);
        }
        if (replySucceeded(progressReply, Command::GetProgress))
        {
            const ProgressSnapshot progress = CommandClient::progressFromReply(progressReply);
            std::lock_guard<std::mutex> lock(stateMutex_);
            snapshot_.progress = progress;
        }
    }

    const Reply reply = finalReply.get();
    const bool operationSucceeded = replySucceeded(reply, Command::Impedance);
    const bool cancelled = replyIsValidFor(reply, Command::Impedance) &&
        reply.status == static_cast<uint32_t>(CommandStatus::Cancelled);
    std::string resultError;
    bool resultOk = true;
    if ((operationSucceeded || cancelled) && reply.data0 != 0)
        resultOk = fetchImpedanceResults(reply.data0, &resultError);

    std::string refreshError;
    (void) refreshStatusAndConfiguration(&refreshError);

    if (! operationSucceeded && ! cancelled)
        setError(replyError("IMPEDANCE", reply, operationError));
    else if (! resultOk && ! cancelled)
        setError(resultError.empty() ? "Could not read impedance results" : resultError);
    else
        clearError();

    impedanceRunning_ = false;
    cancelRequested_ = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.impedanceRunning = false;
        snapshot_.cancelRequested = false;
    }
    endBusy();
}

bool Session::fetchScanPhases(uint32_t resultId, std::string* error)
{
    Topology activeTopology;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        activeTopology = snapshot_.topology;
    }
    if (! activeTopology.valid)
    {
        setError("Cannot read SCAN phases without a valid active topology", error);
        return false;
    }

    auto readStableResult = [this](uint32_t requestedResultId,
                                   uint32_t index,
                                   Reply& reply,
                                   std::string& transportError)
    {
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            transportError.clear();
            reply = commandClient_.getResult(ResultType::Scan,
                                             requestedResultId,
                                             index,
                                             1500ms,
                                             &transportError);
            if (! replyIsValidFor(reply, Command::GetResult) ||
                reply.status !=
                    static_cast<uint32_t>(CommandStatus::SnapshotBusy))
                return;
            std::this_thread::sleep_for(10ms);
        }
    };

    std::string transportError;
    Reply descriptorReply{};
    readStableResult(resultId, kResultIndexMetadata,
                     descriptorReply, transportError);
    if (! replySucceeded(descriptorReply, Command::GetResult))
    {
        setError(replyError("GET_RESULT SCAN metadata", descriptorReply,
                            transportError),
                 error);
        return false;
    }

    const ResultDescriptor descriptor =
        CommandClient::resultDescriptorFromReply(descriptorReply);
    if (descriptor.resultId == 0U ||
        (resultId != 0U && descriptor.resultId != resultId) ||
        descriptor.resultType != static_cast<uint32_t>(ResultType::Scan) ||
        descriptor.state != static_cast<uint32_t>(ResultState::Complete) ||
        descriptor.operationStatus !=
            static_cast<uint32_t>(CommandStatus::Ok) ||
        descriptor.availableRecords != kPhysicalLaneCount ||
        descriptor.totalRecords != kPhysicalLaneCount ||
        descriptor.recordBytes != sizeof(uint32_t) * 4U)
    {
        setError("Firmware returned an invalid completed SCAN result descriptor",
                 error);
        return false;
    }

    std::array<ResultRecord, kPhysicalLaneCount> records{};
    for (size_t index = 0U; index < records.size(); ++index)
    {
        Reply recordReply{};
        readStableResult(descriptor.resultId, static_cast<uint32_t>(index),
                         recordReply, transportError);
        if (! replySucceeded(recordReply, Command::GetResult))
        {
            setError(replyError("GET_RESULT SCAN record", recordReply,
                                transportError),
                     error);
            return false;
        }
        records[index] = CommandClient::resultRecordFromReply(recordReply);
    }

    std::string decodeError;
    if (! detail::applyScanResultPhases(activeTopology, records, &decodeError))
    {
        setError(decodeError, error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (! sameTopologyIdentity(snapshot_.topology, activeTopology))
        {
            const std::string message =
                "Headstage topology changed while SCAN phases were downloaded";
            snapshot_.lastError = message;
            if (error != nullptr)
                *error = message;
            return false;
        }
        snapshot_.topology = std::move(activeTopology);
    }
    return true;
}

bool Session::fetchImpedanceResults(uint32_t resultId, std::string* error)
{
    Topology activeTopology;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        activeTopology = snapshot_.topology;
    }
    if (! activeTopology.valid ||
        activeTopology.channels.size() != activeTopology.totalChannels)
    {
        if (error != nullptr)
            *error = "Cannot read impedance results without a valid active topology";
        return false;
    }

    std::string transportError;
    Reply descriptorReply{};
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        transportError.clear();
        descriptorReply = commandClient_.getResult(ResultType::Impedance,
                                                    resultId,
                                                    kResultIndexMetadata,
                                                    1500ms,
                                                    &transportError);
        if (! replyIsValidFor(descriptorReply, Command::GetResult) ||
            descriptorReply.status != static_cast<uint32_t>(CommandStatus::SnapshotBusy))
            break;
        std::this_thread::sleep_for(10ms);
    }
    if (! replySucceeded(descriptorReply, Command::GetResult))
    {
        if (error != nullptr)
            *error = replyError("GET_RESULT metadata", descriptorReply, transportError);
        return false;
    }

    const ResultDescriptor descriptor =
        CommandClient::resultDescriptorFromReply(descriptorReply);
    if (descriptor.resultId != resultId ||
        descriptor.resultType != static_cast<uint32_t>(ResultType::Impedance) ||
        descriptor.recordBytes != sizeof(uint32_t) * 4U ||
        descriptor.availableRecords > kMaxElectrodeChannels ||
        descriptor.availableRecords > descriptor.totalRecords ||
        descriptor.totalRecords != activeTopology.totalChannels)
    {
        if (error != nullptr)
            *error = "Firmware returned an invalid impedance result descriptor";
        return false;
    }

    std::vector<ImpedanceRecord> records;
    records.reserve(descriptor.availableRecords);
    for (uint32_t index = 0; index < descriptor.availableRecords; ++index)
    {
        Reply recordReply{};
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            transportError.clear();
            recordReply = commandClient_.getResult(ResultType::Impedance,
                                                   resultId,
                                                   index,
                                                   1500ms,
                                                   &transportError);
            if (! replyIsValidFor(recordReply, Command::GetResult) ||
                recordReply.status != static_cast<uint32_t>(CommandStatus::SnapshotBusy))
                break;
            std::this_thread::sleep_for(10ms);
        }
        if (! replySucceeded(recordReply, Command::GetResult))
        {
            if (error != nullptr)
                *error = replyError("GET_RESULT record", recordReply, transportError);
            return false;
        }

        const ResultRecord raw = CommandClient::resultRecordFromReply(recordReply);
        const uint32_t info = raw.words[0];
        ImpedanceRecord record{};
        record.globalChannel = info & 0x1ffU;
        record.localChannel = (info >> 9U) & 0x3fU;
        record.physicalIndex = (info >> 15U) & 0x7U;
        record.capRange = (info >> 18U) & 0x3U;
        record.valid = (info & (1U << 20U)) != 0;
        record.timestampError = (info & (1U << 21U)) != 0;
        record.verifyError = (info & (1U << 22U)) != 0;
        record.saturated = (info & (1U << 23U)) != 0;
        record.magnitudeMilliohms = static_cast<uint64_t>(raw.words[1]) |
                                    (static_cast<uint64_t>(raw.words[2]) << 32U);
        record.phaseMicrodegrees = static_cast<int32_t>(raw.words[3]);
        records.push_back(record);
    }

    if (! detail::validateImpedanceRecords(activeTopology, records, error))
        return false;

    std::sort(records.begin(), records.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.globalChannel < rhs.globalChannel;
    });
    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.impedanceResults = std::move(records);
    return true;
}

bool Session::cancelImpedance(std::string* error)
{
    if (! impedanceRunning_.load())
    {
        if (error != nullptr)
            *error = "No impedance test is active";
        return false;
    }
    cancelRequested_ = true;
    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.cancelRequested = true;
    return true;
}

void Session::joinImpedanceThread(bool disconnectClient)
{
    if (! impedanceThread_.joinable())
        return;

    cancelRequested_ = true;
    if (disconnectClient)
    {
        if (commandClient_.isConnected())
        {
            std::string ignored;
            (void) commandClient_.cancel(500ms, &ignored);
        }
        commandClient_.disconnect();
    }
    impedanceThread_.join();
}

} // namespace nclp
