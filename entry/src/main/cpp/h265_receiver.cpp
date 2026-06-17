#include "h265_receiver.h"
#include "t2s_protocol.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <multimedia/player_framework/native_avbuffer_info.h>

namespace {
constexpr uint32_t T2S_LOG_DOMAIN = 0x545253;
constexpr const char* T2S_LOG_TAG = "T2SH265";
constexpr size_t MAX_PENDING_PACKETS = 1;
constexpr size_t MAX_TRACKED_FRAME_TIMINGS = 240;
constexpr size_t VIDEO_FRAGMENT_HEADER_SIZE = 20;
constexpr int32_t DECODER_MAX_INPUT_SIZE = 8 * 1024 * 1024;

uint16_t ReadU16Le(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t ReadU32Le(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

struct FragmentedFrame {
    bool active = false;
    uint32_t sequence = 0;
    uint64_t timestampUs = 0;
    uint16_t flags = 0;
    uint16_t fragmentCount = 0;
    uint32_t totalLen = 0;
    uint32_t receivedBytes = 0;
    std::chrono::steady_clock::time_point firstReceivedAt {};
    std::vector<uint8_t> payload;
    std::vector<uint8_t> received;

    void Reset()
    {
        active = false;
        sequence = 0;
        timestampUs = 0;
        flags = 0;
        fragmentCount = 0;
        totalLen = 0;
        receivedBytes = 0;
        firstReceivedAt = {};
        payload.clear();
        received.clear();
    }
};

class ScopedStatsThread {
public:
    ScopedStatsThread(std::shared_ptr<std::atomic<bool>> running, std::thread&& thread)
        : running_(std::move(running)), thread_(std::move(thread))
    {
    }

    ~ScopedStatsThread()
    {
        running_->store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::shared_ptr<std::atomic<bool>> running_;
    std::thread thread_;
};

double MillisecondsBetween(
    std::chrono::steady_clock::time_point previous,
    std::chrono::steady_clock::time_point current)
{
    if (previous == std::chrono::steady_clock::time_point{}) {
        return 0.0;
    }
    return std::chrono::duration<double, std::milli>(current - previous).count();
}
}

H265Receiver& H265Receiver::Instance()
{
    static H265Receiver receiver;
    return receiver;
}

H265Receiver::~H265Receiver()
{
    Stop();
}

void H265Receiver::RegisterXComponent(OH_NativeXComponent* component)
{
    if (!component) {
        return;
    }

    static OH_NativeXComponent_Callback callback {
        .OnSurfaceCreated = [](OH_NativeXComponent* component, void* window) {
            H265Receiver::Instance().OnSurfaceCreated(component, window);
        },
        .OnSurfaceChanged = [](OH_NativeXComponent* component, void* window) {
            H265Receiver::Instance().OnSurfaceChanged(component, window);
        },
        .OnSurfaceDestroyed = [](OH_NativeXComponent* component, void* window) {
            H265Receiver::Instance().OnSurfaceDestroyed(component, window);
        },
        .DispatchTouchEvent = nullptr,
    };

    component_ = component;
    OH_NativeXComponent_RegisterCallback(component, &callback);
    SetStatus("xcomponent registered");
}

void H265Receiver::OnSurfaceCreated(OH_NativeXComponent*, void* window)
{
    std::lock_guard<std::mutex> lock(mutex_);
    nativeWindow_ = reinterpret_cast<OHNativeWindow*>(window);
    stats_.surfaceReady = nativeWindow_ != nullptr;
    stats_.status = "surface created";
    if (running_ && configured_ && !decoder_) {
        StartDecoderLocked();
    }
}

void H265Receiver::OnSurfaceChanged(OH_NativeXComponent*, void* window)
{
    std::lock_guard<std::mutex> lock(mutex_);
    nativeWindow_ = reinterpret_cast<OHNativeWindow*>(window);
    stats_.surfaceReady = nativeWindow_ != nullptr;
}

void H265Receiver::OnSurfaceDestroyed(OH_NativeXComponent*, void*)
{
    std::lock_guard<std::mutex> lock(mutex_);
    StopDecoderLocked();
    nativeWindow_ = nullptr;
    stats_.surfaceReady = false;
    stats_.status = "surface destroyed";
}

bool H265Receiver::Start(uint16_t port, int32_t width, int32_t height)
{
    if (running_) {
        std::lock_guard<std::mutex> lock(mutex_);
        StopDecoderLocked();
        packets_.clear();
        inputBuffers_.clear();
        frameTimings_.clear();
        if (!configured_) {
            width_ = width;
            height_ = height;
        }
        hasLastSequence_ = false;
        lastReceiveAt_ = {};
        lastInputAt_ = {};
        lastRenderAt_ = {};
        stats_ = H265Stats{};
        stats_.running = true;
        stats_.surfaceReady = nativeWindow_ != nullptr;
        if (configured_) {
            stats_.streamWidth = width_;
            stats_.streamHeight = height_;
            stats_.status = "receiver restarted";
            if (nativeWindow_) {
                StartDecoderLocked();
            }
        } else {
            stats_.status = "listening";
        }
        return true;
    }

    Stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        width_ = width;
        height_ = height;
        stats_ = H265Stats{};
        stats_.running = true;
        stats_.surfaceReady = nativeWindow_ != nullptr;
        stats_.status = "starting";
        running_ = true;
        configured_ = false;
        hasLastSequence_ = false;
        lastReceiveAt_ = {};
        lastInputAt_ = {};
        lastRenderAt_ = {};
    }

    receiverThread_ = std::thread(&H265Receiver::ReceiverLoop, this, port);
    decodeThread_ = std::thread(&H265Receiver::DecodeLoop, this);
    return true;
}

void H265Receiver::Pause()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
        return;
    }
    StopDecoderLocked();
    packets_.clear();
    inputBuffers_.clear();
    frameTimings_.clear();
    stats_.decoderStarted = false;
    stats_.queueDepth = 0;
    stats_.status = "receiver stopped";
}

void H265Receiver::Stop()
{
    running_ = false;
    packetCv_.notify_all();
    inputCv_.notify_all();

    if (receiverThread_.joinable()) {
        receiverThread_.join();
    }
    if (decodeThread_.joinable()) {
        decodeThread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    StopDecoderLocked();
    packets_.clear();
    inputBuffers_.clear();
    frameTimings_.clear();
    stats_.running = false;
    stats_.decoderStarted = false;
    stats_.status = "stopped";
}

H265Stats H265Receiver::GetStats()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool H265Receiver::StartDecoderLocked()
{
    if (decoder_ || !nativeWindow_) {
        return decoder_ != nullptr;
    }

    decoder_ = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_HEVC);
    if (!decoder_) {
        SetErrorLocked(-1, "create decoder failed");
        return false;
    }

    OH_AVCodecCallback callback {
        .onError = OnCodecError,
        .onStreamChanged = OnCodecStreamChanged,
        .onNeedInputBuffer = OnNeedInputBuffer,
        .onNewOutputBuffer = OnNewOutputBuffer,
    };

    OH_AVErrCode result = OH_VideoDecoder_RegisterCallback(decoder_, callback, this);
    if (result != AV_ERR_OK) {
        SetErrorLocked(result, "register callback failed");
        StopDecoderLocked();
        return false;
    }

    OH_AVFormat* format = OH_AVFormat_Create();
    OH_AVFormat_SetStringValue(format, OH_MD_KEY_CODEC_MIME, OH_AVCODEC_MIMETYPE_VIDEO_HEVC);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width_);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height_);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_MAX_INPUT_SIZE, DECODER_MAX_INPUT_SIZE);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 1);

    result = OH_VideoDecoder_Configure(decoder_, format);
    OH_AVFormat_Destroy(format);
    if (result != AV_ERR_OK) {
        SetErrorLocked(result, "configure decoder failed");
        StopDecoderLocked();
        return false;
    }

    result = OH_VideoDecoder_SetSurface(decoder_, nativeWindow_);
    if (result != AV_ERR_OK) {
        SetErrorLocked(result, "set decoder surface failed");
        StopDecoderLocked();
        return false;
    }

    result = OH_VideoDecoder_Prepare(decoder_);
    if (result != AV_ERR_OK) {
        SetErrorLocked(result, "prepare decoder failed");
        StopDecoderLocked();
        return false;
    }

    result = OH_VideoDecoder_Start(decoder_);
    if (result != AV_ERR_OK) {
        SetErrorLocked(result, "start decoder failed");
        StopDecoderLocked();
        return false;
    }

    stats_.decoderStarted = true;
    stats_.status = "decoder started";
    return true;
}

void H265Receiver::StopDecoderLocked()
{
    if (!decoder_) {
        return;
    }
    OH_VideoDecoder_Stop(decoder_);
    OH_VideoDecoder_Destroy(decoder_);
    decoder_ = nullptr;
    stats_.decoderStarted = false;
}

void H265Receiver::ReceiverLoop(uint16_t port)
{
    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        SetError(errno, "socket failed");
        return;
    }

    int yes = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        SetError(errno, "bind failed");
        close(serverFd);
        return;
    }
    if (listen(serverFd, 1) != 0) {
        SetError(errno, "listen failed");
        close(serverFd);
        return;
    }
    fcntl(serverFd, F_SETFL, fcntl(serverFd, F_GETFL, 0) | O_NONBLOCK);

    SetStatus("listening");
    while (running_) {
        int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (running_) {
                SetError(errno, "accept failed");
            }
            continue;
        }

        timeval receiveTimeout {};
        receiveTimeout.tv_sec = 30;
        receiveTimeout.tv_usec = 0;
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));
        setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        SetStatus("sender connected");
        HandleClient(clientFd);
        close(clientFd);
        SetStatus("sender disconnected");
    }
    close(serverFd);
}

bool H265Receiver::HandleClient(int clientFd)
{
    std::string error;
    t2s::Message message;
    if (!t2s::ReadMessage(clientFd, message, error)) {
        SetError(-2, error);
        return false;
    }
    if (message.type != t2s::TYPE_HELLO) {
        SetError(-3, "expected hello");
        return false;
    }
    if (!t2s::WriteMessage(clientFd, {t2s::TYPE_HELLO_ACK, 0, 0, 0, t2s::HelloAckPayload()}, error)) {
        SetError(-4, error);
        return false;
    }

    if (!t2s::ReadMessage(clientFd, message, error)) {
        SetError(-5, error);
        return false;
    }
    t2s::VideoConfig config;
    if (!t2s::ParseVideoConfig(message, config, error)) {
        SetError(-6, error);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        StopDecoderLocked();
        packets_.clear();
        inputBuffers_.clear();
        frameTimings_.clear();
        width_ = static_cast<int32_t>(config.width);
        height_ = static_cast<int32_t>(config.height);
        configured_ = true;
        hasLastSequence_ = false;
        stats_.streamWidth = width_;
        stats_.streamHeight = height_;
        stats_.streamFps = config.fpsDen == 0 ? 0 : static_cast<int32_t>(config.fpsNum / config.fpsDen);
        stats_.queueDepth = 0;
        stats_.status = "video config received";
        if (nativeWindow_) {
            StartDecoderLocked();
        }
    }
    if (!t2s::WriteMessage(clientFd, {t2s::TYPE_VIDEO_CONFIG_ACK, 0, 1, 0, t2s::EmptyPayload()}, error)) {
        SetError(-7, error);
        return false;
    }

    FragmentedFrame fragmentedFrame;
    auto statsRunning = std::make_shared<std::atomic<bool>>(true);
    ScopedStatsThread statsThread(statsRunning, std::thread([this, clientFd, statsRunning]() {
        std::string statsError;
        auto lastSampleAt = std::chrono::steady_clock::now();
        uint64_t lastBytes = 0;
        uint64_t lastQueuedInputs = 0;
        uint64_t lastRenderedOutputs = 0;
        uint64_t lastDroppedPackets = 0;
        while (running_ && statsRunning->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            H265Stats stats = GetStats();
            auto now = std::chrono::steady_clock::now();
            double elapsedSeconds = std::chrono::duration<double>(now - lastSampleAt).count();
            if (elapsedSeconds <= 0.0) {
                elapsedSeconds = 0.001;
            }
            t2s::ReceiverStatsPayload payload {};
            payload.running = stats.running;
            payload.decoderStarted = stats.decoderStarted;
            payload.surfaceReady = stats.surfaceReady;
            payload.packets = stats.packets;
            payload.bytes = stats.bytes;
            payload.queuedInputs = stats.queuedInputs;
            payload.renderedOutputs = stats.renderedOutputs;
            payload.droppedPackets = stats.droppedPackets;
            payload.sequenceGaps = stats.sequenceGaps;
            payload.configPackets = stats.configPackets;
            payload.keyframes = stats.keyframes;
            payload.lastSequence = stats.lastSequence;
            payload.queueDepth = stats.queueDepth;
            payload.streamWidth = stats.streamWidth;
            payload.streamHeight = stats.streamHeight;
            payload.streamFps = stats.streamFps;
            payload.lastError = stats.lastError;
            payload.receiveMbps = static_cast<double>(stats.bytes - lastBytes) * 8.0 / elapsedSeconds / 1000000.0;
            payload.inputFps = static_cast<double>(stats.queuedInputs - lastQueuedInputs) / elapsedSeconds;
            payload.renderFps = static_cast<double>(stats.renderedOutputs - lastRenderedOutputs) / elapsedSeconds;
            payload.dropFps = static_cast<double>(stats.droppedPackets - lastDroppedPackets) / elapsedSeconds;
            payload.maxReceiveGapMs = stats.maxReceiveGapMs;
            payload.maxInputGapMs = stats.maxInputGapMs;
            payload.maxRenderGapMs = stats.maxRenderGapMs;
            payload.latestReceiveToInputMs = stats.latestReceiveToInputMs;
            payload.latestInputToRenderMs = stats.latestInputToRenderMs;
            payload.latestReceiveToRenderMs = stats.latestReceiveToRenderMs;
            payload.maxReceiveToInputMs = stats.maxReceiveToInputMs;
            payload.maxInputToRenderMs = stats.maxInputToRenderMs;
            payload.maxReceiveToRenderMs = stats.maxReceiveToRenderMs;
            if (!t2s::WriteMessage(clientFd, {t2s::TYPE_STATS, 0, 0, 0, t2s::StatsPayload(payload)}, statsError)) {
                break;
            }
            lastSampleAt = now;
            lastBytes = stats.bytes;
            lastQueuedInputs = stats.queuedInputs;
            lastRenderedOutputs = stats.renderedOutputs;
            lastDroppedPackets = stats.droppedPackets;
        }
    }));

    while (running_ && t2s::ReadMessage(clientFd, message, error)) {
        if (message.type == t2s::TYPE_STOP) {
            SetStatus("stop received");
            return true;
        }
        if (message.type != t2s::TYPE_VIDEO_PACKET) {
            SetError(-8, "unexpected message type");
            return false;
        }
        if (message.payload.empty()) {
            SetError(-9, "empty video packet");
            return false;
        }
        if ((message.flags & t2s::FLAG_FRAGMENT) != 0) {
            if (message.payload.size() < VIDEO_FRAGMENT_HEADER_SIZE) {
                SetError(-10, "video fragment header too short");
                return false;
            }
            const uint8_t* header = message.payload.data();
            const uint32_t frameSequence = ReadU32Le(header);
            const uint16_t fragmentIndex = ReadU16Le(header + 4);
            const uint16_t fragmentCount = ReadU16Le(header + 6);
            const uint16_t frameFlags = ReadU16Le(header + 8);
            const uint32_t fragmentOffset = ReadU32Le(header + 12);
            const uint32_t totalLen = ReadU32Le(header + 16);
            const size_t fragmentLen = message.payload.size() - VIDEO_FRAGMENT_HEADER_SIZE;
            if (fragmentCount == 0 || fragmentIndex >= fragmentCount || totalLen > t2s::MAX_PAYLOAD_LEN ||
                fragmentOffset > totalLen || fragmentLen > totalLen - fragmentOffset) {
                SetError(-11, "invalid video fragment");
                return false;
            }
            if (!fragmentedFrame.active || fragmentedFrame.sequence != frameSequence ||
                fragmentedFrame.timestampUs != message.timestampUs || fragmentedFrame.fragmentCount != fragmentCount ||
                fragmentedFrame.totalLen != totalLen) {
                fragmentedFrame.Reset();
                fragmentedFrame.active = true;
                fragmentedFrame.sequence = frameSequence;
                fragmentedFrame.timestampUs = message.timestampUs;
                fragmentedFrame.flags = frameFlags;
                fragmentedFrame.fragmentCount = fragmentCount;
                fragmentedFrame.totalLen = totalLen;
                fragmentedFrame.firstReceivedAt = std::chrono::steady_clock::now();
                fragmentedFrame.payload.assign(totalLen, 0);
                fragmentedFrame.received.assign(fragmentCount, 0);
            }
            if (fragmentedFrame.received[fragmentIndex] == 0) {
                std::memcpy(
                    fragmentedFrame.payload.data() + fragmentOffset,
                    message.payload.data() + VIDEO_FRAGMENT_HEADER_SIZE,
                    fragmentLen);
                fragmentedFrame.received[fragmentIndex] = 1;
                fragmentedFrame.receivedBytes += static_cast<uint32_t>(fragmentLen);
            }
            if (fragmentedFrame.receivedBytes < fragmentedFrame.totalLen) {
                continue;
            }

            Packet packet;
            packet.sequence = fragmentedFrame.sequence;
            packet.timestampUs = fragmentedFrame.timestampUs;
            packet.flags = fragmentedFrame.flags;
            packet.receivedAt = fragmentedFrame.firstReceivedAt;
            packet.payload = std::move(fragmentedFrame.payload);
            fragmentedFrame.Reset();
            EnqueuePacket(std::move(packet));
            continue;
        }
        Packet packet;
        packet.sequence = message.sequence;
        packet.timestampUs = message.timestampUs;
        packet.flags = message.flags;
        packet.receivedAt = std::chrono::steady_clock::now();
        packet.payload = std::move(message.payload);
        EnqueuePacket(std::move(packet));
    }
    if (running_ && !error.empty()) {
        SetStatus("sender disconnected");
    }
    return true;
}

void H265Receiver::DecodeLoop()
{
    while (running_) {
        Packet packet;
        InputBufferRef input;
        if (!PopInputBuffer(input)) {
            continue;
        }
        if (!PopPacket(packet)) {
            continue;
        }
        PushInputBuffer(input.index, input.buffer, packet);
    }
}

void H265Receiver::EnqueuePacket(Packet&& packet)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    stats_.maxReceiveGapMs = std::max(stats_.maxReceiveGapMs, MillisecondsBetween(lastReceiveAt_, now));
    lastReceiveAt_ = now;
    stats_.packets += 1;
    stats_.bytes += packet.payload.size();
    if (hasLastSequence_ && packet.sequence != stats_.lastSequence + 1) {
        stats_.sequenceGaps += 1;
    }
    hasLastSequence_ = true;
    stats_.lastSequence = packet.sequence;
    if ((packet.flags & t2s::FLAG_CONFIG_NAL) != 0) {
        stats_.configPackets += 1;
    }
    if ((packet.flags & t2s::FLAG_KEYFRAME) != 0) {
        stats_.keyframes += 1;
    }
    while (packets_.size() >= MAX_PENDING_PACKETS) {
        if ((packet.flags & t2s::FLAG_KEYFRAME) != 0) {
            stats_.droppedPackets += packets_.size();
            packets_.clear();
            break;
        }
        auto stale = std::find_if(packets_.begin(), packets_.end(), [](const Packet& queued) {
            return (queued.flags & t2s::FLAG_DROPPABLE) != 0;
        });
        if (stale != packets_.end()) {
            packets_.erase(stale);
            stats_.droppedPackets += 1;
            continue;
        }
        stats_.droppedPackets += 1;
        stats_.queueDepth = static_cast<uint32_t>(packets_.size());
        return;
    }
    packets_.push_back(std::move(packet));
    stats_.queueDepth = static_cast<uint32_t>(packets_.size());
    packetCv_.notify_one();
}

bool H265Receiver::PopPacket(Packet& packet)
{
    std::unique_lock<std::mutex> lock(mutex_);
    packetCv_.wait(lock, [&] { return !running_.load() || !packets_.empty(); });
    if (!running_.load() || packets_.empty()) {
        return false;
    }
    packet = std::move(packets_.front());
    packets_.pop_front();
    stats_.queueDepth = static_cast<uint32_t>(packets_.size());
    return true;
}

bool H265Receiver::PopInputBuffer(InputBufferRef& input)
{
    std::unique_lock<std::mutex> lock(mutex_);
    inputCv_.wait(lock, [&] { return !running_.load() || !inputBuffers_.empty(); });
    if (!running_.load() || inputBuffers_.empty()) {
        return false;
    }
    input = inputBuffers_.front();
    inputBuffers_.pop_front();
    return true;
}

void H265Receiver::PushInputBuffer(uint32_t index, OH_AVBuffer* buffer, const Packet& packet)
{
    uint8_t* addr = OH_AVBuffer_GetAddr(buffer);
    int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
    if (!addr || capacity < static_cast<int32_t>(packet.payload.size())) {
        OH_AVCodecBufferAttr attr {};
        attr.pts = static_cast<int64_t>(packet.timestampUs);
        attr.size = 0;
        attr.offset = 0;
        attr.flags = AVCODEC_BUFFER_FLAGS_DISPOSABLE;
        OH_AVBuffer_SetBufferAttr(buffer, &attr);
        OH_VideoDecoder_PushInputBuffer(decoder_, index);
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.droppedPackets += 1;
        stats_.status = "dropped oversized decoder input";
        return;
    }

    std::memcpy(addr, packet.payload.data(), packet.payload.size());
    OH_AVCodecBufferAttr attr {};
    attr.pts = static_cast<int64_t>(packet.timestampUs);
    attr.size = static_cast<int32_t>(packet.payload.size());
    attr.offset = 0;
    attr.flags = AVCODEC_BUFFER_FLAGS_NONE;
    if ((packet.flags & t2s::FLAG_KEYFRAME) != 0) {
        attr.flags |= AVCODEC_BUFFER_FLAGS_SYNC_FRAME;
    }
    if ((packet.flags & t2s::FLAG_DROPPABLE) != 0) {
        attr.flags |= AVCODEC_BUFFER_FLAGS_DISPOSABLE;
    }
    OH_AVBuffer_SetBufferAttr(buffer, &attr);

    OH_AVErrCode result = OH_VideoDecoder_PushInputBuffer(decoder_, index);
    if (result != AV_ERR_OK) {
        SetError(result, "push input failed");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    stats_.maxInputGapMs = std::max(stats_.maxInputGapMs, MillisecondsBetween(lastInputAt_, now));
    lastInputAt_ = now;
    const double receiveToInputMs = MillisecondsBetween(packet.receivedAt, now);
    stats_.latestReceiveToInputMs = receiveToInputMs;
    stats_.maxReceiveToInputMs = std::max(stats_.maxReceiveToInputMs, receiveToInputMs);
    frameTimings_[attr.pts] = {packet.receivedAt, now};
    if (frameTimings_.size() > MAX_TRACKED_FRAME_TIMINGS) {
        frameTimings_.erase(frameTimings_.begin());
    }
    stats_.queuedInputs += 1;
}

void H265Receiver::SetStatus(const std::string& status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.status = status;
}

void H265Receiver::SetError(int32_t error, const std::string& status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    SetErrorLocked(error, status);
}

void H265Receiver::SetErrorLocked(int32_t error, const std::string& status)
{
    stats_.lastError = error;
    stats_.status = status;
    (void)T2S_LOG_DOMAIN;
    (void)T2S_LOG_TAG;
    OH_LOG_ERROR(LOG_APP, "H265Receiver error %{public}d %{public}s", error, status.c_str());
}

void H265Receiver::OnCodecError(OH_AVCodec*, int32_t errorCode, void* userData)
{
    auto* receiver = static_cast<H265Receiver*>(userData);
    receiver->SetError(errorCode, "codec error");
}

void H265Receiver::OnCodecStreamChanged(OH_AVCodec*, OH_AVFormat*, void* userData)
{
    auto* receiver = static_cast<H265Receiver*>(userData);
    receiver->SetStatus("stream changed");
}

void H265Receiver::OnNeedInputBuffer(OH_AVCodec*, uint32_t index, OH_AVBuffer* buffer, void* userData)
{
    auto* receiver = static_cast<H265Receiver*>(userData);
    {
        std::lock_guard<std::mutex> lock(receiver->mutex_);
        receiver->inputBuffers_.push_back({index, buffer});
    }
    receiver->inputCv_.notify_one();
}

void H265Receiver::OnNewOutputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData)
{
    auto* receiver = static_cast<H265Receiver*>(userData);
    OH_AVCodecBufferAttr attr {};
    bool hasAttr = buffer && OH_AVBuffer_GetBufferAttr(buffer, &attr) == AV_ERR_OK;
    OH_AVErrCode result = OH_VideoDecoder_RenderOutputBuffer(codec, index);
    if (result != AV_ERR_OK) {
        receiver->SetError(result, "render output failed");
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }
    std::lock_guard<std::mutex> lock(receiver->mutex_);
    auto now = std::chrono::steady_clock::now();
    receiver->stats_.maxRenderGapMs =
        std::max(receiver->stats_.maxRenderGapMs, MillisecondsBetween(receiver->lastRenderAt_, now));
    receiver->lastRenderAt_ = now;
    if (hasAttr) {
        auto timing = receiver->frameTimings_.find(attr.pts);
        if (timing != receiver->frameTimings_.end()) {
            const double inputToRenderMs = MillisecondsBetween(timing->second.inputAt, now);
            const double receiveToRenderMs = MillisecondsBetween(timing->second.receivedAt, now);
            receiver->stats_.latestInputToRenderMs = inputToRenderMs;
            receiver->stats_.latestReceiveToRenderMs = receiveToRenderMs;
            receiver->stats_.maxInputToRenderMs =
                std::max(receiver->stats_.maxInputToRenderMs, inputToRenderMs);
            receiver->stats_.maxReceiveToRenderMs =
                std::max(receiver->stats_.maxReceiveToRenderMs, receiveToRenderMs);
            receiver->frameTimings_.erase(timing);
        }
    }
    receiver->stats_.renderedOutputs += 1;
}
