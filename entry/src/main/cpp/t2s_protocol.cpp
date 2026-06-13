#include "t2s_protocol.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>

namespace t2s {
namespace {
constexpr uint8_t MAGIC[] = {'T', '2', 'S', '1'};

uint16_t ReadU16Le(const uint8_t* data)
{
    return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1] << 8);
}

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

void WriteU16Le(uint8_t* data, uint16_t value)
{
    data[0] = static_cast<uint8_t>(value & 0xff);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void WriteU32Le(uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value & 0xff);
    data[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    data[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    data[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

void WriteU64Le(uint8_t* data, uint64_t value)
{
    for (int index = 0; index < 8; ++index) {
        data[index] = static_cast<uint8_t>((value >> (index * 8)) & 0xff);
    }
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

bool SendExact(int socketFd, const uint8_t* data, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        ssize_t sent = send(socketFd, data + offset, size - offset, 0);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}
}

bool ReadMessage(int socketFd, Message& message, std::string& error)
{
    uint8_t header[HEADER_SIZE] {};
    if (!RecvExact(socketFd, header, sizeof(header))) {
        error = "socket header read failed";
        return false;
    }
    if (!std::equal(std::begin(MAGIC), std::end(MAGIC), header)) {
        error = "bad protocol magic";
        return false;
    }
    if (header[4] != VERSION) {
        error = "unsupported protocol version";
        return false;
    }

    uint32_t payloadLen = ReadU32Le(header + 20);
    if (payloadLen > MAX_PAYLOAD_LEN) {
        error = "payload too large";
        return false;
    }

    message.type = header[5];
    message.flags = ReadU16Le(header + 6);
    message.sequence = ReadU32Le(header + 8);
    message.timestampUs = ReadU64Le(header + 12);
    message.payload.assign(payloadLen, 0);
    if (payloadLen > 0 && !RecvExact(socketFd, message.payload.data(), message.payload.size())) {
        error = "socket payload read failed";
        return false;
    }
    return true;
}

bool WriteMessage(int socketFd, const Message& message, std::string& error)
{
    if (message.payload.size() > MAX_PAYLOAD_LEN) {
        error = "payload too large";
        return false;
    }

    uint8_t header[HEADER_SIZE] {};
    std::copy(std::begin(MAGIC), std::end(MAGIC), header);
    header[4] = VERSION;
    header[5] = message.type;
    WriteU16Le(header + 6, message.flags);
    WriteU32Le(header + 8, message.sequence);
    WriteU64Le(header + 12, message.timestampUs);
    WriteU32Le(header + 20, static_cast<uint32_t>(message.payload.size()));

    if (!SendExact(socketFd, header, sizeof(header))) {
        error = "socket header write failed";
        return false;
    }
    if (!message.payload.empty() && !SendExact(socketFd, message.payload.data(), message.payload.size())) {
        error = "socket payload write failed";
        return false;
    }
    return true;
}

bool ParseVideoConfig(const Message& message, VideoConfig& config, std::string& error)
{
    if (message.type != TYPE_VIDEO_CONFIG) {
        error = "expected video config";
        return false;
    }
    if (message.payload.size() < 16) {
        error = "video config payload too short";
        return false;
    }
    config.codec = message.payload[0];
    config.width = ReadU16Le(message.payload.data() + 2);
    config.height = ReadU16Le(message.payload.data() + 4);
    config.fpsNum = ReadU16Le(message.payload.data() + 6);
    config.fpsDen = ReadU16Le(message.payload.data() + 8);
    config.bitrateKbps = ReadU32Le(message.payload.data() + 10);
    config.gop = ReadU16Le(message.payload.data() + 14);
    if (config.codec != CODEC_HEVC || config.width == 0 || config.height == 0) {
        error = "unsupported video config";
        return false;
    }
    return true;
}

std::vector<uint8_t> HelloAckPayload()
{
    std::vector<uint8_t> payload;
    payload.reserve(8);
    payload.insert(payload.end(), {VERSION, 1, 0, 0});
    uint32_t capabilities = CAP_HEVC | CAP_STATS | CAP_KEYFRAME_REQUEST;
    payload.push_back(static_cast<uint8_t>(capabilities & 0xff));
    payload.push_back(static_cast<uint8_t>((capabilities >> 8) & 0xff));
    payload.push_back(static_cast<uint8_t>((capabilities >> 16) & 0xff));
    payload.push_back(static_cast<uint8_t>((capabilities >> 24) & 0xff));
    return payload;
}

std::vector<uint8_t> EmptyPayload()
{
    return {};
}

}
