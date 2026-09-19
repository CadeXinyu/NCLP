#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nclp
{

constexpr uint32_t kRippleAnalysisInputRateHz = 30000U;
constexpr uint32_t kRippleAnalysisOutputRateHz = 3000U;
constexpr double kRippleAnalysisMicrovoltsPerCount = 0.195;
// A private all-channel recording must remain bounded even though firmware
// accepts baseline durations up to ten minutes. The limit covers exact int16
// ADC payload only; container overhead is negligible by comparison.
constexpr uint64_t kRippleAnalysisMaxCaptureBytes =
    UINT64_C(512) * 1024U * 1024U;

struct RippleAnalysisProfile
{
    uint32_t filterKind = 0U;
    uint32_t firTaps = 129U;
    uint32_t lowMilliHz = 150000U;
    uint32_t highMilliHz = 250000U;
    uint32_t powerWindowSamples = 12U;
};

struct RippleAnalysisRequest
{
    uint32_t detectedChannel = 0U;
    uint32_t durationMs = 10000U;
    uint32_t welchFftSize = 4096U;
    RippleAnalysisProfile profile{};
};

struct RippleAnalysisResult
{
    bool success = false;
    bool cancelled = false;
    std::string error;
    uint32_t meanQ16 = 0U;
    uint32_t sigmaQ16 = 0U;
    uint64_t rawSampleCount = 0U;
    uint64_t amplitudeSampleCount = 0U;
    std::vector<double> frequenciesHz;
    std::vector<double> relativePowerDb;

    double meanCounts() const noexcept;
    double sigmaCounts() const noexcept;
    double meanMicrovolts() const noexcept;
    double sigmaMicrovolts() const noexcept;
};

/**
 * One synchronized private recording from every detected amplifier channel.
 *
 * The outer vector uses zero-based detected-channel order, matching the
 * detector profile and UI selector. Each inner vector contains exact signed
 * Intan ADC counts; no microvolt/float conversion occurs in the capture path.
 * Analysis is intentionally lazy so a large topology does not run a complete
 * FIR/PWT/PSD pipeline for channels the user never views.
 */
struct RippleAnalysisCapture
{
    bool success = false;
    bool cancelled = false;
    std::string error;
    RippleAnalysisRequest request{};
    uint64_t rawSampleCount = 0U;
    // Global amplifier index for each detected-channel vector. This pins the
    // scan mapping used by the recording even if the live topology later changes.
    std::vector<uint32_t> globalChannelByDetectedChannel;
    std::vector<std::vector<int16_t>> samplesByDetectedChannel;

    bool hasDetectedChannel(uint32_t detectedChannel) const noexcept;
    uint32_t detectedChannelCount() const noexcept;
};

/**
 * Software model of the detector's integer sample path.
 *
 * Input samples are signed, centred Intan ADC counts at exactly 30 kS/s. A
 * successful process() return supplies one Q16.16 RMS-amplitude sample after
 * both fixed decimators, the selected ripple filter, and a completely filled
 * PWT window. A false return normally means that no 3 kS/s output was due yet;
 * callers should also check valid() for a numeric or configuration failure.
 */
class FixedRippleAnalyzer
{
public:
    explicit FixedRippleAnalyzer(const RippleAnalysisProfile& profile);
    ~FixedRippleAnalyzer();

    FixedRippleAnalyzer(FixedRippleAnalyzer&&) noexcept;
    FixedRippleAnalyzer& operator=(FixedRippleAnalyzer&&) noexcept;
    FixedRippleAnalyzer(const FixedRippleAnalyzer&) = delete;
    FixedRippleAnalyzer& operator=(const FixedRippleAnalyzer&) = delete;

    bool valid() const noexcept;
    const std::string& error() const noexcept;
    bool process(int16_t centredAdcCount, uint32_t& amplitudeQ16);
    uint64_t rawSamplesProcessed() const noexcept;
    uint64_t amplitudeSamplesProduced() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Return the exact number of 30 kS/s samples needed to fill all filter/PWT
 * histories and then produce durationMs*3 full-window amplitude samples.
 * Returns zero and optionally sets error when the request is invalid.
 */
uint64_t requiredRippleRawSamples(const RippleAnalysisRequest& request,
                                  std::string* error = nullptr);

/** Return exact int16 payload bytes, rejecting captures above the host cap. */
uint64_t requiredRippleCaptureBytes(const RippleAnalysisRequest& request,
                                    uint32_t detectedChannelCount,
                                    std::string* error = nullptr);

using RippleAnalysisCancel = std::function<bool()>;

/** Analyze a captured, centred int16 channel without changing board state. */
RippleAnalysisResult analyzeRippleCapture(
    const RippleAnalysisRequest& request,
    const std::vector<int16_t>& rawSamples,
    RippleAnalysisCancel shouldCancel = {});

/** Lazily analyze one detected channel retained in an all-channel capture. */
RippleAnalysisResult analyzeRippleCapturedChannel(
    const RippleAnalysisCapture& capture,
    uint32_t detectedChannel,
    RippleAnalysisCancel shouldCancel = {});

/**
 * Calculate a one-sided Welch PSD from centred ADC counts. DC is omitted, so
 * every returned frequency is positive and suitable for a logarithmic x axis.
 * Power is returned in dB relative to the largest positive-frequency bin.
 */
bool calculateNormalizedWelchPsd(
    const std::vector<int16_t>& rawSamples,
    uint32_t sampleRateHz,
    uint32_t requestedFftSize,
    std::vector<double>& frequenciesHz,
    std::vector<double>& relativePowerDb,
    std::string* error = nullptr);

} // namespace nclp
