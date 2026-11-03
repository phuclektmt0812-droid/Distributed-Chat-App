#include "test_helpers.h"
#include "../protocol/protocol.h"
#include "../protocol/message_payloads.h"
#include "../protocol/file_messages.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
SocketHandle connectAuthenticated(const std::string& username,
                                  const protocol::EncryptionKey& key) {
    SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(s != INVALID_SOCKET_HANDLE, "raw TCP socket creation failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(protocol::TCP_PORT);
    require(::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "raw TCP connection failed");
    protocol::Packet auth;
    auth.type = protocol::Type::AUTH;
    require(messages::encode(messages::Auth{username}, auth.payload), "AUTH encode failed");
    require(protocol::sendTcpPacketSecure(s, auth, key), "AUTH send failed");
    protocol::Packet response;
    require(protocol::receiveTcpPacketSecure(s, response, key) &&
            response.type == protocol::Type::AUTH_OK, "AUTH_OK missing");
    require(protocol::receiveTcpPacketSecure(s, response, key) &&
            response.type == protocol::Type::SESSION_INFO, "SESSION_INFO missing");
    require(protocol::receiveTcpPacketSecure(s, response, key) &&
            response.type == protocol::Type::USER_LIST, "initial USER_LIST missing");
    return s;
}
protocol::Packet waitForType(SocketHandle s, const protocol::EncryptionKey& key,
                             protocol::Type expected) {
    for (int i = 0; i < 8; ++i) {
        protocol::Packet response;
        require(protocol::receiveTcpPacketSecure(s, response, key),
                "server closed connection before expected response");
        if (response.type == expected) return response;
    }
    throw std::runtime_error("expected response type not received");
}
messages::Error readError(const protocol::Packet& packet) {
    messages::Error error;
    require(packet.type == protocol::Type::ERROR &&
            messages::decode(packet.payload, error), "malformed ERROR response");
    return error;
}
void testPingPong() {
    test::TestServer server;
    const auto key = server.server().encryptionKey();
    SocketHandle s = connectAuthenticated("ping-user", key);
    protocol::Packet ping;
    ping.type = protocol::Type::PING;
    ping.payload = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    require(protocol::sendTcpPacketSecure(s, ping, key), "PING send failed");
    const auto pong = waitForType(s, key, protocol::Type::PONG);
    require(pong.payload.size() == 24, "PONG must echo 16 bytes and append server timestamp");
    require(std::equal(ping.payload.begin(), ping.payload.end(), pong.payload.begin()),
            "PONG did not echo PING payload");
    closeSocket(s);
}
void testMalformedAndUnsupportedMessages() {
    test::TestServer server;
    const auto key = server.server().encryptionKey();
    SocketHandle s = connectAuthenticated("control-user", key);

    protocol::Packet malformed;
    malformed.type = protocol::Type::MSG;
    malformed.payload = {0x00};
    require(protocol::sendTcpPacketSecure(s, malformed, key), "malformed MSG send failed");
    auto error = readError(waitForType(s, key, protocol::Type::ERROR));
    require(error.code == protocol::ERR_MALFORMED_PACKET,
            "malformed MSG returned wrong error code");

    protocol::Packet unsupported;
    unsupported.type = protocol::Type::SESSION_INFO;
    require(protocol::sendTcpPacketSecure(s, unsupported, key), "unsupported packet send failed");
    error = readError(waitForType(s, key, protocol::Type::ERROR));
    require(error.code == protocol::ERR_UNKNOWN_MESSAGE_TYPE,
            "unsupported TCP packet returned wrong error code");

    fileproto::Failure missing;
    missing.id.fill(0xA1);
    missing.code = protocol::ERR_TRANSFER_CANCELLED;
    missing.detail = "test cancel";
    protocol::Packet cancel;
    cancel.type = protocol::Type::FILE_CANCEL;
    require(fileproto::encode(missing, cancel.payload), "FILE_CANCEL encode failed");
    require(protocol::sendTcpPacketSecure(s, cancel, key), "FILE_CANCEL send failed");
    error = readError(waitForType(s, key, protocol::Type::ERROR));
    require(error.code == protocol::ERR_TRANSFER_NOT_FOUND,
            "unknown FILE_CANCEL returned wrong error code");
    closeSocket(s);
}
void testMalformedPingAndLogoutAck() {
    test::TestServer server;
    const auto key = server.server().encryptionKey();
    SocketHandle s = connectAuthenticated("logout-control-user", key);
    protocol::Packet ping;
    ping.type = protocol::Type::PING;
    ping.payload = {1};
    require(protocol::sendTcpPacketSecure(s, ping, key), "malformed PING send failed");
    auto error = readError(waitForType(s, key, protocol::Type::ERROR));
    require(error.code == protocol::ERR_MALFORMED_PACKET,
            "malformed PING returned wrong error code");

    protocol::Packet logout;
    logout.type = protocol::Type::LOGOUT;
    require(protocol::sendTcpPacketSecure(s, logout, key), "LOGOUT send failed");
    protocol::Packet ackPacket;
    require(protocol::receiveTcpPacketSecure(s, ackPacket, key) &&
            ackPacket.type == protocol::Type::ACK, "LOGOUT ACK missing");
    messages::Ack ack;
    require(messages::decode(ackPacket.payload, ack) && ack.status == 1,
            "LOGOUT ACK payload incorrect");
    closeSocket(s);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (server.server().users().find("logout-control-user") &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(!server.server().users().find("logout-control-user"),
            "LOGOUT did not remove server session");
}
}
int main() {
    try {
        require(initializeSocketRuntime(), "socket runtime init failed");
        testPingPong();
        testMalformedAndUnsupportedMessages();
        testMalformedPingAndLogoutAck();
        cleanupSocketRuntime();
        std::cout << "TCP control/error handling tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        cleanupSocketRuntime();
        std::cerr << "TCP control tests failed: " << e.what() << '\n';
        return 1;
    }
}