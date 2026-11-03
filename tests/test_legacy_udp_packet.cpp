#include "../protocol/udp_packet.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::uint32_t checksum(const std::uint8_t* data, std::size_t length) {
    std::uint32_t value = 2166136261u;
    for (std::size_t i = 0; i < length; ++i) {
        value ^= data[i];
        value *= 16777619u;
    }
    return value;
}
void write32(std::uint8_t* p, std::uint32_t value) {
    p[0] = static_cast<std::uint8_t>(value >> 24);
    p[1] = static_cast<std::uint8_t>(value >> 16);
    p[2] = static_cast<std::uint8_t>(value >> 8);
    p[3] = static_cast<std::uint8_t>(value);
}
void testRoundTripAndChecksum() {
    UdpPacket source{};
    source.type = UdpPacketType::FILE_DATA;
    source.transferId = 0x10203040;
    source.sequenceNumber = 0x50607080;
    source.payload = {0, 1, 127, 128, 255};
    source.payloadLength = static_cast<std::uint16_t>(source.payload.size());
    const auto wire = encodeUdpPacket(source);
    require(!wire.empty(), "legacy UDP packet encode failed");
    UdpPacket decoded{};
    require(decodeUdpPacket(wire.data(), wire.size(), decoded),
            "legacy UDP packet decode failed");
    require(decoded.type == source.type && decoded.transferId == source.transferId &&
            decoded.sequenceNumber == source.sequenceNumber && decoded.payload == source.payload,
            "legacy UDP packet changed during round trip");
    auto corrupt = wire;
    corrupt.back() ^= 1;
    require(!decodeUdpPacket(corrupt.data(), corrupt.size(), decoded),
            "legacy UDP checksum did not detect payload corruption");
}
void testMalformedAndInvalidPackets() {
    UdpPacket packet{};
    require(!decodeUdpPacket(nullptr, 0, packet), "null legacy UDP input accepted");
    const std::vector<std::uint8_t> shortHeader(18, 0);
    require(!decodeUdpPacket(shortHeader.data(), shortHeader.size(), packet),
            "short legacy UDP header accepted");
    UdpPacket source{};
    source.type = UdpPacketType::FILE_ACK;
    source.payload = {1, 2};
    source.payloadLength = 1;
    require(encodeUdpPacket(source).empty(),
            "legacy UDP encoder accepted inconsistent payload length");
    source.payloadLength = 2;
    auto wire = encodeUdpPacket(source);
    wire[0] ^= 1;
    require(!decodeUdpPacket(wire.data(), wire.size(), packet),
            "legacy UDP decoder accepted invalid magic");

    wire = encodeUdpPacket(source);
    wire[4] = 99;
    std::vector<std::uint8_t> checksumInput(wire.begin() + 4, wire.end());
    std::fill(checksumInput.begin() + 11, checksumInput.begin() + 15, 0);
    write32(wire.data() + 15, checksum(checksumInput.data(), checksumInput.size()));
    require(!decodeUdpPacket(wire.data(), wire.size(), packet),
            "legacy UDP decoder accepted invalid type");

    wire = encodeUdpPacket(source);
    wire[13] = 0;
    wire[14] = 1;
    require(!decodeUdpPacket(wire.data(), wire.size(), packet),
            "legacy UDP decoder accepted inconsistent frame length");
}
}
int main() {
    try {
        testRoundTripAndChecksum();
        testMalformedAndInvalidPackets();
        std::cout << "Legacy UDP packet codec tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Legacy UDP packet tests failed: " << e.what() << '\n';
        return 1;
    }
}