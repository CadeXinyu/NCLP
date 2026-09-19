#pragma once

#include "NCLPTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace nclp
{

// Decoder for the fixed NCLP UDP-v1 envelope. Datagram boundaries are part of
// the protocol: unlike the legacy parser, this class never concatenates UDP
// payloads or searches across packets for frame magic.
class FrameParser
{
public:
    FrameParser() = default;

    bool configure(const Topology& topology,
                   uint32_t sampleRateHz,
                   SampleMode mode,
                   std::string* error = nullptr);
    void reset();

    DecodeDisposition decodeDatagram(const uint8_t* data,
                                     size_t bytes,
                                     DecodedFrame& frame,
                                     std::string* error = nullptr);

    const ParserStats& stats() const { return stats_; }
    const Topology& topology() const { return topology_; }
    bool isConfigured() const { return configured_; }
    uint32_t sampleRateHz() const { return sampleRateHz_; }
    SampleMode sampleMode() const { return sampleMode_; }
    uint16_t expectedFlags() const { return expectedFlags_; }

    static bool parseHeader(const uint8_t* data,
                            size_t bytes,
                            UdpHeader& header,
                            std::string* error = nullptr);

private:
    DecodeDisposition reject(uint64_t& counter,
                             const std::string& message,
                             std::string* error);
    DecodeDisposition fatal(uint64_t& counter,
                            const std::string& message,
                            std::string* error);
    void acceptSequence(const UdpHeader& header, uint32_t missingBefore);

    Topology topology_{};
    uint32_t sampleRateHz_ = 0;
    SampleMode sampleMode_ = SampleMode::Aux;
    uint16_t expectedFlags_ = 0;
    bool configured_ = false;
    bool sessionLocked_ = false;
    bool hasTimestamp_ = false;
    uint32_t sessionId_ = 0;
    uint32_t expectedSequence_ = 0;
    uint32_t lastAcceptedSequence_ = 0;
    uint64_t lastAcceptedTimestamp_ = 0;
    ParserStats stats_{};
};

} // namespace nclp
