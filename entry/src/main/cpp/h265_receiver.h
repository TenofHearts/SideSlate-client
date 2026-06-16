#pragma once

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <vector>

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>

struct H265Stats {
    bool running = false;
    bool decoderStarted = false;
    bool surfaceReady = false;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t queuedInputs = 0;
    uint64_t renderedOutputs = 0;
    uint64_t droppedPackets = 0;
    uint64_t sequenceGaps = 0;
    uint64_t configPackets = 0;
    uint64_t keyframes = 0;
    uint32_t lastSequence = 0;
    uint32_t queueDepth = 0;
    int32_t streamWidth = 0;
    int32_t streamHeight = 0;
    int32_t streamFps = 0;
    int32_t lastError = 0;
    double maxReceiveGapMs = 0.0;
    double maxInputGapMs = 0.0;
    double maxRenderGapMs = 0.0;
    double latestReceiveToInputMs = 0.0;
    double latestInputToRenderMs = 0.0;
    double latestReceiveToRenderMs = 0.0;
    double maxReceiveToInputMs = 0.0;
    double maxInputToRenderMs = 0.0;
    double maxReceiveToRenderMs = 0.0;
    std::string status = "stopped";
};

class H265Receiver {
public:
    static H265Receiver& Instance();

    void RegisterXComponent(OH_NativeXComponent* component);
    void OnSurfaceCreated(OH_NativeXComponent* component, void* window);
    void OnSurfaceChanged(OH_NativeXComponent* component, void* window);
    void OnSurfaceDestroyed(OH_NativeXComponent* component, void* window);

    bool Start(uint16_t port, int32_t width, int32_t height);
    void Pause();
    void Stop();
    H265Stats GetStats();

private:
    H265Receiver() = default;
    ~H265Receiver();
    H265Receiver(const H265Receiver&) = delete;
    H265Receiver& operator=(const H265Receiver&) = delete;

    struct Packet {
        uint32_t sequence = 0;
        uint64_t timestampUs = 0;
        uint16_t flags = 0;
        std::chrono::steady_clock::time_point receivedAt {};
        std::vector<uint8_t> payload;
    };

    struct InputBufferRef {
        uint32_t index = 0;
        OH_AVBuffer* buffer = nullptr;
    };

    struct FrameTiming {
        std::chrono::steady_clock::time_point receivedAt {};
        std::chrono::steady_clock::time_point inputAt {};
    };

    bool StartDecoderLocked();
    void StopDecoderLocked();
    void ReceiverLoop(uint16_t port);
    bool HandleClient(int clientFd);
    void DecodeLoop();
    void EnqueuePacket(Packet&& packet);
    bool PopPacket(Packet& packet);
    bool PopInputBuffer(InputBufferRef& input);
    void PushInputBuffer(uint32_t index, OH_AVBuffer* buffer, const Packet& packet);
    void SetStatus(const std::string& status);
    void SetError(int32_t error, const std::string& status);
    void SetErrorLocked(int32_t error, const std::string& status);

    static void OnCodecError(OH_AVCodec* codec, int32_t errorCode, void* userData);
    static void OnCodecStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* userData);
    static void OnNeedInputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData);
    static void OnNewOutputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* userData);

    std::mutex mutex_;
    std::condition_variable packetCv_;
    std::condition_variable inputCv_;
    std::atomic<bool> running_ {false};
    std::thread receiverThread_;
    std::thread decodeThread_;

    OH_NativeXComponent* component_ = nullptr;
    OHNativeWindow* nativeWindow_ = nullptr;
    OH_AVCodec* decoder_ = nullptr;
    int32_t width_ = 1920;
    int32_t height_ = 1080;
    bool configured_ = false;
    bool hasLastSequence_ = false;

    std::deque<Packet> packets_;
    std::deque<InputBufferRef> inputBuffers_;
    std::unordered_map<int64_t, FrameTiming> frameTimings_;
    std::chrono::steady_clock::time_point lastReceiveAt_ {};
    std::chrono::steady_clock::time_point lastInputAt_ {};
    std::chrono::steady_clock::time_point lastRenderAt_ {};
    H265Stats stats_;
};
