#include "../protocol/udp_packet.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace {

constexpr std::array<std::uint8_t, 4> MAGIC{{'D', 'C', 'H', 'T'}};
constexpr std::size_t HEADER_SIZE = 19;

std::uint32_t checksum(
    const std::uint8_t* data,
    std::size_t length
) {
    std::uint32_t value = 2166136261u;
    for (std::size_t index = 0; index < length; ++index) {
        value ^= data[index];
        value *= 16777619u;
    }
    return value;
}

void write16(std::uint8_t* output, std::uint16_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 8);
    output[1] = static_cast<std::uint8_t>(value);
}

void write32(std::uint8_t* output, std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24);
    output[1] = static_cast<std::uint8_t>(value >> 16);
    output[2] = static_cast<std::uint8_t>(value >> 8);
    output[3] = static_cast<std::uint8_t>(value);
}

std::uint16_t read16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(input[0]) << 8) | input[1]
    );
}

std::uint32_t read32(const std::uint8_t* input) {
    return (static_cast<std::uint32_t>(input[0]) << 24) |
        (static_cast<std::uint32_t>(input[1]) << 16) |
        (static_cast<std::uint32_t>(input[2]) << 8) |
        static_cast<std::uint32_t>(input[3]);
}

bool validType(std::uint8_t type) {
    return type >= static_cast<std::uint8_t>(UdpPacketType::FILE_DATA) &&
        type <= static_cast<std::uint8_t>(UdpPacketType::FILE_ABORT);
}

} // namespace

std::vector<std::uint8_t> encodeUdpPacket(const UdpPacket& packet) {
    if (packet.payload.size() > std::numeric_limits<std::uint16_t>::max() ||
        packet.payload.size() != packet.payloadLength) {
        return {};
    }

    std::vector<std::uint8_t> encoded(HEADER_SIZE + packet.payload.size());
    std::copy(MAGIC.begin(), MAGIC.end(), encoded.begin());
    encoded[4] = static_cast<std::uint8_t>(packet.type);
    write32(encoded.data() + 5, packet.transferId);
    write32(encoded.data() + 9, packet.sequenceNumber);
    write16(encoded.data() + 13, packet.payloadLength);
    write32(encoded.data() + 15, 0);
    std::copy(
        packet.payload.begin(),
        packet.payload.end(),
        encoded.begin() + HEADER_SIZE
    );

    const std::uint32_t value = checksum(
        encoded.data() + 4,
        encoded.size() - 4
    );
    write32(encoded.data() + 15, value);
    return encoded;
}

bool decodeUdpPacket(
    const std::uint8_t* data,
    std::size_t length,
    UdpPacket& packet
) {
    if (data == nullptr || length < HEADER_SIZE ||
        !std::equal(MAGIC.begin(), MAGIC.end(), data) ||
        !validType(data[4])) {
        return false;
    }

    const std::uint16_t payloadLength = read16(data + 13);
    if (length != HEADER_SIZE + payloadLength) {
        return false;
    }

    std::vector<std::uint8_t> checksumInput(data + 4, data + length);
    const std::uint32_t expectedChecksum = read32(data + 15);
    std::fill(checksumInput.begin() + 11, checksumInput.begin() + 15, 0);
    if (checksum(checksumInput.data(), checksumInput.size()) != expectedChecksum) {
        return false;
    }

    packet.type = static_cast<UdpPacketType>(data[4]);
    packet.transferId = read32(data + 5);
    packet.sequenceNumber = read32(data + 9);
    packet.payloadLength = payloadLength;
    packet.checksum = expectedChecksum;
    packet.payload.assign(data + HEADER_SIZE, data + length);
    return true;
}
