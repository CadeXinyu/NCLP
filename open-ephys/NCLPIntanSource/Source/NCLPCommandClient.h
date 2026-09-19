#pragma once

#include "NCLPTypes.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace nclp
{

// Persistent, sequence-multiplexed TCP client for the production NCLP control
// endpoint. request() is safe to call concurrently: one thread may wait for a
// long IMPEDANCE reply while another polls GET_PROGRESS or submits CANCEL.
class CommandClient
{
public:
    explicit CommandClient(std::string fpgaIp = kDefaultFpgaIp,
                           std::string hostIp = kDefaultHostIp,
                           uint16_t commandPort = kFpgaCommandPort);
    ~CommandClient();

    void setFpgaIp(const std::string& fpgaIp);
    void setHostIp(const std::string& hostIp);
    std::string fpgaIp() const;
    std::string hostIp() const;

    bool connect(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000),
                 std::string* error = nullptr);
    bool reconnect(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000),
                   std::string* error = nullptr);
    void disconnect();
    bool isConnected() const;
    std::string banner() const;

    Reply request(Command command,
                  const CommandArgs& args,
                  std::chrono::milliseconds timeout,
                  std::string* error = nullptr);
    Reply request(Command command,
                  std::chrono::milliseconds timeout,
                  std::string* error = nullptr);

    Reply ping(std::chrono::milliseconds timeout, std::string* error = nullptr);
    Reply getStatus(std::chrono::milliseconds timeout, std::string* error = nullptr);
    Reply getConfig(ConfigSection section,
                    std::chrono::milliseconds timeout,
                    std::string* error = nullptr);
    Reply setUdpDestination(uint32_t ipv4,
                            uint16_t port = kHostDataPort,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds(1000),
                            std::string* error = nullptr);
    Reply setRate(uint32_t sampleRateHz,
                  std::chrono::milliseconds timeout,
                  std::string* error = nullptr);
    Reply setBandwidth(uint32_t analogLowerMilliHz,
                       uint32_t analogUpperHz,
                       std::chrono::milliseconds timeout,
                       std::string* error = nullptr);
    Reply setDsp(bool enabled,
                 uint32_t requestedCutoffMilliHz,
                 std::chrono::milliseconds timeout,
                 std::string* error = nullptr);
    Reply setTtlSettle(bool enabled,
                       uint32_t channel,
                       std::chrono::milliseconds timeout,
                       std::string* error = nullptr);
    Reply scan(std::chrono::milliseconds timeout, std::string* error = nullptr);
    Reply init(uint32_t stream = kInitFirstDetectedStream,
               bool vddSense = true,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(60000),
               std::string* error = nullptr);
    Reply impedance(std::chrono::milliseconds timeout, std::string* error = nullptr);
    Reply reset(std::chrono::milliseconds timeout, std::string* error = nullptr);
    Reply cancel(std::chrono::milliseconds timeout, std::string* error = nullptr);
    Reply startStream(SampleMode mode,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(2000),
                      std::string* error = nullptr);
    Reply stopStream(std::chrono::milliseconds timeout, std::string* error = nullptr);
    Reply getProgress(std::chrono::milliseconds timeout, std::string* error = nullptr);
    Reply getResult(ResultType type,
                    uint32_t resultId,
                    uint32_t index,
                    std::chrono::milliseconds timeout,
                    std::string* error = nullptr);

    static Status statusFromReply(const Reply& reply);
    static bool configurationFromReplies(const Reply& core,
                                         const Reply& filters,
                                         const Reply& ttl,
                                         AcquisitionConfig& config,
                                         std::string* error = nullptr);
    static ProgressSnapshot progressFromReply(const Reply& reply);
    static ResultDescriptor resultDescriptorFromReply(const Reply& reply);
    static ResultRecord resultRecordFromReply(const Reply& reply);

private:
    struct PendingReply
    {
        std::mutex mutex;
        std::condition_variable condition;
        Reply reply{};
        std::string error;
        uint32_t expectedCommand = 0;
        bool complete = false;
    };

    bool connectSocket(std::chrono::milliseconds timeout, std::string* error);
    bool readBanner(int socketFd,
                    std::chrono::steady_clock::time_point deadline,
                    std::string& banner,
                    std::string* error);
    bool sendAll(int socketFd,
                 const uint8_t* bytes,
                 size_t count,
                 std::chrono::steady_clock::time_point deadline,
                 std::string* error);
    void readerLoop(int socketFd);
    void readerFailed(int socketFd, const std::string& message);
    void failAllPending(const std::string& message);
    uint32_t nextSequence();

    mutable std::mutex stateMutex_;
    std::mutex writeMutex_;
    std::mutex pendingMutex_;
    std::string fpgaIp_;
    std::string hostIp_;
    uint16_t commandPort_ = kFpgaCommandPort;
    int socketFd_ = -1;
    std::thread readerThread_;
    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> connected_{ false };
    std::atomic<uint32_t> nextSequence_{ 1U };
    std::string banner_;
    std::unordered_map<uint32_t, std::shared_ptr<PendingReply>> pending_;
};

} // namespace nclp
