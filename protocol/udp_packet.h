#pragma once

#include <cstdint>
#include <vector>

enum class UdpPacketType : std::uint8_t {
    FILE_DATA = 1,
    FILE_ACK = 2,
    FILE_FIN = 3,
    FILE_FIN_ACK = 4,
    FILE_ABORT = 5
};

struct UdpPacket {
    UdpPacketType type;

    std::uint32_t transferId;
    std::uint32_t sequenceNumber;

    std::uint16_t payloadLength;

    std::uint32_t checksum;

    std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> encodeUdpPacket(
    const UdpPacket& packet
);

bool decodeUdpPacket(
    const std::uint8_t* data,
    std::size_t length,
    UdpPacket& packet
);