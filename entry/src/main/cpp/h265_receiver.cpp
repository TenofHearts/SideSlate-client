#include "h265_receiver.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <multimedia/player_framework/native_avbuffer_info.h>

namespace {
constexpr uint32_t T2S_LOG_DOMAIN = 0x545253;
constexpr const char* T2S_LOG_TAG = "T2SH265";
constexpr uint8_t MAGIC[] = {'T', '2', 'H', '5'};
constexpr size_t HEADER_SIZE = 24;
constexpr size_t MAX_PACKET_BYTES = 64 * 1024 * 1024;
constexpr size_t MAX_PENDING_PACKETS = 3;

uint32_t ReadU32Le(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t ReadU64Le(const uint8_t* data)
{
    uint64_t value = 0;
    for (int index = 7; index >= 0; --index) {
        value = (value << 8) | data[index];
    }
    return value;
}

bool RecvExact(int socketFd, uint8_t* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        ssize_t readSize = recv(socketFd, data + offset, size - offset, 0);
        if (readSize <= 0) {
            return false;
        }
        offset += static_cast<size_t>(readSize);
    }
    return true;
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
    if (running_ && !decoder_) {
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
        if (nativeWindow_) {
            StartDecoderLocked();
        }
    }

    receiverThread_ = std::thread(&H265Receiver::ReceiverLoop, this, port);
    decodeThread_ = std::thread(&H265Receiver::DecodeLoop, this);
    return true;
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
        receiveTimeout.tv_sec = 2;
        receiveTimeout.tv_usec = 0;
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));
        SetStatus("sender connected");
        std::vector<uint8_t> header(HEADER_SIZE);
        while (running_ && RecvExact(clientFd, header.data(), header.size())) {
            if (!std::equal(std::begin(MAGIC), std::end(MAGIC), header.begin())) {
                SetError(-2, "bad packet magic");
                break;
            }
            Packet packet;
            packet.sequence = ReadU32Le(header.data() + 4);
            packet.timestampUs = ReadU64Le(header.data() + 8);
            packet.flags = ReadU32Le(header.data() + 16);
            uint32_t payloadLen = ReadU32Le(header.data() + 20);
            if (payloadLen == 0 || payloadLen > MAX_PACKET_BYTES) {
                SetError(-3, "invalid payload length");
                break;
            }
            packet.payload.resize(payloadLen);
            if (!RecvExact(clientFd, packet.payload.data(), packet.payload.size())) {
                break;
            }
            EnqueuePacket(std::move(packet));
        }
        close(clientFd);
        SetStatus("sender disconnected");
    }
    close(serverFd);
}

void H265Receiver::DecodeLoop()
{
    while (running_) {
        Packet packet;
        InputBufferRef input;
        if (!PopPacket(packet)) {
            continue;
        }
        if (!PopInputBuffer(input)) {
            continue;
        }
        PushInputBuffer(input.index, input.buffer, packet);
    }
}

void H265Receiver::EnqueuePacket(Packet&& packet)
{
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.packets += 1;
    stats_.bytes += packet.payload.size();
    while (packets_.size() >= MAX_PENDING_PACKETS) {
        packets_.pop_front();
        stats_.droppedPackets += 1;
    }
    packets_.push_back(std::move(packet));
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
        SetError(-4, "input buffer too small");
        return;
    }

    std::memcpy(addr, packet.payload.data(), packet.payload.size());
    OH_AVCodecBufferAttr attr {};
    attr.pts = static_cast<int64_t>(packet.timestampUs);
    attr.size = static_cast<int32_t>(packet.payload.size());
    attr.offset = 0;
    attr.flags = 0;
    OH_AVBuffer_SetBufferAttr(buffer, &attr);

    OH_AVErrCode result = OH_VideoDecoder_PushInputBuffer(decoder_, index);
    if (result != AV_ERR_OK) {
        SetError(result, "push input failed");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
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

void H265Receiver::OnNewOutputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer*, void* userData)
{
    auto* receiver = static_cast<H265Receiver*>(userData);
    OH_AVErrCode result = OH_VideoDecoder_RenderOutputBuffer(codec, index);
    if (result != AV_ERR_OK) {
        receiver->SetError(result, "render output failed");
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }
    std::lock_guard<std::mutex> lock(receiver->mutex_);
    receiver->stats_.renderedOutputs += 1;
}
