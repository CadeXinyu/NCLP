#pragma once
#include "NCLPSession.h"
#include <algorithm>
#include <cerrno>
#include <cstring>

namespace nclp::session_detail
{

using namespace std::chrono_literals;

class ScopeExit
{
public:
    explicit ScopeExit(std::function<void()> callback)
        : callback_(std::move(callback))
    {
    }

    ~ScopeExit()
    {
        if (callback_)
            callback_();
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    std::function<void()> callback_;
};

inline std::string errnoMessage(const char* operation)
{
    return std::string(operation) + ": " + std::strerror(errno);
}

inline const char* statusName(uint32_t status)
{
    switch (static_cast<CommandStatus>(status))
    {
        case CommandStatus::Ok: return "OK";
        case CommandStatus::HardwareFailure: return "hardware failure";
        case CommandStatus::Prerequisite: return "prerequisite not met";
        case CommandStatus::InvalidArgument: return "invalid argument";
        case CommandStatus::Cancelled: return "cancelled";
        case CommandStatus::Busy: return "busy";
        case CommandStatus::Network: return "network error";
        case CommandStatus::NoResult: return "no result";
        case CommandStatus::StaleResult: return "stale result";
        case CommandStatus::ResultRange: return "result index out of range";
        case CommandStatus::SnapshotBusy: return "snapshot busy";
        case CommandStatus::NotActive: return "not active";
        case CommandStatus::NoChipDetected: return "no supported RHD chip detected";
        case CommandStatus::UnknownCommand: return "unknown command";
    }
    return "unknown status";
}

inline bool isSupportedSampleRate(uint32_t sampleRateHz)
{
    return std::find(kSupportedSampleRatesHz.begin(),
                     kSupportedSampleRatesHz.end(),
                     sampleRateHz) != kSupportedSampleRatesHz.end();
}

inline bool replyIsValidFor(const Reply& reply, Command command)
{
    return reply.magic == kCommandMagic &&
           reply.version == kCommandVersion &&
           reply.command == static_cast<uint32_t>(command) &&
           reply.sequence != 0U;
}

inline bool replySucceeded(const Reply& reply, Command command)
{
    return replyIsValidFor(reply, command) &&
           reply.status == static_cast<uint32_t>(CommandStatus::Ok);
}

inline bool sameProgress(const ProgressSnapshot& first,
                  const ProgressSnapshot& second)
{
    return first.operation == second.operation &&
           first.state == second.state &&
           first.resultId == second.resultId &&
           first.completedUnits == second.completedUnits &&
           first.totalUnits == second.totalUnits &&
           first.status == second.status &&
           first.failCount == second.failCount &&
           first.currentStream == second.currentStream &&
           first.currentChannel == second.currentChannel &&
           first.currentCapRange == second.currentCapRange;
}

inline bool sameTopologyIdentity(const Topology& first, const Topology& second)
{
    return first.valid && second.valid &&
           first.physicalMask == second.physicalMask &&
           first.logicalMask == second.logicalMask &&
           first.layoutId == second.layoutId &&
           first.packedChipIds == second.packedChipIds;
}

}
