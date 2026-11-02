#include "test_helpers.h"

#include "../clients/connection.h"
#include "../protocol/udp_packet.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

void testUdpPacketRoundTrip() {
    UdpPacket original{};
    original.type = UdpPacketType::FILE_DATA;
    original.transferId = 42;
    original.sequenceNumber = 7;
    original.payload = {'t', 'e', 's', 't'};
    original.payloadLength = static_cast<std::uint16_t>(original.payload.size());

    const std::vector<std::uint8_t> encoded = encodeUdpPacket(original);
    test::require(!encoded.empty(), "Could not encode a valid UDP packet");

    UdpPacket decoded{};
    test::require(
        decodeUdpPacket(encoded.data(), encoded.size(), decoded),
        "Could not decode an encoded UDP packet"
    );
    test::require(decoded.transferId == original.transferId,
        "Decoded transfer ID did not match");
    test::require(decoded.sequenceNumber == original.sequenceNumber,
        "Decoded sequence number did not match");
    test::require(decoded.payload == original.payload,
        "Decoded file chunk did not match");

    std::vector<std::uint8_t> corrupted = encoded;
    corrupted.back() ^= 0x01;
    test::require(
        !decodeUdpPacket(corrupted.data(), corrupted.size(), decoded),
        "Corrupted packet passed checksum validation"
    );
    test::require(
        !decodeUdpPacket(encoded.data(), encoded.size() - 1, decoded),
        "Truncated packet was accepted"
    );

    UdpPacket duplicate{};
    test::require(
        decodeUdpPacket(encoded.data(), encoded.size(), duplicate) &&
            duplicate.transferId == original.transferId &&
            duplicate.sequenceNumber == original.sequenceNumber,
        "Repeated packet decode changed its transfer or sequence identifiers"
    );
}

void testRejectsInvalidTransferId(std::uint16_t port) {
    Connection client("127.0.0.1", port);
    test::require(client.connectToServer("127.0.0.1", port),
        "Could not connect for invalid-transfer test");

    Message login{};
    login.type = MessageType::LOGIN;
    const std::uint16_t senderUdpPort = 50001;
    login.payload = {
        {"username", "file-sender"},
        {"password", "test-password"},
        {"udp_port", senderUdpPort}
    };
    test::require(client.sendFramedMessage(login),
        "Could not send file-transfer test login");

    Message response{};
    test::require(client.receiveFramedMessage(response) &&
            response.type == MessageType::LOGIN_OK,
        "Could not authenticate file-transfer test client");

    Message invalidOffer{};
    invalidOffer.type = MessageType::FILE_START;
    const std::uint32_t invalidTransferId = 0;
    const std::uint64_t totalSize = 4;
    invalidOffer.payload = {
        {"transfer_id", invalidTransferId},
        {"recipient", "file-recipient"},
        {"filename", "test.bin"},
        {"total_size", totalSize},
        {"udp_port", senderUdpPort}
    };
    test::require(client.sendFramedMessage(invalidOffer),
        "Could not send invalid file-transfer offer");
    test::require(client.receiveFramedMessage(response) &&
            response.type == MessageType::FILE_REJECT,
        "Server did not reject a zero transfer ID");
    client.disconnect();
}

} // namespace

int main() {
    try {
        testUdpPacketRoundTrip();
        test::TestServer server;
        testRejectsInvalidTransferId(server.port());

        std::cout << "File-transfer packet and validation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "File-transfer tests failed: " << error.what() << '\n';
        return 1;
    }
}