#pragma once
#include "NCLPTypes.h"
#include <cstring>
#include "../../../software/common/nclp_pl_registers.h"

namespace nclp
{
inline float decodeFp32(uint32_t bits) { float value; std::memcpy(&value, &bits, sizeof value); return value; }
inline uint32_t encodeFp32(float value) { uint32_t bits; std::memcpy(&bits, &value, sizeof bits); return bits; }
// Readback values, never a cache of requested settings. Each section is read
// serially; counters describe nearby instants, not a cross-IP atomic snapshot.
struct OutputSnapshot
{
    bool valid = false;
    // A failed status poll must not discard known settings or make a later
    // status-only recovery look like an initial settings load.
    bool settingsValid = false;
    uint32_t rippleStatus = 0, triggerCount = 0, powerSampleCount = 0;
    uint32_t baselineState = NCLP_RIPPLE_BASELINE_STATE_IDLE;
    uint32_t detectedChannelIndex = 0, filterKind = NCLP_RIPPLE_FILTER_MINIMUM_FIR;
    uint32_t firTaps = 0, windowUs = 0;
    uint32_t lowMilliHz = 0, highMilliHz = 0, refractoryMs = 0;
    uint32_t baselineDurationMs = NCLP_RIPPLE_BASELINE_DURATION_MS_DEFAULT;
    uint32_t muBits = 0, sigmaBits = 0, kBits = 0;
    uint32_t hardwareInputId = 0;
    uint32_t meanSquare = 0, sumSquareThresholdBits = 0, lastInputTimestamp = 0;
    uint32_t baselineCollected = 0, baselineTargetSamples = 0;
    uint32_t baselineGeneration = 0, baselineError = 0, baselineMissedSamples = 0;
    uint32_t syncMode = 0, syncPeriodFrames = 0, syncHighFrames = 0;
    uint32_t stimStatus = 0, safetyStatus = 0, accepted = 0, rejected = 0;
    uint32_t actionMode = 0, ttl0Source = 0, ttl1Source = 0, ttlWidthCycles = 0, markerMask = 0;
    bool externalTrigger = false;
    uint32_t dacChannels = 0, dacPeriod = 0, dacStart = 0, dacLoop = 0, dacEnd = 0, dacUpdates = 0;
    uint32_t dacClockConfig = 0, dacClockStatus = 0, primeStatus = 0, completedUpdates = 0;
    bool presetValid = false;
    uint32_t presetKind = 0, presetRateHz = 0, presetParameter = 0;
    uint32_t minimumAuv = 0, maximumAuv = 0, minimumBuv = 0, maximumBuv = 0;
    uint32_t presetChannels = 0, presetRepeats = 0;
    uint32_t errors = 0;
    uint32_t buttonPressCount = 0;
    bool armed() const { return (stimStatus & STIM_STATUS_ARMED) != 0; }
    bool busy() const { return (stimStatus & STIM_STATUS_BUSY) != 0; }
    bool locked() const { return (stimStatus & STIM_STATUS_CONFIG_LOCKED) != 0; }
    bool safeOff() const { return (safetyStatus & STIM_SAFETY_STATUS_SAFE_OFF_ACTIVE) != 0; }
    bool rippleEnabled() const { return (rippleStatus & RIPPLE_DETECTOR_STATUS_ENABLED) != 0; }
    bool rippleBusy() const { return (rippleStatus & RIPPLE_DETECTOR_STATUS_BUSY) != 0; }
    bool rippleRateValid() const { return (rippleStatus & RIPPLE_DETECTOR_STATUS_RATE_30K_VALID) != 0; }
    bool baselineCollecting() const { return baselineState == NCLP_RIPPLE_BASELINE_STATE_COLLECTING; }
};
}
