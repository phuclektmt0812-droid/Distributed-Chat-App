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
SocketHandle makeUdpSocket() {
    SocketHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    require(s != INVALID_SOCKET_HANDLE, "UDP socket creation failed");
    require(setSocketTimeout(s, 700, 0), "UDP receive timeout setup failed");
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = 0;
    require(::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0,
            "UDP local bind failed");
    return s;
}
bool sendPacket(SocketHandle s, const protocol::EncryptionKey& key,
                const sockaddr_in& destination, const protocol::Packet& packet) {
    return protocol::sendUdpPacketSecure(s, packet, destination, key);
}
bool receivePacket(SocketHandle s, const protocol::EncryptionKey& key,
                   protocol::Packet& packet) {
    std::array<std::uint8_t, protocol::MAX_UDP_DATAGRAM + 1> bytes{};
    sockaddr_in source{};
    SocketLength length = sizeof(source);
#ifdef _WIN32
    const int n = recvfrom(s, reinterpret_cast<char*>(bytes.data()),
        static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&source), &length);
#else
    const auto n = recvfrom(s, bytes.data(), bytes.size(), 0,
        reinterpret_cast<sockaddr*>(&source), &length);
#endif
    return n > 0 && protocol::decodeSecureDatagram(bytes.data(),
        static_cast<std::size_t>(n), key, packet);
}
void testRegistrationHeartbeatDiscovery() {
    test::TestServer server;
    DcapClient client("127.0.0.1");
    test::connectAndLogin(client, "udp-user");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::shared_ptr<ClientSession> session;
    while (std::chrono::steady_clock::now() < deadline) {
        session = server.server().users().find("udp-user");
        if (session && session->udpRegistered) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(session != nullptr, "authenticated TCP session missing");
    require(session->udpRegistered, "client did not register UDP endpoint");

    const auto key = server.server().encryptionKey();
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(protocol::UDP_PORT);

    SocketHandle raw = makeUdpSocket();
    messages::UdpRegister registration{session->udpToken};
    protocol::Packet request;
    request.type = protocol::Type::UDP_REGISTER;
    require(messages::encode(registration, request.payload), "UDP_REGISTER encode failed");
    require(sendPacket(raw, key, destination, request), "UDP_REGISTER send failed");
    protocol::Packet response;
    require(receivePacket(raw, key, response), "UDP_REGISTER response missing");
    require(response.type == protocol::Type::UDP_REGISTER_OK, "wrong registration response type");
    messages::UdpRegisterOk registered;
    require(messages::decode(response.payload, registered) && registered.registered,
            "valid token was not accepted");

    messages::Heartbeat heartbeat{0x123456789ULL, 987654321ULL};
    request = {};
    request.type = protocol::Type::HEARTBEAT;
    require(messages::encode(heartbeat, request.payload), "HEARTBEAT encode failed");
    require(sendPacket(raw, key, destination, request), "HEARTBEAT send failed");
    require(receivePacket(raw, key, response), "HEARTBEAT_ACK missing");
    require(response.type == protocol::Type::HEARTBEAT_ACK, "wrong heartbeat response type");
    messages::HeartbeatAck ack;
    require(messages::decode(response.payload, ack), "HEARTBEAT_ACK decode failed");
    require(ack.nonce == heartbeat.nonce && ack.client_time_ms == heartbeat.client_time_ms,
            "heartbeat correlation fields changed");

    request = {};
    request.type = protocol::Type::DISCOVERY;
    require(sendPacket(raw, key, destination, request), "DISCOVERY send failed");
    require(receivePacket(raw, key, response), "DISCOVERY_REPLY missing");
    require(response.type == protocol::Type::DISCOVERY_REPLY, "wrong discovery response type");
    messages::UserList list;
    require(messages::decode(response.payload, list), "DISCOVERY_REPLY decode failed");
    require(test::contains(list.users, "udp-user"), "discovery omitted online user");

    // A valid token is not enough after registration: datagrams must come
    // from the endpoint that owns the registration.
    SocketHandle unknown = makeUdpSocket();
    request = {};
    request.type = protocol::Type::HEARTBEAT;
    require(messages::encode(heartbeat, request.payload), "unknown-endpoint heartbeat encode failed");
    require(sendPacket(unknown, key, destination, request), "unknown-endpoint heartbeat send failed");
    require(!receivePacket(unknown, key, response),
            "unregistered UDP endpoint received a heartbeat response");

    // Malformed heartbeat and non-empty discovery payloads are silently ignored.
    request.payload = {1};
    require(sendPacket(raw, key, destination, request), "malformed heartbeat send failed");
    require(!receivePacket(raw, key, response), "malformed heartbeat received a response");
    request = {};
    request.type = protocol::Type::DISCOVERY;
    request.payload = {1};
    require(sendPacket(raw, key, destination, request), "malformed discovery send failed");
    require(!receivePacket(raw, key, response), "malformed discovery received a response");

    closeSocket(unknown);
    closeSocket(raw);
    client.disconnect();
}
void testInvalidTokenRejected() {
    test::TestServer server;
    SocketHandle raw = makeUdpSocket();
    const auto key = server.server().encryptionKey();
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(protocol::UDP_PORT);
    messages::UdpRegister registration{};
    registration.token.fill(0xA5);
    protocol::Packet request;
    request.type = protocol::Type::UDP_REGISTER;
    require(messages::encode(registration, request.payload), "invalid register encode failed");
    require(protocol::sendUdpPacketSecure(raw, request, destination, key),
            "invalid registration send failed");
    protocol::Packet response;
    require(receivePacket(raw, key, response), "invalid registration response missing");
    messages::UdpRegisterOk result;
    require(response.type == protocol::Type::UDP_REGISTER_OK &&
            messages::decode(response.payload, result) && !result.registered &&
            result.code == protocol::ERR_INVALID_SESSION,
            "invalid UDP token was not rejected correctly");
    closeSocket(raw);
}
}
int main() {
    try {
        require(initializeSocketRuntime(), "socket runtime init failed");
        testInvalidTokenRejected();
        testRegistrationHeartbeatDiscovery();
        cleanupSocketRuntime();
        std::cout << "UDP service tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "UDP service tests failed: " << e.what() << '\n';
        cleanupSocketRuntime();
        return 1;
    }
}