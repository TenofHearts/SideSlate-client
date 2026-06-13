#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace t2s {

constexpr uint8_t VERSION = 1;
constexpr size_t HEADER_SIZE = 24;
constexpr uint32_t MAX_PAYLOAD_LEN = 64 * 1024 * 1024;

constexpr uint8_t TYPE_HELLO = 1;
constexpr uint8_t TYPE_HELLO_ACK = 2;
constexpr uint8_t TYPE_VIDEO_CONFIG = 3;
constexpr uint8_t TYPE_VIDEO_CONFIG_ACK = 4;
constexpr uint8_t TYPE_VIDEO_PACKET = 5;
constexpr uint8_t TYPE_KEYFRAME_REQUEST = 6;
constexpr uint8_t TYPE_STATS = 7;
constexpr uint8_t TYPE_ERROR = 8;
constexpr uint8_t TYPE_STOP = 9;

constexpr uint16_t FLAG_KEYFRAME = 0x0001;
constexpr uint16_t FLAG_CONFIG_NAL = 0x0002;
constexpr uint16_t FLAG_VCL = 0x0004;
constexpr uint16_t FLAG_DROPPABLE = 0x0020;

constexpr uint32_t CAP_HEVC = 0x00000001;
constexpr uint32_t CAP_STATS = 0x00000004;
constexpr uint32_t CAP_KEYFRAME_REQUEST = 0x00000008;

constexpr uint8_t CODEC_HEVC = 1;

struct Message {
    uint8_t type = 0;
    uint16_t flags = 0;
    uint32_t sequence = 0;
    uint64_t timestampUs = 0;
    std::vector<uint8_t> payload;
};

struct VideoConfig {
    uint8_t codec = CODEC_HEVC;
    uint16_t width = 1920;
    uint16_t height = 1080;
    uint16_t fpsNum = 30;
    uint16_t fpsDen = 1;
    uint32_t bitrateKbps = 0;
    uint16_t gop = 15;
};

bool ReadMessage(int socketFd, Message& message, std::string& error);
bool WriteMessage(int socketFd, const Message& message, std::string& error);
bool ParseVideoConfig(const Message& message, VideoConfig& config, std::string& error);
std::vector<uint8_t> HelloAckPayload();
std::vector<uint8_t> EmptyPayload();

}
