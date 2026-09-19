#include "NCLPRippleAnalysis.h"

#include "../../../software/common/nclp_wire.h"
#include "../../../software/main/src/drivers/ripple_filter_design.h"
#include "../../../programmable_logic/hls/ripple_detector/coefficients.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace nclp
{
namespace
{

constexpr uint32_t kHalfbandTaps = 11U;
constexpr uint32_t kDecimateFiveTaps = 120U;
constexpr uint32_t kMaximumFirTaps = 256U;
constexpr uint32_t kMaximumPowerWindowSamples = 30U;
constexpr uint32_t kIirCoefficientCount = 10U;
constexpr int32_t kIirStateFractionBits = 20;
constexpr double kPsdFloorDb = -120.0;
constexpr uint32_t kMinimumWelchFftSize = 16U;
constexpr uint32_t kMaximumWelchFftSize = 131072U;

std::mutex& filterDesignMutex()
{
    static std::mutex mutex;
    return mutex;
}

__extension__ typedef unsigned __int128 UInt128;

void setError(std::string* destination, const std::string& message)
{
    if (destination != nullptr)
        *destination = message;
}

bool isPowerOfTwo(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

bool validateProfile(const RippleAnalysisProfile& profile, std::string* error)
{
    if (profile.filterKind > NCLP_RIPPLE_FILTER_FIXED_IIR)
    {
        setError(error, "Unsupported ripple filter kind");
        return false;
    }
    if (profile.firTaps < NCLP_RIPPLE_FIR_TAPS_MIN ||
        profile.firTaps > NCLP_RIPPLE_FIR_TAPS_MAX)
    {
        setError(error, "Ripple FIR tap count must be in the range 3..256");
        return false;
    }
    if (profile.powerWindowSamples < 1U ||
        profile.powerWindowSamples > kMaximumPowerWindowSamples)
    {
        setError(error, "Ripple power window must contain 1..30 samples");
        return false;
    }
    if (profile.lowMilliHz == 0U ||
        profile.lowMilliHz >= profile.highMilliHz ||
        profile.highMilliHz >= 1500000U)
    {
        setError(error, "Ripple band must satisfy 0 < low < high < 1500 Hz");
        return false;
    }
    if (profile.filterKind == NCLP_RIPPLE_FILTER_FIXED_IIR &&
        (profile.lowMilliHz != NCLP_RIPPLE_FIXED_IIR_LOW_MILLIHZ ||
         profile.highMilliHz != NCLP_RIPPLE_FIXED_IIR_HIGH_MILLIHZ))
    {
        setError(error, "The fixed IIR supports only the 150..250 Hz band");
        return false;
    }
    return true;
}

bool validateRequest(const RippleAnalysisRequest& request, std::string* error)
{
    if (! validateProfile(request.profile, error))
        return false;
    if (request.durationMs < NCLP_RIPPLE_BASELINE_DURATION_MS_MIN ||
        request.durationMs > NCLP_RIPPLE_BASELINE_DURATION_MS_MAX)
    {
        setError(error, "Baseline duration must be in the range 1..600 seconds");
        return false;
    }
    if (! isPowerOfTwo(request.welchFftSize) ||
        request.welchFftSize < kMinimumWelchFftSize ||
        request.welchFftSize > kMaximumWelchFftSize)
    {
        setError(error, "Welch FFT size must be a power of two from 16 to 131072");
        return false;
    }
    return true;
}

int64_t roundShiftNearestEven(int64_t value, uint32_t shift)
{
    const bool negative = value < 0;
    const uint64_t magnitude = negative
        ? static_cast<uint64_t>(-(value + 1)) + 1U
        : static_cast<uint64_t>(value);
    uint64_t rounded = magnitude >> shift;
    const uint64_t remainder = magnitude & ((UINT64_C(1) << shift) - 1U);
    const uint64_t halfway = UINT64_C(1) << (shift - 1U);
    if (remainder > halfway ||
        (remainder == halfway && (rounded & 1U) != 0U))
        ++rounded;
    return negative ? -static_cast<int64_t>(rounded)
                    : static_cast<int64_t>(rounded);
}

int32_t saturateSigned(int64_t value, int64_t minimum, int64_t maximum)
{
    if (value < minimum)
        return static_cast<int32_t>(minimum);
    if (value > maximum)
        return static_cast<int32_t>(maximum);
    return static_cast<int32_t>(value);
}

uint32_t divideRoundNearestEven(uint64_t numerator, uint32_t denominator)
{
    if (denominator == 0U)
        return 0U;
    uint64_t quotient = numerator / denominator;
    const uint64_t remainder = numerator % denominator;
    const uint64_t twiceRemainder = remainder << 1U;
    if (twiceRemainder > denominator ||
        (twiceRemainder == denominator && (quotient & 1U) != 0U))
        ++quotient;
    return static_cast<uint32_t>(quotient);
}

uint64_t divideRoundNearestEven128(UInt128 numerator, uint64_t denominator)
{
    const UInt128 quotient = numerator / denominator;
    const UInt128 remainder = numerator % denominator;
    const UInt128 half = denominator / 2U;
    UInt128 rounded = quotient;
    if (remainder > half ||
        (((denominator & 1U) == 0U) && remainder == half &&
         (quotient & 1U) != 0U))
        ++rounded;
    return rounded > std::numeric_limits<uint64_t>::max()
        ? std::numeric_limits<uint64_t>::max()
        : static_cast<uint64_t>(rounded);
}

uint64_t integerSqrtFloor(uint64_t value)
{
    uint64_t result = 0U;
    uint64_t bit = UINT64_C(1) << 62U;
    while (bit > value)
        bit >>= 2U;
    while (bit != 0U)
    {
        if (value >= result + bit)
        {
            value -= result + bit;
            result = (result >> 1U) + bit;
        }
        else
        {
            result >>= 1U;
        }
        bit >>= 2U;
    }
    return result;
}

uint32_t integerSqrtRoundNearestEven(uint64_t value)
{
    uint64_t floorValue = integerSqrtFloor(value);
    const uint64_t floorSquare = floorValue * floorValue;
    const uint64_t lowerDistance = value - floorSquare;
    const uint64_t next = floorValue + 1U;
    const uint64_t upperDistance = next * next - value;
    if (upperDistance < lowerDistance ||
        (upperDistance == lowerDistance && (floorValue & 1U) != 0U))
        ++floorValue;
    return floorValue > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(floorValue);
}

template <size_t Capacity>
int64_t convolveRing(const std::array<int32_t, Capacity>& history,
                     const int32_t* coefficients,
                     uint32_t newest,
                     uint32_t taps)
{
    int64_t accumulator = 0;
    for (uint32_t lag = 0U; lag < taps; ++lag)
    {
        const uint32_t index = static_cast<uint32_t>(
            (newest + Capacity - lag) % Capacity);
        accumulator += static_cast<int64_t>(history[index]) *
                       static_cast<int64_t>(coefficients[lag]);
    }
    return accumulator;
}

void fftInPlace(std::vector<std::complex<double>>& values)
{
    const size_t count = values.size();
    for (size_t source = 1U, destination = 0U; source < count; ++source)
    {
        size_t bit = count >> 1U;
        while ((destination & bit) != 0U)
        {
            destination ^= bit;
            bit >>= 1U;
        }
        destination ^= bit;
        if (source < destination)
            std::swap(values[source], values[destination]);
    }

    const double pi = std::acos(-1.0);
    for (size_t length = 2U; length <= count; length <<= 1U)
    {
        const std::complex<double> step = std::polar(
            1.0, -2.0 * pi / static_cast<double>(length));
        const size_t half = length >> 1U;
        for (size_t base = 0U; base < count; base += length)
        {
            std::complex<double> twiddle(1.0, 0.0);
            for (size_t offset = 0U; offset < half; ++offset)
            {
                const std::complex<double> even = values[base + offset];
                const std::complex<double> odd =
                    twiddle * values[base + offset + half];
                values[base + offset] = even + odd;
                values[base + offset + half] = even - odd;
                twiddle *= step;
            }
        }
    }
}

bool calculateNormalizedWelchPsdImpl(
    const int16_t* rawSamples,
    size_t rawSampleCount,
    uint32_t sampleRateHz,
    uint32_t requestedFftSize,
    std::vector<double>& frequenciesHz,
    std::vector<double>& relativePowerDb,
    const RippleAnalysisCancel& shouldCancel,
    bool* cancelled,
    std::string* error)
{
    frequenciesHz.clear();
    relativePowerDb.clear();
    if (cancelled != nullptr)
        *cancelled = false;
    if (rawSamples == nullptr || rawSampleCount < kMinimumWelchFftSize ||
        sampleRateHz == 0U)
    {
        setError(error, "Not enough samples for a Welch PSD");
        return false;
    }
    if (! isPowerOfTwo(requestedFftSize) ||
        requestedFftSize < kMinimumWelchFftSize ||
        requestedFftSize > kMaximumWelchFftSize)
    {
        setError(error, "Welch FFT size must be a power of two from 16 to 131072");
        return false;
    }

    size_t fftSize = requestedFftSize;
    while (fftSize > rawSampleCount)
        fftSize >>= 1U;
    if (fftSize < kMinimumWelchFftSize)
    {
        setError(error, "Not enough samples for the requested Welch PSD");
        return false;
    }

    const size_t hop = fftSize / 2U;
    const size_t binCount = fftSize / 2U + 1U;
    std::vector<double> power(binCount, 0.0);
    std::vector<double> window(fftSize, 0.0);
    std::vector<std::complex<double>> spectrum(fftSize);
    const double pi = std::acos(-1.0);
    double windowSquareSum = 0.0;
    for (size_t index = 0U; index < fftSize; ++index)
    {
        window[index] = 0.5 - 0.5 * std::cos(
            2.0 * pi * static_cast<double>(index) /
            static_cast<double>(fftSize - 1U));
        windowSquareSum += window[index] * window[index];
    }

    size_t segmentCount = 0U;
    for (size_t base = 0U; base + fftSize <= rawSampleCount; base += hop)
    {
        if (shouldCancel && shouldCancel())
        {
            if (cancelled != nullptr)
                *cancelled = true;
            setError(error, "Ripple analysis was cancelled");
            return false;
        }
        double mean = 0.0;
        for (size_t index = 0U; index < fftSize; ++index)
            mean += static_cast<double>(rawSamples[base + index]);
        mean /= static_cast<double>(fftSize);
        for (size_t index = 0U; index < fftSize; ++index)
        {
            spectrum[index] = std::complex<double>(
                (static_cast<double>(rawSamples[base + index]) - mean) *
                    window[index],
                0.0);
        }
        fftInPlace(spectrum);
        const double scale = 1.0 /
            (static_cast<double>(sampleRateHz) * windowSquareSum);
        for (size_t bin = 0U; bin < binCount; ++bin)
        {
            double oneSided = std::norm(spectrum[bin]) * scale;
            if (bin != 0U && bin + 1U != binCount)
                oneSided *= 2.0;
            power[bin] += oneSided;
        }
        ++segmentCount;
    }

    if (segmentCount == 0U)
    {
        setError(error, "Not enough complete segments for a Welch PSD");
        return false;
    }
    for (double& value : power)
        value /= static_cast<double>(segmentCount);

    const auto maximum = std::max_element(power.begin() + 1, power.end());
    const double maximumPower = maximum == power.end() ? 0.0 : *maximum;
    frequenciesHz.reserve(binCount - 1U);
    relativePowerDb.reserve(binCount - 1U);
    for (size_t bin = 1U; bin < binCount; ++bin)
    {
        frequenciesHz.push_back(
            static_cast<double>(bin) * static_cast<double>(sampleRateHz) /
            static_cast<double>(fftSize));
        if (maximumPower > 0.0 && std::isfinite(maximumPower))
        {
            const double ratio = std::max(
                power[bin] / maximumPower, std::pow(10.0, kPsdFloorDb / 10.0));
            relativePowerDb.push_back(10.0 * std::log10(ratio));
        }
        else
        {
            relativePowerDb.push_back(kPsdFloorDb);
        }
    }
    return true;
}

} // namespace

struct FixedRippleAnalyzer::Impl
{
    explicit Impl(const RippleAnalysisProfile& requestedProfile)
        : profile(requestedProfile)
    {
        if (! validateProfile(profile, &errorMessage))
            return;

        if (profile.filterKind == NCLP_RIPPLE_FILTER_FIXED_IIR)
        {
            nclp_ripple_filter_fixed_iir(iirCoefficients.data());
        }
        else
        {
            // The shared firmware FIR designer uses internal scratch storage.
            // Serialize designs so independent plugin instances cannot race.
            std::lock_guard<std::mutex> lock(filterDesignMutex());
            if (nclp_ripple_filter_design_fir(
                    profile.filterKind,
                    profile.lowMilliHz,
                    profile.highMilliHz,
                    profile.firTaps,
                    rippleCoefficients.data()) !=
                NCLP_RIPPLE_FILTER_DESIGN_OK)
            {
                errorMessage = "Could not design the requested fixed-point ripple FIR";
                return;
            }
        }
        configured = true;
    }

    bool filterIir(int32_t input, int32_t& output)
    {
        int64_t value = static_cast<int64_t>(input) << kIirStateFractionBits;
        for (uint32_t section = 0U; section < 2U; ++section)
        {
            const uint32_t coefficient = section * 5U;
            const uint32_t history = section * 2U;
            int64_t accumulator =
                static_cast<int64_t>(iirCoefficients[coefficient]) * value;
            accumulator += static_cast<int64_t>(iirCoefficients[coefficient + 1U]) *
                           iirXHistory[history];
            accumulator += static_cast<int64_t>(iirCoefficients[coefficient + 2U]) *
                           iirXHistory[history + 1U];
            accumulator -= static_cast<int64_t>(iirCoefficients[coefficient + 3U]) *
                           iirYHistory[history];
            accumulator -= static_cast<int64_t>(iirCoefficients[coefficient + 4U]) *
                           iirYHistory[history + 1U];
            const int64_t next = roundShiftNearestEven(accumulator, 16U);
            if (next < -(INT64_C(1) << 39U) ||
                next > (INT64_C(1) << 39U) - 1)
            {
                configured = false;
                errorMessage = "Fixed-point ripple IIR state overflow";
                return false;
            }
            iirXHistory[history + 1U] = iirXHistory[history];
            iirXHistory[history] = value;
            iirYHistory[history + 1U] = iirYHistory[history];
            iirYHistory[history] = next;
            value = next;
        }
        output = saturateSigned(
            roundShiftNearestEven(value, kIirStateFractionBits),
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max());
        return true;
    }

    bool process(int16_t centredAdcCount, uint32_t& amplitudeQ16)
    {
        amplitudeQ16 = 0U;
        if (! configured)
            return false;
        ++rawSampleCount;

        halfbandHistory[halfbandPosition] = centredAdcCount;
        const uint32_t newestHalfband = halfbandPosition;
        halfbandPosition = halfbandPosition == kHalfbandTaps - 1U
            ? 0U : halfbandPosition + 1U;
        if (halfbandCount < kHalfbandTaps)
            ++halfbandCount;
        phaseTwo ^= 1U;
        if (phaseTwo != 0U)
            return false;

        const int64_t halfbandAccumulator = convolveRing(
            halfbandHistory,
            nclp_ripple::HALFBAND_COEFFICIENTS,
            newestHalfband,
            kHalfbandTaps);
        const int32_t intermediateQ2 = saturateSigned(
            roundShiftNearestEven(halfbandAccumulator, 15U),
            -(INT64_C(1) << 17U),
            (INT64_C(1) << 17U) - 1);
        if (halfbandCount < kHalfbandTaps)
            return false;

        decimateFiveHistory[decimateFivePosition] = intermediateQ2;
        const uint32_t newestDecimateFive = decimateFivePosition;
        decimateFivePosition = decimateFivePosition == kDecimateFiveTaps - 1U
            ? 0U : decimateFivePosition + 1U;
        if (decimateFiveCount < kDecimateFiveTaps)
            ++decimateFiveCount;
        phaseFive = phaseFive == 4U ? 0U : phaseFive + 1U;
        if (phaseFive != 0U)
            return false;

        const int64_t decimateFiveAccumulator = convolveRing(
            decimateFiveHistory,
            nclp_ripple::DECIMATE5_COEFFICIENTS,
            newestDecimateFive,
            kDecimateFiveTaps);
        const int32_t lowRateSample = saturateSigned(
            roundShiftNearestEven(decimateFiveAccumulator, 19U),
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max());
        if (decimateFiveCount < kDecimateFiveTaps)
            return false;

        int32_t rippleSample = 0;
        if (profile.filterKind != NCLP_RIPPLE_FILTER_FIXED_IIR)
        {
            rippleHistory[ripplePosition] = lowRateSample;
            const uint32_t newestRipple = ripplePosition;
            ripplePosition = ripplePosition == kMaximumFirTaps - 1U
                ? 0U : ripplePosition + 1U;
            if (rippleCount < profile.firTaps)
                ++rippleCount;
            const int64_t rippleAccumulator = convolveRing(
                rippleHistory,
                rippleCoefficients.data(),
                newestRipple,
                profile.firTaps);
            rippleSample = saturateSigned(
                roundShiftNearestEven(rippleAccumulator, 17U),
                std::numeric_limits<int16_t>::min(),
                std::numeric_limits<int16_t>::max());
            if (rippleCount < profile.firTaps)
                return false;
        }
        else if (! filterIir(lowRateSample, rippleSample))
        {
            return false;
        }

        const uint32_t newSquare = static_cast<uint32_t>(
            static_cast<int64_t>(rippleSample) * rippleSample);
        const uint32_t oldSquare = squareHistory[squarePosition];
        squareHistory[squarePosition] = newSquare;
        squareSum = squareSum - oldSquare + newSquare;
        squarePosition = squarePosition + 1U == profile.powerWindowSamples
            ? 0U : squarePosition + 1U;
        if (squareCount < profile.powerWindowSamples)
            ++squareCount;
        if (squareCount < profile.powerWindowSamples)
            return false;

        const uint32_t meanSquare = divideRoundNearestEven(
            squareSum, profile.powerWindowSamples);
        amplitudeQ16 = integerSqrtRoundNearestEven(
            static_cast<uint64_t>(meanSquare) << 32U);
        ++amplitudeSampleCount;
        return true;
    }

    RippleAnalysisProfile profile;
    bool configured = false;
    std::string errorMessage;
    std::array<int32_t, kHalfbandTaps> halfbandHistory{};
    std::array<int32_t, kDecimateFiveTaps> decimateFiveHistory{};
    std::array<int32_t, kMaximumFirTaps> rippleHistory{};
    std::array<int32_t, kMaximumFirTaps> rippleCoefficients{};
    std::array<int32_t, kIirCoefficientCount> iirCoefficients{};
    std::array<int64_t, 4U> iirXHistory{};
    std::array<int64_t, 4U> iirYHistory{};
    std::array<uint32_t, kMaximumPowerWindowSamples> squareHistory{};
    uint32_t halfbandPosition = 0U;
    uint32_t decimateFivePosition = 0U;
    uint32_t ripplePosition = 0U;
    uint32_t squarePosition = 0U;
    uint32_t halfbandCount = 0U;
    uint32_t decimateFiveCount = 0U;
    uint32_t rippleCount = 0U;
    uint32_t squareCount = 0U;
    uint32_t phaseTwo = 0U;
    uint32_t phaseFive = 0U;
    uint64_t squareSum = 0U;
    uint64_t rawSampleCount = 0U;
    uint64_t amplitudeSampleCount = 0U;
};

double RippleAnalysisResult::meanCounts() const noexcept
{
    return static_cast<double>(meanQ16) / 65536.0;
}

double RippleAnalysisResult::sigmaCounts() const noexcept
{
    return static_cast<double>(sigmaQ16) / 65536.0;
}

double RippleAnalysisResult::meanMicrovolts() const noexcept
{
    return meanCounts() * kRippleAnalysisMicrovoltsPerCount;
}

double RippleAnalysisResult::sigmaMicrovolts() const noexcept
{
    return sigmaCounts() * kRippleAnalysisMicrovoltsPerCount;
}

bool RippleAnalysisCapture::hasDetectedChannel(
    uint32_t detectedChannel) const noexcept
{
    const size_t index = static_cast<size_t>(detectedChannel);
    return success && index < samplesByDetectedChannel.size() &&
           index < globalChannelByDetectedChannel.size() &&
           samplesByDetectedChannel[index].size() == rawSampleCount;
}

uint32_t RippleAnalysisCapture::detectedChannelCount() const noexcept
{
    const size_t count = std::min(samplesByDetectedChannel.size(),
                                  globalChannelByDetectedChannel.size());
    return count > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max()
        : static_cast<uint32_t>(count);
}

FixedRippleAnalyzer::FixedRippleAnalyzer(const RippleAnalysisProfile& profile)
    : impl_(new Impl(profile))
{
}

FixedRippleAnalyzer::~FixedRippleAnalyzer() = default;
FixedRippleAnalyzer::FixedRippleAnalyzer(FixedRippleAnalyzer&&) noexcept = default;
FixedRippleAnalyzer& FixedRippleAnalyzer::operator=(FixedRippleAnalyzer&&) noexcept = default;

bool FixedRippleAnalyzer::valid() const noexcept
{
    return impl_ != nullptr && impl_->configured;
}

const std::string& FixedRippleAnalyzer::error() const noexcept
{
    static const std::string movedFromError = "Ripple analyzer has no state";
    return impl_ != nullptr ? impl_->errorMessage : movedFromError;
}

bool FixedRippleAnalyzer::process(int16_t centredAdcCount,
                                  uint32_t& amplitudeQ16)
{
    if (impl_ == nullptr)
    {
        amplitudeQ16 = 0U;
        return false;
    }
    return impl_->process(centredAdcCount, amplitudeQ16);
}

uint64_t FixedRippleAnalyzer::rawSamplesProcessed() const noexcept
{
    return impl_ != nullptr ? impl_->rawSampleCount : 0U;
}

uint64_t FixedRippleAnalyzer::amplitudeSamplesProduced() const noexcept
{
    return impl_ != nullptr ? impl_->amplitudeSampleCount : 0U;
}

uint64_t requiredRippleRawSamples(const RippleAnalysisRequest& request,
                                  std::string* error)
{
    if (! validateRequest(request, error))
        return 0U;
    const uint64_t targetAmplitudeSamples =
        static_cast<uint64_t>(request.durationMs) * 3U;
    const uint64_t rippleHistorySamples =
        request.profile.filterKind == NCLP_RIPPLE_FILTER_FIXED_IIR
            ? 1U : request.profile.firTaps;
    // The first fixed /5 output consumes 250 raw samples. Each later 3 kS/s
    // output consumes ten. Filling the selected filter and N-sample PWT window
    // gives 10*(M + N + T) + 220 raw samples, with T=1 for the stateful IIR.
    return 10U * (targetAmplitudeSamples +
                  request.profile.powerWindowSamples +
                  rippleHistorySamples) + 220U;
}

uint64_t requiredRippleCaptureBytes(const RippleAnalysisRequest& request,
                                    uint32_t detectedChannelCount,
                                    std::string* error)
{
    if (detectedChannelCount == 0U)
    {
        setError(error, "At least one detected amplifier channel is required");
        return 0U;
    }

    std::string requestError;
    const uint64_t rawSamples = requiredRippleRawSamples(request, &requestError);
    if (rawSamples == 0U)
    {
        setError(error, requestError);
        return 0U;
    }

    constexpr uint64_t bytesPerSample = sizeof(int16_t);
    if (rawSamples > std::numeric_limits<uint64_t>::max() /
                         bytesPerSample / detectedChannelCount)
    {
        setError(error, "All-channel baseline capture size overflowed");
        return 0U;
    }
    const uint64_t bytes = rawSamples * bytesPerSample * detectedChannelCount;
    if (bytes > kRippleAnalysisMaxCaptureBytes)
    {
        std::ostringstream message;
        message << "Baseline duration and " << detectedChannelCount
                << " detected channels require "
                << ((bytes + UINT64_C(1024) * 1024U - 1U) /
                    (UINT64_C(1024) * 1024U))
                << " MiB; the private capture limit is "
                << (kRippleAnalysisMaxCaptureBytes /
                    (UINT64_C(1024) * 1024U))
                << " MiB. Reduce the duration or detected channel count.";
        setError(error, message.str());
        return 0U;
    }
    if (error != nullptr)
        error->clear();
    return bytes;
}

RippleAnalysisResult analyzeRippleCapture(
    const RippleAnalysisRequest& request,
    const std::vector<int16_t>& rawSamples,
    RippleAnalysisCancel shouldCancel)
{
    RippleAnalysisResult result;
    std::string validationError;
    const uint64_t required = requiredRippleRawSamples(request, &validationError);
    if (required == 0U)
    {
        result.error = validationError;
        return result;
    }
    if (rawSamples.size() < required)
    {
        std::ostringstream message;
        message << "Baseline capture ended after " << rawSamples.size()
                << " raw samples; " << required << " are required";
        result.error = message.str();
        result.rawSampleCount = rawSamples.size();
        return result;
    }

    FixedRippleAnalyzer analyzer(request.profile);
    if (! analyzer.valid())
    {
        result.error = analyzer.error();
        return result;
    }

    const uint64_t targetAmplitudeSamples =
        static_cast<uint64_t>(request.durationMs) * 3U;
    uint64_t amplitudeSumQ16 = 0U;
    UInt128 amplitudeSquareSumQ32 = 0U;
    for (size_t index = 0U; index < rawSamples.size(); ++index)
    {
        if ((index & 0xffU) == 0U && shouldCancel && shouldCancel())
        {
            result.cancelled = true;
            result.error = "Ripple analysis was cancelled";
            result.rawSampleCount = analyzer.rawSamplesProcessed();
            result.amplitudeSampleCount = analyzer.amplitudeSamplesProduced();
            return result;
        }
        uint32_t amplitudeQ16 = 0U;
        if (analyzer.process(rawSamples[index], amplitudeQ16))
        {
            amplitudeSumQ16 += amplitudeQ16;
            amplitudeSquareSumQ32 +=
                static_cast<uint64_t>(amplitudeQ16) * amplitudeQ16;
            if (analyzer.amplitudeSamplesProduced() >= targetAmplitudeSamples)
                break;
        }
        if (! analyzer.valid())
        {
            result.error = analyzer.error();
            result.rawSampleCount = analyzer.rawSamplesProcessed();
            result.amplitudeSampleCount = analyzer.amplitudeSamplesProduced();
            return result;
        }
    }

    result.rawSampleCount = analyzer.rawSamplesProcessed();
    result.amplitudeSampleCount = analyzer.amplitudeSamplesProduced();
    if (result.amplitudeSampleCount != targetAmplitudeSamples)
    {
        std::ostringstream message;
        message << "Fixed-point pipeline produced "
                << result.amplitudeSampleCount << " of "
                << targetAmplitudeSamples << " requested baseline samples";
        result.error = message.str();
        return result;
    }

    const uint64_t meanQ16 = divideRoundNearestEven128(
        amplitudeSumQ16, targetAmplitudeSamples);
    const UInt128 varianceNumerator =
        static_cast<UInt128>(targetAmplitudeSamples) *
            amplitudeSquareSumQ32 -
        static_cast<UInt128>(amplitudeSumQ16) * amplitudeSumQ16;
    const uint64_t varianceQ32 = divideRoundNearestEven128(
        varianceNumerator,
        targetAmplitudeSamples * targetAmplitudeSamples);
    result.meanQ16 = static_cast<uint32_t>(meanQ16);
    result.sigmaQ16 = integerSqrtRoundNearestEven(varianceQ32);

    bool cancelled = false;
    if (! calculateNormalizedWelchPsdImpl(
            rawSamples.data(),
            static_cast<size_t>(result.rawSampleCount),
            kRippleAnalysisInputRateHz,
            request.welchFftSize,
            result.frequenciesHz,
            result.relativePowerDb,
            shouldCancel,
            &cancelled,
            &result.error))
    {
        result.cancelled = cancelled;
        return result;
    }
    result.success = true;
    return result;
}

RippleAnalysisResult analyzeRippleCapturedChannel(
    const RippleAnalysisCapture& capture,
    uint32_t detectedChannel,
    RippleAnalysisCancel shouldCancel)
{
    RippleAnalysisResult result;
    if (! capture.success)
    {
        result.cancelled = capture.cancelled;
        result.error = capture.error.empty()
            ? "The all-channel baseline capture is incomplete"
            : capture.error;
        result.rawSampleCount = capture.rawSampleCount;
        return result;
    }
    if (! capture.hasDetectedChannel(detectedChannel))
    {
        result.error = "Detected channel is not present in the baseline capture";
        result.rawSampleCount = capture.rawSampleCount;
        return result;
    }

    RippleAnalysisRequest request = capture.request;
    request.detectedChannel = detectedChannel;
    return analyzeRippleCapture(
        request,
        capture.samplesByDetectedChannel[static_cast<size_t>(detectedChannel)],
        std::move(shouldCancel));
}

bool calculateNormalizedWelchPsd(
    const std::vector<int16_t>& rawSamples,
    uint32_t sampleRateHz,
    uint32_t requestedFftSize,
    std::vector<double>& frequenciesHz,
    std::vector<double>& relativePowerDb,
    std::string* error)
{
    return calculateNormalizedWelchPsdImpl(
        rawSamples.data(), rawSamples.size(), sampleRateHz, requestedFftSize,
        frequenciesHz, relativePowerDb, {}, nullptr, error);
}

} // namespace nclp
