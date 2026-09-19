#include "NCLPSession.h"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

#include "NCLPSessionSupport.h"

namespace nclp
{
using namespace session_detail;

int detail::effectiveLinuxReceiveBufferBytes(int kernelReportedBytes)
{
    return kernelReportedBytes > 0 ? kernelReportedBytes / 2 : 0;
}

bool Session::startStreaming(std::chrono::milliseconds firstPacketTimeout,
                             std::string* error)
{
    if (! beginBusy("Start streaming", error))
        return false;
    ScopeExit finish([this] { endBusy(); });
    std::lock_guard<std::mutex> operationLock(operationMutex_);

    if (! ensureConnected(error) || ! ensureInitialized(error))
        return false;

    SessionConfig config = configSnapshot();
    std::string bindHostIp;
    Topology topology{};
    AcquisitionConfig acquisitionConfig{};
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (snapshot_.streaming)
        {
            const std::string message = "Stream is already active";
            snapshot_.lastError = message;
            if (error != nullptr)
                *error = message;
            return false;
        }
        topology = snapshot_.topology;
        acquisitionConfig = snapshot_.config;
        bindHostIp = resolvedHostIp_;
    }

    // refreshStatus() can learn that firmware stopped after a listener error,
    // leaving an exited std::thread object joinable. Reap it before assigning
    // a new listenerThread_; assigning over a joinable std::thread terminates
    // the process. Stop it before resetting the queue so no stale final packet
    // can be inserted into the next stream's state.
    stopListener();

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        queueHead_ = 0;
        queueSize_ = 0;
        wakeAtQueueSize_ = 0;
        fatalError_.clear();
        snapshot_.queuedFrames = 0;
        snapshot_.receivedData = false;
        snapshot_.parserStats = {};
        snapshot_.queueOverruns = 0;
        snapshot_.kernelDroppedPackets = 0;
        snapshot_.unexpectedSourcePackets = 0;
        snapshot_.consumerDroppedFrames = 0;
        snapshot_.queuedFramesHighWater = 0;
        snapshot_.queueCapacityFrames = frameQueue_.size();
        snapshot_.kernelReportedReceiveBufferBytes = 0;
        snapshot_.actualReceiveBufferBytes = 0;
        snapshot_.receiveBufferSetSucceeded = false;
        snapshot_.receiveBufferQuerySucceeded = false;
        snapshot_.receiveBufferClamped = false;
        snapshot_.receiveBufferWarning.clear();
        snapshot_.lastError.clear();
    }

    if (bindHostIp.empty())
    {
        setError("No resolved local IPv4 address is available; reconnect first", error);
        return false;
    }

    std::string parserError;
    if (! parser_.configure(topology, acquisitionConfig.sampleRateHz,
                            config.sampleMode, &parserError))
    {
        setError("Could not configure UDP decoder: " + parserError, error);
        return false;
    }
    if (! openDataSocket(bindHostIp, error))
        return false;

    const uint32_t destination = hostOrderIpv4(bindHostIp);
    const uint32_t expectedSource = hostOrderIpv4(config.fpgaIp);
    if (destination == 0 || expectedSource == 0)
    {
        closeDataSocket();
        setError("Streaming requires valid non-zero FPGA and host IPv4 addresses", error);
        return false;
    }

    std::string transportError;
    const Reply destinationReply = commandClient_.setUdpDestination(destination,
                                                                    config.dataPort,
                                                                    2000ms,
                                                                    &transportError);
    if (! replySucceeded(destinationReply, Command::SetUdpDestination))
    {
        closeDataSocket();
        setError(replyError("SET_UDP_DEST", destinationReply, transportError), error);
        return false;
    }

    listenerStopRequested_ = false;
    listenerFailed_ = false;
    listenerThread_ = std::thread(&Session::listenerLoop, this, expectedSource);

    transportError.clear();
    const Reply startReply = commandClient_.startStream(config.sampleMode,
                                                        3000ms,
                                                        &transportError);
    if (! replySucceeded(startReply, Command::StartStream))
        return reconcileFailedStart(replyError("START", startReply, transportError),
                                    error);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.streaming = true;
        snapshot_.routeUdp = true;
    }

    std::unique_lock<std::mutex> stateLock(stateMutex_);
    const bool received = dataReadyCv_.wait_for(stateLock, firstPacketTimeout, [this] {
        return snapshot_.receivedData || listenerFailed_.load();
    });
    const bool listenerFailed = listenerFailed_.load();
    const std::string listenerError = fatalError_;
    stateLock.unlock();

    if (! received || listenerFailed)
    {
        const std::string message = listenerFailed
            ? (listenerError.empty() ? "UDP listener failed" : listenerError)
            : "Timed out waiting for the first valid UDP v1 frame";
        return reconcileFailedStart(message, error);
    }

    std::string statusError;
    if (! refreshStatus(&statusError))
    {
        const std::string message = statusError.empty()
            ? "Could not verify firmware state after START"
            : "Could not verify firmware state after START: " + statusError;
        return reconcileFailedStart(message, error);
    }
    bool validStreamingState = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        validStreamingState = snapshot_.streaming && snapshot_.routeUdp;
    }
    if (! validStreamingState)
        return reconcileFailedStart(
            "Firmware did not remain in UDP STREAMING state", error);
    clearError();
    return true;
}

bool Session::reconcileFailedStart(const std::string& startError,
                                   std::string* error)
{
    // START is not idempotent and its reply can be lost after firmware has
    // committed the command. Never tear down the UDP receiver merely because
    // the TCP reply was missing: first establish authoritative STATUS, request
    // STOP if needed, and confirm the resulting state.
    bool haveStatus = false;
    bool hardwareStopped = false;
    bool reachedReady = false;
    ControlState state = ControlState::Idle;
    std::string lastStatusError;

    const auto queryStatus = [this, &haveStatus, &hardwareStopped,
                              &reachedReady, &state,
                              &lastStatusError]() -> bool
    {
        if (! commandClient_.isConnected())
        {
            lastStatusError = "TCP control connection is down";
            std::lock_guard<std::mutex> lock(stateMutex_);
            snapshot_.connected = false;
            return false;
        }

        std::string statusError;
        if (! refreshStatus(&statusError))
        {
            lastStatusError = statusError.empty()
                ? "STATUS could not be read"
                : statusError;
            return false;
        }

        haveStatus = true;
        lastStatusError.clear();
        std::lock_guard<std::mutex> lock(stateMutex_);
        state = snapshot_.controlState;
        hardwareStopped = ! snapshot_.streaming && ! snapshot_.routeUdp;
        reachedReady = hardwareStopped && state == ControlState::Ready;
        return true;
    };

    (void) queryStatus();

    std::string stopError;
    if ((! haveStatus || ! hardwareStopped) && commandClient_.isConnected())
    {
        std::string transportError;
        const Reply stopReply = commandClient_.stopStream(3000ms, &transportError);
        const bool stopAccepted =
            replySucceeded(stopReply, Command::StopStream) ||
            (replyIsValidFor(stopReply, Command::StopStream) &&
             stopReply.status == static_cast<uint32_t>(CommandStatus::NotActive));
        if (! stopAccepted)
            stopError = replyError("STOP after failed START", stopReply,
                                   transportError);
    }

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (! reachedReady && commandClient_.isConnected() &&
           std::chrono::steady_clock::now() < deadline)
    {
        (void) queryStatus();
        if (reachedReady || (hardwareStopped && state == ControlState::Fault))
            break;
        std::this_thread::sleep_for(25ms);
    }

    if (haveStatus && hardwareStopped)
    {
        stopListener();
        closeDataSocket();
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            snapshot_.streaming = false;
            snapshot_.routeUdp = false;
            snapshot_.listenerRunning = false;
            queueHead_ = 0;
            queueSize_ = 0;
            wakeAtQueueSize_ = 0;
            snapshot_.queuedFrames = 0;
        }

        std::string message = startError;
        if (reachedReady)
            message += "; recovery STATUS confirms firmware READY";
        else
            message += "; STATUS confirms UDP stopped, but firmware did not reach READY";
        setError(message, error);
        return false;
    }

    // With no authoritative non-streaming STATUS, retain the socket/listener
    // and conservatively mark the stream active. This makes STOP and reconnect
    // remain available instead of silently orphaning a possible hardware run.
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.streaming = true;
        if (! haveStatus)
            snapshot_.routeUdp = true;
    }

    std::string message = startError +
        "; hardware streaming remains active or unconfirmed; UDP recovery is "
        "being preserved for Reconnect/STOP retry";
    if (! stopError.empty())
        message += "; " + stopError;
    if (! lastStatusError.empty())
        message += "; " + lastStatusError;
    setError(message, error);
    return false;
}

void Session::stopStreaming()
{
    commandBusy_ = true;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.commandBusy = true;
    }
    std::lock_guard<std::mutex> operationLock(operationMutex_);

    bool wasStreaming = false;
    std::string priorError;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        wasStreaming = snapshot_.streaming || snapshot_.routeUdp;
        priorError = snapshot_.lastError;
    }

    bool hardwareStopped = ! wasStreaming;
    bool reachedReady = ! wasStreaming;
    std::string stopError;

    if (wasStreaming && ! commandClient_.isConnected())
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            snapshot_.connected = false;
        }
        stopError = "STOP could not be sent because the TCP control connection is down; "
                    "the board may still be streaming";
    }
    else if (wasStreaming)
    {
        std::string transportError;
        const Reply stopReply = commandClient_.stopStream(3000ms, &transportError);
        const bool stopAccepted =
            replySucceeded(stopReply, Command::StopStream) ||
            (replyIsValidFor(stopReply, Command::StopStream) &&
             stopReply.status == static_cast<uint32_t>(CommandStatus::NotActive));
        if (! stopAccepted)
            stopError = replyError("STOP", stopReply, transportError);

        const auto deadline = std::chrono::steady_clock::now() +
            (stopAccepted ? 5s : 250ms);
        do
        {
            std::string statusError;
            if (refreshStatus(&statusError))
            {
                ControlState state = ControlState::Idle;
                bool streaming = true;
                bool routeUdp = true;
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    state = snapshot_.controlState;
                    streaming = snapshot_.streaming;
                    routeUdp = snapshot_.routeUdp;
                }
                hardwareStopped = ! streaming && ! routeUdp;
                reachedReady = hardwareStopped && state == ControlState::Ready;
                if (reachedReady ||
                    (hardwareStopped && state == ControlState::Fault) ||
                    (! stopAccepted && ! hardwareStopped))
                    break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(25ms);
        }
        while (true);

        if (reachedReady)
            stopError.clear(); // STATUS is authoritative even if the STOP reply was lost.
        else if (stopAccepted && ! hardwareStopped)
            stopError = "STOP was accepted, but STATUS still reports an active UDP stream; "
                        "STOP may be retried";
        else if (stopAccepted && hardwareStopped && ! reachedReady)
            stopError = "UDP streaming stopped, but firmware did not reach READY before the timeout";
    }

    if (hardwareStopped)
    {
        stopListener();
        closeDataSocket();
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.streaming = false;
        snapshot_.routeUdp = false;
        snapshot_.listenerRunning = false;
        queueHead_ = 0;
        queueSize_ = 0;
        wakeAtQueueSize_ = 0;
        snapshot_.queuedFrames = 0;
        if (stopError.empty())
            snapshot_.lastError = priorError;
    }
    // When STATUS still says streaming, leave the listener/socket intact and
    // preserve that state. A later stopStreaming() call can safely retry STOP.

    if (! stopError.empty())
        setError(stopError);
    endBusy();
}

bool Session::openDataSocket(const std::string& bindHostIp, std::string* error)
{
    closeDataSocket();
    const SessionConfig config = configSnapshot();
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        setError(errnoMessage("Could not create UDP socket"), error);
        return false;
    }

    int enabled = 1;
    (void) ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#ifdef SO_RXQ_OVFL
    (void) ::setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &enabled, sizeof(enabled));
#endif
    const int requested = config.receiveBufferBytes;
    const int setResult = ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                                      &requested, sizeof(requested));
    const int setErrorNumber = setResult == 0 ? 0 : errno;
    int kernelReported = 0;
    socklen_t actualSize = sizeof(kernelReported);
    const int queryResult = ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                                        &kernelReported, &actualSize);
    const int queryErrorNumber = queryResult == 0 ? 0 : errno;
    int effective = 0;
    if (queryResult == 0)
    {
#if defined(__linux__)
        effective = detail::effectiveLinuxReceiveBufferBytes(kernelReported);
#else
        effective = kernelReported;
#endif
    }

    std::string receiveBufferWarning;
    if (setResult != 0)
    {
        receiveBufferWarning =
            "UDP SO_RCVBUF request failed: " +
            std::string(std::strerror(setErrorNumber));
    }
    if (queryResult != 0)
    {
        if (! receiveBufferWarning.empty())
            receiveBufferWarning += "; ";
        receiveBufferWarning +=
            "UDP SO_RCVBUF query failed: " +
            std::string(std::strerror(queryErrorNumber));
    }
    const bool clamped = queryResult == 0 && effective < requested;
    if (clamped)
    {
        if (! receiveBufferWarning.empty())
            receiveBufferWarning += "; ";
        receiveBufferWarning +=
            "UDP SO_RCVBUF clamped: requested " +
            std::to_string(requested) + " bytes, effective " +
            std::to_string(effective) + " bytes";
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(config.dataPort);
    if (::inet_pton(AF_INET, bindHostIp.c_str(), &local.sin_addr) != 1)
    {
        ::close(fd);
        setError("Invalid local IPv4 address: " + bindHostIp, error);
        return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
    {
        const std::string message = errnoMessage("Could not bind UDP data port");
        ::close(fd);
        setError(message, error);
        return false;
    }

    dataSocketFd_ = fd;
    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.requestedReceiveBufferBytes = requested;
    snapshot_.kernelReportedReceiveBufferBytes = kernelReported;
    snapshot_.actualReceiveBufferBytes = effective;
    snapshot_.receiveBufferSetSucceeded = setResult == 0;
    snapshot_.receiveBufferQuerySucceeded = queryResult == 0;
    snapshot_.receiveBufferClamped = clamped;
    snapshot_.receiveBufferWarning = std::move(receiveBufferWarning);
    return true;
}

void Session::closeDataSocket()
{
    if (dataSocketFd_ >= 0)
    {
        ::close(dataSocketFd_);
        dataSocketFd_ = -1;
    }
}

void Session::stopListener()
{
    listenerStopRequested_ = true;
    if (listenerThread_.joinable())
        listenerThread_.join();
    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.listenerRunning = false;
}

void Session::listenerLoop(uint32_t expectedSourceAddress)
{
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.listenerRunning = true;
    }

    std::array<uint8_t, 65536> datagram{};
    while (! listenerStopRequested_.load())
    {
        pollfd descriptor{};
        descriptor.fd = dataSocketFd_;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, 100);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            failListener(errnoMessage("UDP poll failed"));
            break;
        }
        if (ready == 0)
            continue;
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            if (! listenerStopRequested_.load())
                failListener("UDP socket reported an error");
            break;
        }

        sockaddr_in source{};
        iovec vector{};
        vector.iov_base = datagram.data();
        vector.iov_len = datagram.size();
        std::array<uint8_t, CMSG_SPACE(sizeof(uint32_t))> controls{};
        msghdr message{};
        message.msg_name = &source;
        message.msg_namelen = sizeof(source);
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = controls.data();
        message.msg_controllen = controls.size();

        const ssize_t bytes = ::recvmsg(dataSocketFd_, &message, 0);
        if (bytes < 0)
        {
            if (errno == EINTR || errno == EAGAIN
#if EWOULDBLOCK != EAGAIN
                || errno == EWOULDBLOCK
#endif
                )
                continue;
            if (! listenerStopRequested_.load())
                failListener(errnoMessage("UDP receive failed"));
            break;
        }

        if (ntohl(source.sin_addr.s_addr) != expectedSourceAddress)
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            ++snapshot_.unexpectedSourcePackets;
            continue;
        }

#ifdef SO_RXQ_OVFL
        for (cmsghdr* control = CMSG_FIRSTHDR(&message);
             control != nullptr;
             control = CMSG_NXTHDR(&message, control))
        {
            if (control->cmsg_level == SOL_SOCKET &&
                control->cmsg_type == SO_RXQ_OVFL &&
                control->cmsg_len >= CMSG_LEN(sizeof(uint32_t)))
            {
                uint32_t dropped = 0;
                std::memcpy(&dropped, CMSG_DATA(control), sizeof(dropped));
                std::lock_guard<std::mutex> lock(stateMutex_);
                snapshot_.kernelDroppedPackets =
                    std::max(snapshot_.kernelDroppedPackets,
                             static_cast<uint64_t>(dropped));
            }
        }
#endif

        DecodedFrame decoded{};
        std::string decodeError;
        const DecodeDisposition disposition = parser_.decodeDatagram(
            datagram.data(), static_cast<size_t>(bytes), decoded, &decodeError);
        const ParserStats stats = parser_.stats();

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            snapshot_.parserStats = stats;
        }

        if (disposition == DecodeDisposition::Fatal)
        {
            failListener(decodeError.empty() ? "Fatal UDP protocol change" : decodeError);
            break;
        }
        if (disposition != DecodeDisposition::Accepted)
            continue;

        QueuedFrame queued{};
        queued.samples = decoded.samples;
        queued.amplifierCounts = decoded.amplifierCounts;
        queued.channelCount = decoded.channelCount;
        queued.timestamp = decoded.timestamp;
        queued.ttl = decoded.ttl;
        queued.missingBefore = decoded.missingBefore;
        queued.timestampMissingBefore = decoded.timestampMissingBefore;

        bool shouldNotify = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            const bool firstFrame = ! snapshot_.receivedData;
            const size_t capacity = frameQueue_.size();
            if (queueSize_ >= capacity)
            {
                queueHead_ = (queueHead_ + 1U) % capacity;
                --queueSize_;
                ++snapshot_.queueOverruns;
            }
            const size_t tail = (queueHead_ + queueSize_) % capacity;
            frameQueue_[tail] = std::move(queued);
            ++queueSize_;
            snapshot_.queuedFrames = queueSize_;
            snapshot_.queuedFramesHighWater =
                std::max(snapshot_.queuedFramesHighWater, queueSize_);
            snapshot_.receivedData = true;
            shouldNotify = firstFrame ||
                (wakeAtQueueSize_ != 0U && queueSize_ >= wakeAtQueueSize_);
            if (shouldNotify)
                wakeAtQueueSize_ = 0;
        }
        // START needs the first-frame notification. During acquisition, wake
        // the single consumer only when its requested block is ready instead
        // of waking/relocking it once for every UDP datagram.
        if (shouldNotify)
            dataReadyCv_.notify_one();
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snapshot_.listenerRunning = false;
    }
    dataReadyCv_.notify_all();
}

void Session::failListener(const std::string& message)
{
    listenerFailed_ = true;
    listenerStopRequested_ = true;
    std::lock_guard<std::mutex> lock(stateMutex_);
    fatalError_ = message;
    snapshot_.lastError = message;
    dataReadyCv_.notify_all();
}

size_t Session::drainFrames(std::vector<QueuedFrame>& out, size_t maxFrames)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    const size_t count = std::min(maxFrames, queueSize_);
    out.clear();
    out.reserve(count);
    for (size_t index = 0; index < count; ++index)
    {
        out.push_back(std::move(frameQueue_[queueHead_]));
        queueHead_ = (queueHead_ + 1U) % frameQueue_.size();
        --queueSize_;
    }
    snapshot_.queuedFrames = queueSize_;
    return count;
}

size_t Session::waitAndDrainFrames(std::vector<QueuedFrame>& out,
                                   size_t minimumFrames,
                                   size_t maxFrames,
                                   std::chrono::milliseconds timeout)
{
    minimumFrames = std::max<size_t>(1U, minimumFrames);
    maxFrames = std::max(minimumFrames, maxFrames);

    std::unique_lock<std::mutex> lock(stateMutex_);
    if (queueSize_ < minimumFrames && ! listenerFailed_.load() &&
        ! listenerStopRequested_.load() && fatalError_.empty())
        wakeAtQueueSize_ = minimumFrames;
    dataReadyCv_.wait_for(lock, timeout, [this, minimumFrames]
    {
        return queueSize_ >= minimumFrames || listenerFailed_.load() ||
               listenerStopRequested_.load() || ! fatalError_.empty();
    });
    wakeAtQueueSize_ = 0;

    // A timeout is only a responsiveness checkpoint for the DataThread. Keep
    // a partial block in the larger session ring until all requested frames
    // arrive, so normal Open Ephys writes are always one complete delivery
    // block (20 ms for the NCLP source). Stop/failure paths are handled by the
    // caller immediately after this function returns.
    if (queueSize_ < minimumFrames)
    {
        out.clear();
        return 0;
    }

    const size_t count = std::min(maxFrames, queueSize_);
    out.clear();
    out.reserve(count);
    for (size_t index = 0; index < count; ++index)
    {
        out.push_back(std::move(frameQueue_[queueHead_]));
        queueHead_ = (queueHead_ + 1U) % frameQueue_.size();
        --queueSize_;
    }
    snapshot_.queuedFrames = queueSize_;
    return count;
}

void Session::recordConsumerDrops(uint64_t frames)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    snapshot_.consumerDroppedFrames += frames;
}

bool Session::consumeFatalError(std::string* error)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (fatalError_.empty())
        return false;
    if (error != nullptr)
        *error = fatalError_;
    fatalError_.clear();
    return true;
}

} // namespace nclp
