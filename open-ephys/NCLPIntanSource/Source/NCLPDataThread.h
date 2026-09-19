#pragma once

#include "NCLPRippleAnalysis.h"
#include "NCLPSession.h"

#include <DataThreadHeaders.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nclp
{

class DataThreadPlugin : public DataThread
{
public:
    using ScanProgressCallback = Session::ProgressCallback;

    /**
     * Shared lifetime gate for detached UI commands.
     *
     * A SourceNode destroys its DataThread before its editor.  UI workers must
     * therefore acquire this gate before dereferencing the plugin; destruction
     * invalidates the gate and waits for the current holder to finish.
     */
    class AsyncAccess final
    {
    public:
        bool run(const std::function<void(DataThreadPlugin&)>& work);
        bool tryRun(const std::function<void(DataThreadPlugin&)>& work);

    private:
        friend class DataThreadPlugin;

        explicit AsyncAccess(DataThreadPlugin* owner);
        void invalidateAndWait();

        std::mutex mutex_;
        DataThreadPlugin* owner_ = nullptr;
    };

    explicit DataThreadPlugin(SourceNode* sourceNode);
    ~DataThreadPlugin() override;

    bool updateBuffer() override;
    bool foundInputSource() override;
    bool startAcquisition() override;
    bool stopAcquisition() override;

    void updateSettings(OwnedArray<ContinuousChannel>* continuousChannels,
                        OwnedArray<EventChannel>* eventChannels,
                        OwnedArray<SpikeChannel>* spikeChannels,
                        OwnedArray<DataStream>* sourceStreams,
                        OwnedArray<DeviceInfo>* devices,
                        OwnedArray<ConfigurationObject>* configurationObjects) override;
    void registerParameters() override;
    void resizeBuffers() override;
    bool isReady() override;
    void parameterValueChanged(Parameter* parameter) override;
    String handleConfigMessage(const String& msg) override;
    std::unique_ptr<GenericEditor> createEditor(SourceNode* sn) override;

    bool connectDevice(bool printOutput = true);
    bool reconnectDevice(bool printOutput = true);
    void disconnectDevice();
    String checkConnection();
    String scanDevice(const ScanProgressCallback& progressCallback = {});
    String resetDevice();
    String initDevice();
    String setSampleRate(uint32_t sampleRateHz,
                         const ScanProgressCallback& progressCallback = {});
    String setBandwidth(uint32_t analogLowerMilliHz, uint32_t analogUpperHz);
    String setDsp(bool enabled, uint32_t requestedCutoffMilliHz);
    String setTtlFastSettle(bool enabled, uint32_t channel);
    String setAuxChannelsEnabled(bool enabled);
    bool auxChannelsEnabled() const;
    String readOutputs();
    void pollOutputStatus();
    String sendOutputCommand(Command command, const CommandArgs& args);
    String runImpedance();
    String cancelImpedance();
    /**
     * Privately record every detected amplifier channel without starting the
     * Open Ephys acquisition graph. The returned channel-major int16 capture
     * can be analyzed lazily as the user changes the displayed channel.
     *
     * This call is synchronous. Call it from a worker thread. Cancellation is
     * polled during capture; progress is reported in 30 kS/s time samples,
     * shared by all captured channels.
     */
    bool collectRippleAnalysis(
        const RippleAnalysisRequest& request,
        RippleAnalysisCapture& capture,
        const std::function<bool()>& cancel = {},
        const std::function<void(uint64_t, uint64_t)>& progress = {});

    /** Compatibility helper: capture all channels, then analyze the selected one. */
    bool collectRippleAnalysis(
        const RippleAnalysisRequest& request,
        RippleAnalysisResult& result,
        const std::function<bool()>& cancel = {},
        const std::function<void(uint64_t, uint64_t)>& progress = {});
    bool saveImpedances(const File& file, String* error = nullptr) const;
    String setChannelNamingScheme(int scheme, bool updateSignalChain = true);
    int channelNamingScheme() const { return channelNamingScheme_.load(); }

    bool isCommandBusy() const;
    String lastStatusText() const;
    SessionSnapshot sessionSnapshot() const;
    std::shared_ptr<AsyncAccess> asyncAccess() const { return asyncAccess_; }

private:
    enum class StreamOwner : uint8_t
    {
        None,
        OpenEphys,
        RippleAnalysis
    };

    struct PublishedTopologyIdentity
    {
        uint8_t physicalMask = 0;
        uint16_t logicalMask = 0;
        uint32_t packedChipIds = 0;
        uint32_t totalChannels = 0;
        uint32_t totalAuxChannels = 0;
        SampleMode sampleMode = SampleMode::Aux;
        bool valid = false;
    };

    void updateConfigFromParameters();
    void rejectAcquisitionStart(const std::string& message);
    void setLastStatus(const String& message, bool broadcast);
    void publishStatus(const std::string& message);
    String formatStatusSnapshot(const SessionSnapshot& snapshot) const;
    static String channelName(const ChannelAddress& channel, int namingScheme);
    static String auxChannelName(const AuxChannelAddress& channel);
    bool publishedTopologyMatches(const Topology& topology,
                                  SampleMode sampleMode) const;
    void clearPublishedTopology();
    void setPublishedTopology(const Topology& topology, SampleMode sampleMode);
    void recordOpenEphysFifoDepth(size_t samples);
    void reportDataGap(const QueuedFrame& frame);
    void beginHardwareStop(bool reportStatus);
    bool settleHardwareStop(bool waitForCompletion);
    bool acquireStream(StreamOwner owner);
    void releaseStream(StreamOwner owner);

    Session session_;

    std::string fpgaIp_ = kDefaultFpgaIp;
    std::string hostIp_ = kDefaultHostIp;
    std::atomic<bool> acquisitionRunning_{ false };
    std::atomic<StreamOwner> streamOwner_{ StreamOwner::None };
    // Also lets destruction cancel a synchronous analysis that currently
    // holds AsyncAccess::run while the destructor waits for that gate.
    std::atomic<bool> rippleAnalysisCancelRequested_{ false };
    std::atomic<bool> shuttingDown_{ false };
    std::atomic<size_t> publishedChannelCount_{ 0 };
    std::atomic<uint32_t> publishedSampleRateHz_{ 0 };
    // Acquisition Board-compatible IDs: 1 = Global, 2 = Stream-Based.
    std::atomic<int> channelNamingScheme_{ 1 };
    mutable std::mutex publishedTopologyMutex_;
    PublishedTopologyIdentity publishedTopology_;

    std::vector<float> sampleBlock_;
    std::vector<QueuedFrame> frameBatch_;
    std::vector<int64> sampleNumbers_;
    std::vector<double> timestamps_;
    std::vector<uint64> eventCodes_;
    uint64_t lastBufferedTimestamp_ = 0;
    bool hasBufferedTimestamp_ = false;
    std::atomic<size_t> openEphysFifoSamples_{ 0 };
    std::atomic<size_t> openEphysFifoHighWater_{ 0 };
    std::atomic<uint64_t> dataGapEvents_{ 0 };
    std::atomic<uint64_t> lastDataGapMissingSamples_{ 0 };
    std::atomic<uint64_t> lastDataGapBeforeTimestamp_{ 0 };
    // Written only by the DataThread; reset before that thread starts.
    std::chrono::steady_clock::time_point lastGapStatusTime_{};

    // resizeBuffers() runs on Open Ephys' graph-control path. Stop returns
    // before the producer thread has necessarily unwound, so every direct
    // sourceBuffers access must share this lifetime lock with resize/clear.
    mutable std::mutex sourceBufferMutex_;

    mutable std::mutex statusMutex_;
    String lastStatus_ = "Disconnected";
    std::shared_ptr<AsyncAccess> asyncAccess_;

    // Open Ephys calls stopAcquisition() on its graph-control path. Local
    // thread exit and firmware STOP/READY can outlive that callback, so keep
    // both waits off the caller and retain a joinable worker for safe
    // destruction/restart.
    std::mutex hardwareStopMutex_;
    std::thread hardwareStopThread_;
    std::atomic<bool> hardwareStopInProgress_{ false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DataThreadPlugin);
};

} // namespace nclp
