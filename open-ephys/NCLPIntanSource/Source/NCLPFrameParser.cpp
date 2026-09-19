#include "NCLPFrameParser.h"

#include <algorithm>

namespace nclp
{
namespace
{

uint16_t readLe16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8U);
}

uint32_t readLe32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) |
           (static_cast<uint32_t>(bytes[3]) << 24U);
}

uint64_t readLe64(const uint8_t* bytes)
{
    return static_cast<uint64_t>(readLe32(bytes)) |
           (static_cast<uint64_t>(readLe32(bytes + 4U)) << 32U);
}

void setError(std::string* destination, const std::string& message)
{
    if (destination != nullptr)
        *destination = message;
}

bool supportedChipId(uint8_t chipId)
{
    return chipId == kChipIdRHD2132 || chipId == kChipIdRHD2216 ||
           chipId == kChipIdRHD2164;
}

} // namespace

bool Topology::fromStatus(uint32_t physicalMaskValue,
                          uint32_t logicalMaskValue,
                          uint32_t packedIds,
                          uint16_t layout,
                          Topology& destination,
                          std::string* error)
{
    destination = Topology{};
    if ((physicalMaskValue & ~0xffU) != 0U)
    {
        setError(error, "physical headstage mask has bits above A1..D2");
        return false;
    }
    if ((logicalMaskValue & ~0xffffU) != 0U)
    {
        setError(error, "logical stream mask has bits above slot 15");
        return false;
    }

    uint16_t derivedLogicalMask = 0;
    uint32_t globalBase = 0;
    for (size_t physical = 0; physical < kPhysicalLaneCount; ++physical)
    {
        const bool selected = (physicalMaskValue & (1U << physical)) != 0U;
        const uint8_t chipId = static_cast<uint8_t>(
            (packedIds >> (physical * 4U)) & 0x0fU);
        LaneDetection lane{};
        lane.chipId = chipId;
        lane.chip = static_cast<ChipType>(chipId);
        lane.channelCount = channelCountForChipId(chipId);
        lane.detected = selected && lane.channelCount != 0U;
        destination.lanes[physical] = lane;

        if (! selected)
        {
            if (chipId != kChipIdNone)
            {
                setError(error, "packed chip ID is nonzero for an unselected physical lane");
                return false;
            }
            continue;
        }
        if (! supportedChipId(chipId) || lane.channelCount == 0U)
        {
            setError(error, "selected physical lane has an unsupported chip ID");
            return false;
        }

        const uint8_t primarySlot = static_cast<uint8_t>(2U * physical);
        uint16_t headstageLogicalMask = static_cast<uint16_t>(1U << primarySlot);
        if (lane.chip == ChipType::RHD2164)
            headstageLogicalMask = static_cast<uint16_t>(
                headstageLogicalMask | (1U << (primarySlot + 1U)));
        derivedLogicalMask = static_cast<uint16_t>(derivedLogicalMask |
                                                   headstageLogicalMask);

        HeadstageDescriptor headstage{};
        headstage.physicalIndex = physical;
        headstage.portIndex = physical / 2U;
        headstage.slotInPort = physical % 2U;
        headstage.chip = lane.chip;
        headstage.channelCount = lane.channelCount;
        headstage.globalChannelBase = globalBase;
        headstage.logicalMask = headstageLogicalMask;
        destination.headstages.push_back(headstage);

        // AUX1/2/3 are physical-chip signals. RHD2164 exposes its valid AUX
        // replies on MISO-A only, so map all three rows to the primary slot and
        // never duplicate them on the adjacent module-B logical stream.
        for (size_t auxInput = 0;
             auxInput < kAuxInputsPerHeadstage;
             ++auxInput)
        {
            AuxChannelAddress auxAddress{};
            auxAddress.globalAuxChannel = destination.auxChannels.size();
            auxAddress.physicalIndex = physical;
            auxAddress.logicalSlot = primarySlot;
            auxAddress.auxInput = static_cast<uint8_t>(auxInput);
            auxAddress.frameRow = static_cast<uint8_t>(kAmplifierRows + auxInput);
            destination.auxChannels.push_back(auxAddress);
        }

        for (uint32_t localChannel = 0;
             localChannel < lane.channelCount;
             ++localChannel)
        {
            ChannelAddress address{};
            address.globalChannel = globalBase + localChannel;
            address.physicalIndex = physical;
            address.localChannel = localChannel;
            address.logicalSlot = static_cast<uint8_t>(
                primarySlot + ((lane.chip == ChipType::RHD2164 &&
                                localChannel >= 32U) ? 1U : 0U));
            // RTL emits amplifier replies in rows 0..31, followed by AUX1..3
            // in rows 32..34. RHD2164 module B restarts at amplifier row zero.
            address.amplifierRow = static_cast<uint8_t>(
                localChannel % kAmplifierRows);
            destination.channels.push_back(address);
        }
        globalBase += lane.channelCount;
    }

    if (derivedLogicalMask != static_cast<uint16_t>(logicalMaskValue))
    {
        setError(error, "logical stream mask does not match the detected chip topology");
        return false;
    }
    if (destination.channels.size() > kMaxElectrodeChannels)
    {
        setError(error, "detected topology exceeds the 512-channel limit");
        return false;
    }
    if (destination.auxChannels.size() > kMaxAuxChannels)
    {
        setError(error, "detected topology exceeds the AUX-channel limit");
        return false;
    }

    destination.physicalMask = static_cast<uint8_t>(physicalMaskValue);
    destination.logicalMask = static_cast<uint16_t>(logicalMaskValue);
    destination.layoutId = layout;
    destination.packedChipIds = packedIds;
    destination.totalChannels = globalBase;
    destination.totalAuxChannels = static_cast<uint32_t>(
        destination.auxChannels.size());
    destination.valid = true;
    return true;
}

bool FrameParser::configure(const Topology& topology,
                            uint32_t sampleRateHz,
                            SampleMode mode,
                            std::string* error)
{
    const uint16_t rateFlag = sampleRateFlag(sampleRateHz);
    if (! topology.valid || topology.logicalMask == 0U ||
        topology.totalChannels == 0U || topology.channels.empty())
    {
        setError(error, "cannot configure UDP decoder with an empty or invalid topology");
        return false;
    }
    if (topology.totalChannels != topology.channels.size() ||
        topology.totalChannels > kMaxElectrodeChannels)
    {
        setError(error, "topology channel map is inconsistent");
        return false;
    }
    if (topology.totalAuxChannels != topology.auxChannels.size() ||
        topology.totalAuxChannels > kMaxAuxChannels ||
        topology.totalAuxChannels !=
            topology.headstages.size() * kAuxInputsPerHeadstage)
    {
        setError(error, "topology AUX channel map is inconsistent");
        return false;
    }
    std::array<std::array<bool, kAuxInputsPerHeadstage>,
               kPhysicalLaneCount> auxAddressSeen{};
    for (size_t index = 0; index < topology.auxChannels.size(); ++index)
    {
        const AuxChannelAddress& address = topology.auxChannels[index];
        if (address.globalAuxChannel != index ||
            address.physicalIndex >= kPhysicalLaneCount ||
            ! topology.lanes[address.physicalIndex].detected ||
            address.logicalSlot != 2U * address.physicalIndex ||
            address.auxInput >= kAuxInputsPerHeadstage ||
            address.frameRow != kAmplifierRows + address.auxInput ||
            (topology.logicalMask & (1U << address.logicalSlot)) == 0U)
        {
            setError(error, "topology AUX channel address is inconsistent");
            return false;
        }
        if (auxAddressSeen[address.physicalIndex][address.auxInput])
        {
            setError(error, "topology AUX channel address is duplicated");
            return false;
        }
        auxAddressSeen[address.physicalIndex][address.auxInput] = true;
    }
    if (rateFlag == 0U)
    {
        setError(error, "unsupported NCLP sample rate");
        return false;
    }
    if (mode != SampleMode::Aux && mode != SampleMode::Vdd)
    {
        setError(error, "unsupported NCLP sample mode");
        return false;
    }

    topology_ = topology;
    sampleRateHz_ = sampleRateHz;
    sampleMode_ = mode;
    expectedFlags_ = static_cast<uint16_t>(rateFlag | sampleModeFlag(mode));
    configured_ = true;
    reset();
    return true;
}

void FrameParser::reset()
{
    stats_ = ParserStats{};
    sessionLocked_ = false;
    hasTimestamp_ = false;
    sessionId_ = 0;
    expectedSequence_ = 0;
    lastAcceptedSequence_ = 0;
    lastAcceptedTimestamp_ = 0;
}

bool FrameParser::parseHeader(const uint8_t* data,
                              size_t bytes,
                              UdpHeader& header,
                              std::string* error)
{
    header = UdpHeader{};
    if (data == nullptr || bytes < kUdpHeaderBytes)
    {
        setError(error, "NCLP UDP datagram is shorter than its 32-byte header");
        return false;
    }
    header.magic = readLe32(data + 0U);
    header.version = data[4U];
    header.headerBytes = data[5U];
    header.flags = readLe16(data + 6U);
    header.sessionId = readLe32(data + 8U);
    header.sequence = readLe32(data + 12U);
    header.firstTimestamp = readLe64(data + 16U);
    header.logicalStreamMask = readLe16(data + 24U);
    header.frameBytes = readLe16(data + 26U);
    header.frameCount = readLe16(data + 28U);
    header.layoutId = readLe16(data + 30U);
    return true;
}

DecodeDisposition FrameParser::reject(uint64_t& counter,
                                      const std::string& message,
                                      std::string* error)
{
    ++counter;
    ++stats_.rejectedPackets;
    setError(error, message);
    return DecodeDisposition::Rejected;
}

DecodeDisposition FrameParser::fatal(uint64_t& counter,
                                     const std::string& message,
                                     std::string* error)
{
    ++counter;
    ++stats_.rejectedPackets;
    setError(error, message);
    return DecodeDisposition::Fatal;
}

void FrameParser::acceptSequence(const UdpHeader& header, uint32_t missingBefore)
{
    if (! sessionLocked_)
    {
        sessionLocked_ = true;
        sessionId_ = header.sessionId;
    }
    if (missingBefore != 0U)
    {
        ++stats_.sequenceGaps;
        stats_.lostPackets += missingBefore;
    }
    lastAcceptedSequence_ = header.sequence;
    expectedSequence_ = header.sequence + 1U;
}

DecodeDisposition FrameParser::decodeDatagram(const uint8_t* data,
                                               size_t bytes,
                                               DecodedFrame& frame,
                                               std::string* error)
{
    frame = DecodedFrame{};
    ++stats_.packets;

    if (! configured_)
        return fatal(stats_.headerErrors, "NCLP UDP decoder is not configured", error);
    if (data == nullptr || bytes != kUdpDatagramBytes)
        return reject(stats_.lengthErrors,
                      "NCLP UDP datagram must be exactly " +
                          std::to_string(kUdpDatagramBytes) + " bytes",
                      error);

    UdpHeader header{};
    if (! parseHeader(data, bytes, header, error))
        return reject(stats_.headerErrors, "malformed NCLP UDP header", error);
    if (header.magic != kUdpMagic || header.version != kUdpVersion ||
        header.headerBytes != kUdpHeaderBytes || header.sessionId == 0U)
        return reject(stats_.headerErrors, "bad NCLP UDP magic, version, header size, or session", error);
    if (header.frameBytes != kUdpFrameBytes || header.frameCount != 1U ||
        static_cast<size_t>(header.headerBytes) +
            static_cast<size_t>(header.frameBytes) * header.frameCount != bytes)
        return reject(stats_.lengthErrors, "bad NCLP UDP frame size or count", error);
    if (header.flags != expectedFlags_)
    {
        if (sessionLocked_)
            return fatal(stats_.flagErrors,
                         "NCLP UDP mode/sample-rate flags changed", error);
        return reject(stats_.flagErrors,
                      "NCLP UDP mode/sample-rate flags do not match configuration", error);
    }
    if (header.logicalStreamMask != topology_.logicalMask)
        return fatal(stats_.maskMismatches, "NCLP UDP logical stream mask changed", error);
    if (header.layoutId != topology_.layoutId)
        return fatal(stats_.layoutMismatches, "NCLP UDP layout ID changed", error);
    if (sessionLocked_ && header.sessionId != sessionId_)
        return fatal(stats_.sessionMismatches, "NCLP UDP session ID changed", error);

    uint32_t missingBefore = 0;
    if (! sessionLocked_)
    {
        missingBefore = header.sequence;
    }
    else
    {
        const uint32_t sequenceDelta = header.sequence - expectedSequence_;
        if ((sequenceDelta & 0x80000000U) != 0U)
        {
            if (header.sequence == lastAcceptedSequence_)
            {
                ++stats_.duplicatePackets;
                setError(error, "duplicate NCLP UDP datagram");
                return DecodeDisposition::Duplicate;
            }
            ++stats_.latePackets;
            setError(error, "late or reordered NCLP UDP datagram");
            return DecodeDisposition::Late;
        }
        missingBefore = sequenceDelta;
    }

    const uint8_t* normalizedFrame = data + kUdpHeaderBytes;
    if (! std::equal(kFrameMagicBytes.begin(), kFrameMagicBytes.end(),
                     normalizedFrame))
    {
        return reject(stats_.frameErrors, "NCLP normalized frame magic is invalid", error);
    }
    const uint32_t embeddedTimestamp = readLe32(normalizedFrame + 8U);
    if (embeddedTimestamp != static_cast<uint32_t>(header.firstTimestamp))
        return reject(stats_.timestampErrors, "NCLP header and frame timestamps disagree", error);

    for (size_t row = 0; row < kFrameRows; ++row)
    {
        const uint8_t* rowBytes = normalizedFrame + kFramePrefixBytes +
            row * kLogicalStreamSlots * kFrameSlotBytes;
        for (size_t slot = 0; slot < kLogicalStreamSlots; ++slot)
        {
            if ((topology_.logicalMask & (1U << slot)) == 0U &&
                readLe16(rowBytes + slot * kFrameSlotBytes) != 0U)
                return reject(stats_.inactiveSlotErrors,
                              "inactive NCLP normalized stream slot is nonzero", error);
        }
    }

    uint64_t timestampMissingBefore = 0;
    if (hasTimestamp_)
    {
        const uint64_t expectedTimestamp = lastAcceptedTimestamp_ + 1U;
        if (header.firstTimestamp < expectedTimestamp)
            return fatal(stats_.timestampErrors,
                         "NCLP frame timestamp moved backwards", error);
        if (header.firstTimestamp > expectedTimestamp)
        {
            timestampMissingBefore = header.firstTimestamp - expectedTimestamp;
            ++stats_.timestampGaps;
            stats_.timestampMissing += timestampMissingBefore;
        }
    }

    frame.amplifierChannelCount = topology_.totalChannels;
    frame.auxChannelCount = sampleMode_ == SampleMode::Aux
        ? topology_.totalAuxChannels
        : 0U;
    frame.channelCount = frame.amplifierChannelCount + frame.auxChannelCount;
    frame.timestamp = header.firstTimestamp;
    frame.ttl = readLe16(normalizedFrame + kUdpFrameBytes - kFrameTailBytes);
    frame.sessionId = header.sessionId;
    frame.sequence = header.sequence;
    frame.missingBefore = missingBefore;
    frame.timestampMissingBefore = timestampMissingBefore;
    for (const ChannelAddress& address : topology_.channels)
    {
        const size_t wordOffset = kFramePrefixBytes +
            (static_cast<size_t>(address.amplifierRow) * kLogicalStreamSlots +
             address.logicalSlot) * kFrameSlotBytes;
        const uint16_t raw = readLe16(normalizedFrame + wordOffset);
        const int16_t signedCount = static_cast<int16_t>(
            static_cast<int32_t>(raw) - 32768);
        frame.amplifierCounts[address.globalChannel] = signedCount;
        frame.samples[address.globalChannel] =
            static_cast<float>(signedCount) * kDefaultBitVolts;
    }
    if (sampleMode_ == SampleMode::Aux)
    {
        for (const AuxChannelAddress& address : topology_.auxChannels)
        {
            const size_t wordOffset = kFramePrefixBytes +
                (static_cast<size_t>(address.frameRow) * kLogicalStreamSlots +
                 address.logicalSlot) * kFrameSlotBytes;
            const uint16_t raw = readLe16(normalizedFrame + wordOffset);
            const size_t outputChannel = topology_.totalChannels +
                                         address.globalAuxChannel;
            frame.samples[outputChannel] =
                static_cast<float>(static_cast<int32_t>(raw) - 32768) *
                kAuxBitVolts;
        }
    }

    acceptSequence(header, missingBefore);
    hasTimestamp_ = true;
    lastAcceptedTimestamp_ = header.firstTimestamp;
    ++stats_.frames;
    return DecodeDisposition::Accepted;
}

} // namespace nclp
