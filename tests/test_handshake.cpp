#include "test_helpers.h"
#include "../protocol/protocol.h"
#include "../protocol/message_payloads.h"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
SocketHandle connectRaw() {
    SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(s != INVALID_SOCKET_HANDLE, "raw TCP socket creation failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(protocol::TCP_PORT);
    require(::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "raw TCP connect failed");
    return s;
}
protocol::Packet receiveSecure(SocketHandle s, const protocol::EncryptionKey& key) {
    std::array<std::uint8_t, protocol::HEADER_SIZE> header{};
    require(receiveExact(s, header.data(), header.size()), "server response header missing");
    const auto length = protocol::readU32(header.data() + 10);
    require(length <= protocol::MAX_TCP_PAYLOAD, "server response length invalid");
    protocol::Bytes wire(header.begin(), header.end());
    wire.resize(protocol::HEADER_SIZE + length);
    if (length)
        require(receiveExact(s, wire.data() + protocol::HEADER_SIZE, length),
                "server response body truncated");
    protocol::Packet response;
    require(protocol::decodeSecurePacket(wire.data(), wire.size(), key, response),
            "server secure response failed to decode");
    return response;
}
void testAuthMustBeFirst() {
    test::TestServer server;
    const auto key = server.server().encryptionKey();
    SocketHandle socket = connectRaw();
    messages::ChatRequest chat{1, "unauthenticated"};
    protocol::Packet request;
    request.type = protocol::Type::MSG;
    require(messages::encode(chat, request.payload), "could not encode pre-auth message");
    require(protocol::sendTcpPacketSecure(socket, request, key),
            "could not send pre-auth message");
    const auto response = receiveSecure(socket, key);
    require(response.type == protocol::Type::AUTH_FAIL, "server did not reject non-AUTH first packet");
    messages::AuthFail failure;
    require(messages::decode(response.payload, failure), "AUTH_FAIL payload malformed");
    require(failure.code == protocol::ERR_NOT_AUTHENTICATED,
            "server returned wrong pre-auth error code");
    closeSocket(socket);
}
void testMalformedAuthRejected() {
    test::TestServer server;
    const auto key = server.server().encryptionKey();
    SocketHandle socket = connectRaw();
    protocol::Packet request;
    request.type = protocol::Type::AUTH;
    request.payload = {0x00, 0x05, 'a'}; // truncated username field
    require(protocol::sendTcpPacketSecure(socket, request, key),
            "could not send malformed AUTH");
    const auto response = receiveSecure(socket, key);
    require(response.type == protocol::Type::AUTH_FAIL, "malformed AUTH not rejected");
    messages::AuthFail failure;
    require(messages::decode(response.payload, failure), "malformed AUTH_FAIL response");
    require(failure.code == protocol::ERR_INVALID_USERNAME,
            "malformed AUTH returned wrong error code");
    closeSocket(socket);
}
}
int main() {
    try {
        require(initializeSocketRuntime(), "socket runtime init failed");
        testAuthMustBeFirst();
        testMalformedAuthRejected();
        cleanupSocketRuntime();
        std::cout << "TCP handshake negative tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        cleanupSocketRuntime();
        std::cerr << "TCP handshake tests failed: " << e.what() << '\n';
        return 1;
    }
}