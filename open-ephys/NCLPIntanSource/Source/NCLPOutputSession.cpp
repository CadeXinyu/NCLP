#include "NCLPSession.h"
#include "NCLPSessionSupport.h"
#include <cmath>

namespace nclp
{
using namespace session_detail;

bool Session::readOutputs(std::string* error)
{
    if (!beginBusy("Read outputs", error)) return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    return readOutputsImpl(error);
}

bool Session::pollOutputStatus(std::string* error)
{
    // Background reads never advertise commandBusy or queue behind an action.
    // A user command can wait for an in-flight read, without disabling editors
    // on every polling tick.
    std::unique_lock<std::mutex> operationLock(operationMutex_, std::try_to_lock);
    if (!operationLock.owns_lock() || commandBusy_.load()) return false;
    bool statusOnly;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!snapshot_.connected) return false;
        statusOnly = snapshot_.outputs.settingsValid;
    }
    // Retry a failed full read on a later tick. Status-only replies cannot
    // establish settings that have never been read successfully.
    return readOutputsImpl(error, statusOnly);
}

bool Session::readOutputsImpl(std::string* error, bool statusOnly)
{
    OutputSnapshot next;
    uint32_t previousBaselineState = NCLP_RIPPLE_BASELINE_STATE_IDLE;
    uint32_t previousBaselineGeneration = 0;
    if (statusOnly) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        next = snapshot_.outputs;
        previousBaselineState = next.baselineState;
        previousBaselineGeneration = next.baselineGeneration;
    }
    auto query = [&](Command command, uint32_t section, Reply& reply) {
        CommandArgs args{};
        args[0] = section;
        std::string transportError;
        reply = commandClient_.request(command, args, 1500ms, &transportError);
        if (replySucceeded(reply, command)) return true;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            snapshot_.outputs.valid = false;
            if (!statusOnly) snapshot_.outputs.settingsValid = false;
        }
        const auto message = replyError("Read output state", reply, transportError);
        if (!statusOnly) setError(message, error);
        else if (error != nullptr) *error = message;
        return false;
    };
    Reply reply;
    if (!query(Command::RippleStatus, NCLP_RIPPLE_STATUS_RUNTIME, reply)) return false;
    next.rippleStatus = reply.data0; next.triggerCount = reply.data1;
    next.powerSampleCount = reply.data2; next.baselineState = reply.data3;
    if (!query(Command::RippleStatus, NCLP_RIPPLE_STATUS_SIGNAL, reply)) return false;
    next.meanSquare = reply.data0; next.sumSquareThresholdBits = reply.data1;
    next.lastInputTimestamp = reply.data2; next.baselineCollected = reply.data3;
    if (!query(Command::RippleStatus, NCLP_RIPPLE_STATUS_BASELINE, reply)) return false;
    next.baselineTargetSamples = reply.data0; next.baselineGeneration = reply.data1;
    next.baselineError = reply.data2; next.baselineMissedSamples = reply.data3;
    // Completion changes mu/sigma and a new collection changes the configured
    // duration. Fetch the three config pages once when either event appears.
    if (statusOnly && (next.baselineState != previousBaselineState ||
                       next.baselineGeneration != previousBaselineGeneration))
        return readOutputsImpl(error, false);
    if (!statusOnly) {
        // Read profile pages after baseline state. If collection completed near
        // this refresh, mu/sigma can only be as new as (or newer than) the
        // observed state, so completion cannot publish stale baseline values.
        if (!query(Command::RippleGetConfig, NCLP_RIPPLE_CONFIG_PROFILE, reply)) return false;
        next.detectedChannelIndex = reply.data0; next.filterKind = reply.data1;
        next.firTaps = reply.data2; next.windowUs = reply.data3;
        if (!query(Command::RippleGetConfig, NCLP_RIPPLE_CONFIG_BAND, reply)) return false;
        next.lowMilliHz = reply.data0; next.highMilliHz = reply.data1;
        next.refractoryMs = reply.data2; next.baselineDurationMs = reply.data3;
        if (!query(Command::RippleGetConfig, NCLP_RIPPLE_CONFIG_THRESHOLD, reply)) return false;
        next.muBits = reply.data0; next.sigmaBits = reply.data1;
        next.kBits = reply.data2; next.hardwareInputId = reply.data3;
        if (!query(Command::GetIntanSync, 0, reply)) return false;
        next.syncMode = reply.data0; next.syncPeriodFrames = reply.data1; next.syncHighFrames = reply.data2;
    }
    if (!query(Command::StimGet, NCLP_STIM_SECTION_LIVE, reply)) return false;
    next.stimStatus = reply.data0; next.safetyStatus = reply.data1;
    next.accepted = reply.data2; next.rejected = reply.data3;
    if (!statusOnly) {
        if (!query(Command::StimGet, NCLP_STIM_SECTION_ACTION, reply)) return false;
        next.actionMode = reply.data0 & 3U;
        next.externalTrigger = (reply.data0 & STIM_ACTION_CONFIG_EXTERNAL_TRIGGER_ENABLE) != 0;
        next.ttl0Source = reply.data1 & 3U; next.ttl1Source = (reply.data1 >> 2) & 3U;
        next.ttlWidthCycles = reply.data2; next.markerMask = reply.data3;
        if (!query(Command::StimGet, NCLP_STIM_SECTION_DAC_CONFIG_0, reply)) return false;
        next.dacChannels = reply.data0 & 3U;
        next.dacPeriod = reply.data1; next.dacStart = reply.data2; next.dacLoop = reply.data3;
        if (!query(Command::StimGet, NCLP_STIM_SECTION_DAC_CONFIG_1, reply)) return false;
        next.dacEnd = reply.data0; next.dacUpdates = reply.data1;
        next.dacClockConfig = reply.data2; next.dacClockStatus = reply.data3;
        if (!query(Command::StimGet, NCLP_STIM_SECTION_PRESET_0, reply)) return false;
        next.presetValid = reply.data0 != 0;
        next.presetKind = reply.data1; next.presetRateHz = reply.data2; next.presetParameter = reply.data3;
        if (!query(Command::StimGet, NCLP_STIM_SECTION_PRESET_1, reply)) return false;
        next.minimumAuv = reply.data0; next.maximumAuv = reply.data1;
        next.minimumBuv = reply.data2; next.maximumBuv = reply.data3;
        if (!query(Command::StimGet, NCLP_STIM_SECTION_PRESET_2, reply)) return false;
        next.presetChannels = reply.data0; next.presetRepeats = reply.data1;
    }
    if (!query(Command::StimGet, NCLP_STIM_SECTION_DAC_RUNTIME, reply)) return false;
    // A short board-button press can finish between polls. The retained count
    // catches it and requests one authoritative settings/status refresh.
    if (statusOnly && reply.data3 != next.buttonPressCount)
        return readOutputsImpl(error, false);
    next.buttonPressCount = reply.data3;
    next.primeStatus = reply.data0; next.completedUpdates = reply.data2;
    if (!query(Command::StimGet, NCLP_STIM_SECTION_ERRORS, reply)) return false;
    next.errors = reply.data0;
    next.valid = true;
    next.settingsValid = true;
    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.outputs = next;
    return true;
}

bool Session::sendOutputCommand(Command command, const CommandArgs& args, std::string* error)
{
    // Keep this API scoped to outputs; acquisition commands use their own state transitions.
    switch (command) {
        case Command::RippleApplyProfile: case Command::RippleBaseline:
        case Command::RippleControl: case Command::RippleSetK:
        case Command::SetIntanSync: case Command::StimSetAction:
        case Command::DacSetConfig: case Command::DacSetClock: case Command::DacSetIntanTtl:
        case Command::DacWrite: case Command::DacPreset:
        case Command::StimControl: case Command::StimDiagnostics: break;
        default: setError("Unsupported output command", error); return false;
    }
    if (command == Command::RippleControl && args[0] > NCLP_RIPPLE_ACTION_START) {
        setError("Ripple control action must be STOP or START", error); return false;
    }
    if (command == Command::RippleBaseline) {
        if (args[0] > NCLP_RIPPLE_BASELINE_START) {
            setError("Ripple baseline action must be CANCEL or START", error); return false;
        }
        if ((args[0] == NCLP_RIPPLE_BASELINE_CANCEL && args[1] != 0U) ||
            (args[0] == NCLP_RIPPLE_BASELINE_START &&
             (args[1] < NCLP_RIPPLE_BASELINE_DURATION_MS_MIN ||
              args[1] > NCLP_RIPPLE_BASELINE_DURATION_MS_MAX))) {
            setError("Ripple baseline duration is invalid for this action", error); return false;
        }
    }
    if (command == Command::RippleSetK &&
        (!std::isfinite(decodeFp32(args[0])) || decodeFp32(args[0]) < 0.0f)) {
        setError("K must be finite and nonnegative", error); return false;
    }
    if (!beginBusy("Output command", error)) return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    std::string transportError;
    const Reply reply = commandClient_.request(command, args, 5000ms, &transportError);
    const bool succeeded = replySucceeded(reply, command);
    const std::string commandError = succeeded ? std::string{} : replyError("Output command", reply, transportError);
    if (command == Command::StimControl) {
        // STIM_CONTROL already returns firmware's post-action live snapshot.
        // Consuming it directly avoids six extra status transactions before a
        // safety-priority DISARM queued behind ARM/Test can use the command
        // channel. A later timer poll refreshes unrelated ripple/DAC pages.
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (succeeded) {
                auto& output = snapshot_.outputs;
                output.stimStatus = reply.data0;
                output.primeStatus = reply.data1;
                output.accepted = reply.data2;
                output.rejected = reply.data3;
                if (args[0] == NCLP_STIM_ACTION_CLEAR)
                    output.errors = 0U;
            } else {
                // Early firmware rejections need not include a live snapshot.
                // Do not present the previous ARM state as authoritative.
                snapshot_.outputs.valid = false;
            }
        }
        if (!succeeded) {
            setError(commandError, error);
            return false;
        }
        clearError();
        return true;
    }
    // Refresh even after a rejection: ARM/configuration may have become locked
    // since the previous poll. A failed refresh must never look like current state.
    bool statusOnly = false;
    if (command == Command::StimControl || command == Command::RippleControl) {
        std::lock_guard<std::mutex> lock(stateMutex_);
        statusOnly = snapshot_.outputs.settingsValid;
    }
    const bool refreshed = readOutputsImpl(error, statusOnly);
    if (!succeeded) { setError(commandError, error); return false; }
    if (!refreshed) return false;
    clearError();
    return true;
}
bool Session::setRippleK(float k, std::string* error)
{
    if (!std::isfinite(k) || k < 0) { setError("K must be finite and nonnegative", error); return false; }
    CommandArgs args{}; args[0] = encodeFp32(k);
    return sendOutputCommand(Command::RippleSetK, args, error);
}
} // namespace nclp
