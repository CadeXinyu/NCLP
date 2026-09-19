#include "NCLPCommandClient.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace nclp
{
namespace
{

uint32_t readLe32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) |
           (static_cast<uint32_t>(bytes[3]) << 24U);
}

void writeLe32(uint8_t* bytes, uint32_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8U);
    bytes[2] = static_cast<uint8_t>(value >> 16U);
    bytes[3] = static_cast<uint8_t>(value >> 24U);
}

void setError(std::string* destination, const std::string& message)
{
    if (destination != nullptr)
        *destination = message;
}

std::string errnoMessage(const std::string& prefix)
{
    return prefix + ": " + std::strerror(errno);
}

int pollTimeoutMs(std::chrono::steady_clock::time_point deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
        return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    return static_cast<int>(std::min<int64_t>(
        std::max<int64_t>(remaining.count(), 1),
        std::numeric_limits<int>::max()));
}

} // namespace

CommandClient::CommandClient(std::string fpgaIp,
                             std::string hostIp,
                             uint16_t commandPort)
    : fpgaIp_(std::move(fpgaIp)),
      hostIp_(std::move(hostIp)),
      commandPort_(commandPort)
{
}

CommandClient::~CommandClient()
{
    disconnect();
}

void CommandClient::setFpgaIp(const std::string& fpgaIp)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    fpgaIp_ = fpgaIp;
}

void CommandClient::setHostIp(const std::string& hostIp)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    hostIp_ = hostIp;
}

std::string CommandClient::fpgaIp() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return fpgaIp_;
}

std::string CommandClient::hostIp() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return hostIp_;
}

bool CommandClient::connect(std::chrono::milliseconds timeout, std::string* error)
{
    disconnect();
    return connectSocket(timeout, error);
}

bool CommandClient::reconnect(std::chrono::milliseconds timeout, std::string* error)
{
    return connect(timeout, error);
}

bool CommandClient::connectSocket(std::chrono::milliseconds timeout,
                                  std::string* error)
{
    std::string addressText;
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        addressText = fpgaIp_;
        port = commandPort_;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, addressText.c_str(), &address.sin_addr) != 1)
    {
        setError(error, "invalid FPGA IPv4 address: " + addressText);
        return false;
    }

    const int socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0)
    {
        setError(error, errnoMessage("TCP control socket"));
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const int originalFlags = ::fcntl(socketFd, F_GETFL, 0);
    if (originalFlags < 0 ||
        ::fcntl(socketFd, F_SETFL, originalFlags | O_NONBLOCK) != 0)
    {
        setError(error, errnoMessage("configure TCP control socket"));
        ::close(socketFd);
        return false;
    }

    int connectionResult = ::connect(socketFd,
                                     reinterpret_cast<sockaddr*>(&address),
                                     sizeof(address));
    if (connectionResult != 0 && errno != EINPROGRESS)
    {
        setError(error, errnoMessage("connect TCP control"));
        ::close(socketFd);
        return false;
    }
    if (connectionResult != 0)
    {
        pollfd descriptor{};
        descriptor.fd = socketFd;
        descriptor.events = POLLOUT;
        int pollResult;
        do
        {
            pollResult = ::poll(&descriptor, 1, pollTimeoutMs(deadline));
        }
        while (pollResult < 0 && errno == EINTR);

        if (pollResult <= 0)
        {
            setError(error, pollResult == 0 ? "TCP control connect timeout" :
                                             errnoMessage("poll TCP connect"));
            ::close(socketFd);
            return false;
        }

        int socketError = 0;
        socklen_t socketErrorBytes = sizeof(socketError);
        if (::getsockopt(socketFd, SOL_SOCKET, SO_ERROR,
                         &socketError, &socketErrorBytes) != 0 || socketError != 0)
        {
            if (socketError != 0)
                errno = socketError;
            setError(error, errnoMessage("connect TCP control"));
            ::close(socketFd);
            return false;
        }
    }

    if (::fcntl(socketFd, F_SETFL, originalFlags) != 0)
    {
        setError(error, errnoMessage("restore TCP control socket flags"));
        ::close(socketFd);
        return false;
    }

    const int enabled = 1;
    (void)::setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY,
                       &enabled, sizeof(enabled));
    (void)::setsockopt(socketFd, SOL_SOCKET, SO_KEEPALIVE,
                       &enabled, sizeof(enabled));

    std::string acceptedBanner;
    if (! readBanner(socketFd, deadline, acceptedBanner, error))
    {
        ::close(socketFd);
        return false;
    }
    if (acceptedBanner.rfind("NCLP READY", 0) != 0)
    {
        if (acceptedBanner == "NCLP CONTROLLED BY SFP LINK")
            setError(error, "NCLP control is currently owned by the SFP link");
        else if (acceptedBanner.rfind("NCLP BUSY", 0) == 0)
            setError(error, "NCLP control connection refused: another client is connected");
        else
            setError(error, "unexpected NCLP TCP banner: " + acceptedBanner);
        ::close(socketFd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        socketFd_ = socketFd;
        banner_ = acceptedBanner;
        stopRequested_ = false;
        connected_ = true;
    }
    readerThread_ = std::thread(&CommandClient::readerLoop, this, socketFd);
    return true;
}

bool CommandClient::readBanner(int socketFd,
                               std::chrono::steady_clock::time_point deadline,
                               std::string& destination,
                               std::string* error)
{
    destination.clear();
    while (destination.size() < 512U)
    {
        pollfd descriptor{};
        descriptor.fd = socketFd;
        descriptor.events = POLLIN;
        int pollResult;
        do
        {
            pollResult = ::poll(&descriptor, 1, pollTimeoutMs(deadline));
        }
        while (pollResult < 0 && errno == EINTR);
        if (pollResult <= 0)
        {
            setError(error, pollResult == 0 ? "timeout waiting for NCLP TCP banner" :
                                             errnoMessage("poll NCLP TCP banner"));
            return false;
        }
        if ((descriptor.revents & POLLNVAL) != 0)
        {
            setError(error, "NCLP TCP connection closed before banner");
            return false;
        }

        // A peer may send its complete banner and close immediately. poll(2)
        // can then report POLLIN together with POLLHUP (and, on some stacks,
        // POLLERR). Consume the readable bytes before acting on the close so
        // the ownership/busy banner is not discarded.
        if ((descriptor.revents & POLLIN) == 0)
        {
            if ((descriptor.revents & (POLLERR | POLLHUP)) != 0)
            {
                setError(error, "NCLP TCP connection closed before banner");
                return false;
            }
            continue;
        }

        char byte = 0;
        const ssize_t count = ::recv(socketFd, &byte, 1, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            setError(error, count == 0 ? "NCLP TCP connection closed before banner" :
                                        errnoMessage("read NCLP TCP banner"));
            return false;
        }
        destination.push_back(byte);
        if (byte == '\n')
        {
            while (! destination.empty() &&
                   (destination.back() == '\n' || destination.back() == '\r'))
                destination.pop_back();
            return true;
        }
    }

    setError(error, "NCLP TCP banner exceeded 512 bytes");
    return false;
}

void CommandClient::disconnect()
{
    stopRequested_ = true;
    int socketFd = -1;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        socketFd = socketFd_;
        socketFd_ = -1;
        connected_ = false;
        banner_.clear();
    }
    if (socketFd >= 0)
    {
        (void)::shutdown(socketFd, SHUT_RDWR);
        (void)::close(socketFd);
    }
    if (readerThread_.joinable())
        readerThread_.join();
    failAllPending("NCLP TCP control disconnected");
}

bool CommandClient::isConnected() const
{
    return connected_.load();
}

std::string CommandClient::banner() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return banner_;
}

uint32_t CommandClient::nextSequence()
{
    for (;;)
    {
        uint32_t sequence = nextSequence_.fetch_add(1U);
        if (sequence == 0U)
            continue;
        std::lock_guard<std::mutex> lock(pendingMutex_);
        if (pending_.find(sequence) == pending_.end())
            return sequence;
    }
}

bool CommandClient::sendAll(int socketFd,
                            const uint8_t* bytes,
                            size_t count,
                            std::chrono::steady_clock::time_point deadline,
                            std::string* error)
{
    size_t sent = 0;
    while (sent < count)
    {
        const ssize_t result = ::send(socketFd, bytes + sent, count - sent,
                                      MSG_NOSIGNAL);
        if (result > 0)
        {
            sent += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0 && (errno == EAGAIN
#if EWOULDBLOCK != EAGAIN
                           || errno == EWOULDBLOCK
#endif
                           ))
        {
            pollfd descriptor{};
            descriptor.fd = socketFd;
            descriptor.events = POLLOUT;
            const int pollResult = ::poll(&descriptor, 1, pollTimeoutMs(deadline));
            if (pollResult > 0)
                continue;
            setError(error, pollResult == 0 ? "timeout writing NCLP TCP command" :
                                             errnoMessage("poll NCLP TCP command"));
            return false;
        }
        setError(error, result == 0 ? "NCLP TCP command socket closed" :
                                     errnoMessage("write NCLP TCP command"));
        return false;
    }
    return true;
}

Reply CommandClient::request(Command command,
                             const CommandArgs& args,
                             std::chrono::milliseconds timeout,
                             std::string* error)
{
    Reply empty{};
    if (! isConnected())
    {
        setError(error, "NCLP TCP control is not connected");
        return empty;
    }

    const uint32_t commandWord = static_cast<uint32_t>(command);
    const uint32_t sequence = nextSequence();
    auto pending = std::make_shared<PendingReply>();
    pending->expectedCommand = commandWord;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_[sequence] = pending;
    }

    std::array<uint8_t, kCommandWords * sizeof(uint32_t)> bytes{};
    writeLe32(bytes.data() + 0U, kCommandMagic);
    writeLe32(bytes.data() + 4U, kCommandVersion);
    writeLe32(bytes.data() + 8U, commandWord);
    writeLe32(bytes.data() + 12U, sequence);
    for (size_t index = 0; index < args.size(); ++index)
        writeLe32(bytes.data() + (index + 4U) * 4U, args[index]);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string sendError;
    bool sent = false;
    {
        std::lock_guard<std::mutex> writeLock(writeMutex_);
        int socketFd = -1;
        {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            socketFd = socketFd_;
        }
        if (socketFd >= 0)
            sent = sendAll(socketFd, bytes.data(), bytes.size(), deadline, &sendError);
        else
            sendError = "NCLP TCP control disconnected before command write";
    }
    if (! sent)
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pending_.erase(sequence);
        setError(error, sendError);
        return empty;
    }

    std::unique_lock<std::mutex> pendingLock(pending->mutex);
    if (! pending->condition.wait_until(
            pendingLock, deadline, [&pending] { return pending->complete; }))
    {
        pendingLock.unlock();
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            const auto found = pending_.find(sequence);
            if (found != pending_.end() && found->second == pending)
                pending_.erase(found);
        }
        setError(error, "timeout waiting for NCLP TCP reply cmd=" +
                        std::to_string(commandWord) + " seq=" +
                        std::to_string(sequence));
        return empty;
    }

    if (! pending->error.empty())
    {
        setError(error, pending->error);
        return empty;
    }
    return pending->reply;
}

Reply CommandClient::request(Command command,
                             std::chrono::milliseconds timeout,
                             std::string* error)
{
    return request(command, CommandArgs{}, timeout, error);
}

void CommandClient::readerLoop(int socketFd)
{
    std::array<uint8_t, kReplyWords * sizeof(uint32_t)> bytes{};
    size_t used = 0;
    while (! stopRequested_.load())
    {
        const ssize_t count = ::recv(socketFd, bytes.data() + used,
                                     bytes.size() - used, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            if (! stopRequested_.load())
                readerFailed(socketFd,
                             count == 0 ? "NCLP TCP control connection closed" :
                                          errnoMessage("read NCLP TCP reply"));
            return;
        }
        used += static_cast<size_t>(count);
        if (used != bytes.size())
            continue;

        Reply reply{};
        reply.magic = readLe32(bytes.data() + 0U);
        reply.version = readLe32(bytes.data() + 4U);
        reply.command = readLe32(bytes.data() + 8U);
        reply.sequence = readLe32(bytes.data() + 12U);
        reply.status = readLe32(bytes.data() + 16U);
        reply.data0 = readLe32(bytes.data() + 20U);
        reply.data1 = readLe32(bytes.data() + 24U);
        reply.data2 = readLe32(bytes.data() + 28U);
        reply.data3 = readLe32(bytes.data() + 32U);
        reply.failCount = readLe32(bytes.data() + 36U);
        used = 0;

        if (reply.magic != kCommandMagic)
        {
            readerFailed(socketFd, "malformed NCLP binary TCP reply");
            return;
        }
        if (reply.version != kCommandVersion)
        {
            readerFailed(socketFd, "Firmware mismatch. Boot the matching BOOT.bin and reconnect.");
            return;
        }

        std::shared_ptr<PendingReply> pending;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            const auto found = pending_.find(reply.sequence);
            if (found != pending_.end())
            {
                pending = found->second;
                pending_.erase(found);
            }
        }
        if (pending == nullptr)
            continue; // A timed-out request may receive a late valid reply.

        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            if (reply.command != pending->expectedCommand)
                pending->error = "NCLP TCP reply command does not match its sequence";
            else
                pending->reply = reply;
            pending->complete = true;
        }
        pending->condition.notify_all();
    }
}

void CommandClient::readerFailed(int socketFd, const std::string& message)
{
    bool ownsSocket = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (socketFd_ == socketFd)
        {
            socketFd_ = -1;
            connected_ = false;
            ownsSocket = true;
        }
    }
    if (ownsSocket)
    {
        (void)::shutdown(socketFd, SHUT_RDWR);
        (void)::close(socketFd);
    }
    failAllPending(message);
}

void CommandClient::failAllPending(const std::string& message)
{
    std::vector<std::shared_ptr<PendingReply>> pendingReplies;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingReplies.reserve(pending_.size());
        for (auto& item : pending_)
            pendingReplies.push_back(item.second);
        pending_.clear();
    }
    for (const auto& pending : pendingReplies)
    {
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->error = message;
            pending->complete = true;
        }
        pending->condition.notify_all();
    }
}

Reply CommandClient::ping(std::chrono::milliseconds timeout, std::string* error)
{
    return request(Command::Ping, timeout, error);
}

Reply CommandClient::getStatus(std::chrono::milliseconds timeout, std::string* error)
{
    return request(Command::GetStatus, timeout, error);
}

Reply CommandClient::getConfig(ConfigSection section,
                               std::chrono::milliseconds timeout,
                               std::string* error)
{
    CommandArgs args{};
    args[0] = static_cast<uint32_t>(section);
    return request(Command::GetConfig, args, timeout, error);
}

Reply CommandClient::setUdpDestination(uint32_t ipv4,
                                       uint16_t port,
                                       std::chrono::milliseconds timeout,
                                       std::string* error)
{
    CommandArgs args{};
    args[0] = ipv4;
    args[1] = port;
    return request(Command::SetUdpDestination, args, timeout, error);
}

Reply CommandClient::setRate(uint32_t sampleRateHz,
                             std::chrono::milliseconds timeout,
                             std::string* error)
{
    CommandArgs args{};
    args[0] = sampleRateHz;
    return request(Command::SetRate, args, timeout, error);
}

Reply CommandClient::setBandwidth(uint32_t analogLowerMilliHz,
                                  uint32_t analogUpperHz,
                                  std::chrono::milliseconds timeout,
                                  std::string* error)
{
    CommandArgs args{};
    args[0] = analogLowerMilliHz;
    args[1] = analogUpperHz;
    return request(Command::SetBandwidth, args, timeout, error);
}

Reply CommandClient::setDsp(bool enabled,
                            uint32_t requestedCutoffMilliHz,
                            std::chrono::milliseconds timeout,
                            std::string* error)
{
    CommandArgs args{};
    args[0] = enabled ? 1U : 0U;
    args[1] = requestedCutoffMilliHz;
    return request(Command::SetDsp, args, timeout, error);
}

Reply CommandClient::setTtlSettle(bool enabled,
                                  uint32_t channel,
                                  std::chrono::milliseconds timeout,
                                  std::string* error)
{
    CommandArgs args{};
    args[0] = enabled ? 1U : 0U;
    args[1] = channel;
    return request(Command::SetTtlSettle, args, timeout, error);
}

Reply CommandClient::scan(std::chrono::milliseconds timeout, std::string* error)
{
    return request(Command::Scan, timeout, error);
}

Reply CommandClient::init(uint32_t stream,
                          bool vddSense,
                          std::chrono::milliseconds timeout,
                          std::string* error)
{
    CommandArgs args{};
    args[0] = stream;
    args[1] = vddSense ? 1U : 0U;
    return request(Command::Init, args, timeout, error);
}

Reply CommandClient::impedance(std::chrono::milliseconds timeout, std::string* error)
{
    return request(Command::Impedance, timeout, error);
}

Reply CommandClient::reset(std::chrono::milliseconds timeout, std::string* error)
{
    return request(Command::Reset, timeout, error);
}

Reply CommandClient::cancel(std::chrono::milliseconds timeout, std::string* error)
{
    return request(Command::Cancel, timeout, error);
}

Reply CommandClient::startStream(SampleMode mode,
                                 std::chrono::milliseconds timeout,
                                 std::string* error)
{
    CommandArgs args{};
    args[4] = mode == SampleMode::Vdd ? kStartAuxVdd : kStartAuxInputs;
    return request(Command::StartStream, args, timeout, error);
}

Reply CommandClient::stopStream(std::chrono::milliseconds timeout, std::string* error)
{
    return request(Command::StopStream, timeout, error);
}

Reply CommandClient::getProgress(std::chrono::milliseconds timeout, std::string* error)
{
    return request(Command::GetProgress, timeout, error);
}

Reply CommandClient::getResult(ResultType type,
                               uint32_t resultId,
                               uint32_t index,
                               std::chrono::milliseconds timeout,
                               std::string* error)
{
    CommandArgs args{};
    args[0] = static_cast<uint32_t>(type);
    args[1] = resultId;
    args[2] = index;
    return request(Command::GetResult, args, timeout, error);
}

Status CommandClient::statusFromReply(const Reply& reply)
{
    Status status{};
    status.physicalChipMask = reply.data0 & 0xffU;
    status.logicalStreamMask = reply.data1 & 0xffffU;
    status.packedChipIds = reply.data2;
    const uint16_t controlStatusWord = static_cast<uint16_t>(reply.data3 & 0xffffU);
    status.layoutId = static_cast<uint16_t>(reply.data3 >> 16U);
    status.streamError = reply.failCount;
    status.state = static_cast<ControlState>((controlStatusWord >> 8U) & 0x0fU);
    status.streaming = (controlStatusWord & 0x0001U) != 0U;
    status.routeUdp = (controlStatusWord & 0x0002U) != 0U;
    status.auxVdd = ((controlStatusWord >> 2U) & 0x03U) == 1U;
    status.scanValid = (controlStatusWord & 0x1000U) != 0U;
    status.initialized = (controlStatusWord & 0x2000U) != 0U;
    status.impedanceValid = (controlStatusWord & 0x4000U) != 0U;
    return status;
}

bool CommandClient::configurationFromReplies(const Reply& core,
                                              const Reply& filters,
                                              const Reply& ttl,
                                              AcquisitionConfig& config,
                                              std::string* error)
{
    if (! core.ok() || ! filters.ok() || ! ttl.ok() ||
        core.command != kCmdGetConfig || filters.command != kCmdGetConfig ||
        ttl.command != kCmdGetConfig)
    {
        setError(error, "invalid GET_CONFIG reply set");
        return false;
    }
    config = AcquisitionConfig{};
    config.generation = core.data0;
    config.sampleRateHz = core.data1;
    config.flags = core.data2;
    config.analogLowerMilliHz = filters.data0;
    config.analogUpperHz = filters.data1;
    config.dspRequestedMilliHz = filters.data2;
    config.dspActualMilliHz = filters.data3;
    config.dspEnabled = ttl.data0 != 0U;
    config.dspCode = ttl.data1;
    config.ttlSettleEnabled = ttl.data2 != 0U;
    config.ttlSettleChannel = ttl.data3;
    config.initialized = (config.flags & kConfigFlagInitialized) != 0U;
    return true;
}

ProgressSnapshot CommandClient::progressFromReply(const Reply& reply)
{
    ProgressSnapshot progress{};
    const uint32_t packed = reply.data0;
    progress.operation = packed & 0xffU;
    progress.state = (packed >> 8U) & 0x0fU;
    const uint32_t stream = (packed >> 12U) & 0x0fU;
    const uint32_t channel = (packed >> 16U) & 0xffU;
    const uint32_t capRange = (packed >> 24U) & 0x03U;
    progress.status = (packed >> 26U) & 0x3fU;
    progress.currentStream = stream == 0x0fU ? -1 : static_cast<int32_t>(stream);
    progress.currentChannel = channel == 0xffU ? -1 : static_cast<int32_t>(channel);
    progress.currentCapRange = capRange == 0x03U ? -1 : static_cast<int32_t>(capRange);
    progress.completedUnits = reply.data1;
    progress.totalUnits = reply.data2;
    progress.resultId = reply.data3;
    progress.failCount = reply.failCount;
    return progress;
}

ResultDescriptor CommandClient::resultDescriptorFromReply(const Reply& reply)
{
    ResultDescriptor descriptor{};
    descriptor.resultId = reply.data0;
    descriptor.state = reply.data1 & 0xffU;
    descriptor.resultType = (reply.data1 >> 8U) & 0xffU;
    descriptor.operationStatus = reply.data1 >> 16U;
    descriptor.availableRecords = reply.data2;
    descriptor.totalRecords = reply.data3 & 0xffffU;
    descriptor.recordBytes = reply.data3 >> 16U;
    descriptor.failCount = reply.failCount;
    return descriptor;
}

ResultRecord CommandClient::resultRecordFromReply(const Reply& reply)
{
    ResultRecord record{};
    record.words = { reply.data0, reply.data1, reply.data2, reply.data3 };
    record.failCount = reply.failCount;
    return record;
}

} // namespace nclp
