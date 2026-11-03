#include "../protocol/protocol.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct TcpPair {
    SocketHandle listener = INVALID_SOCKET_HANDLE;
    SocketHandle client = INVALID_SOCKET_HANDLE;
    SocketHandle peer = INVALID_SOCKET_HANDLE;
    TcpPair() {
        listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listener != INVALID_SOCKET_HANDLE, "TCP listener creation failed");
        setReuseAddress(listener);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "TCP listener bind failed");
        require(::listen(listener, 1) == 0, "TCP listen failed");
        SocketLength length = sizeof(address);
        require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0,
                "TCP getsockname failed");
        client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(client != INVALID_SOCKET_HANDLE, "TCP client creation failed");
        require(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "TCP client connect failed");
        peer = ::accept(listener, nullptr, nullptr);
        require(peer != INVALID_SOCKET_HANDLE, "TCP accept failed");
    }
    ~TcpPair() {
        closeSocket(peer);
        closeSocket(client);
        closeSocket(listener);
    }
};
void testIntegerAndStringHelpers() {
    protocol::Bytes bytes;
    protocol::putU16(bytes, 0xABCD);
    protocol::putU32(bytes, 0x12345678);
    protocol::putU64(bytes, 0x0102030405060708ULL);
    require(protocol::readU16(bytes.data()) == 0xABCD, "put/read U16 mismatch");
    require(protocol::readU32(bytes.data() + 2) == 0x12345678, "put/read U32 mismatch");
    require(protocol::readU64(bytes.data() + 6) == 0x0102030405060708ULL,
            "put/read U64 mismatch");
    protocol::Bytes text;
    require(protocol::putString(text, "hello 🌍"), "valid protocol string rejected");
    std::size_t offset = 0;
    std::string decoded;
    require(protocol::getString(text, offset, decoded, 32) && decoded == "hello 🌍" &&
            offset == text.size(), "putString/getString round trip failed");
    require(!protocol::putString(text, std::string("\xFF", 1)),
            "invalid UTF-8 protocol string accepted");
    protocol::Bytes truncated{0, 5, 'a'};
    offset = 0;
    require(!protocol::getString(truncated, offset, decoded), "truncated protocol string accepted");
    require(!protocol::getString(text, text.size() + 1, decoded),
            "string offset beyond buffer accepted");
}
void testPlainAndSecureTcp() {
    {
        TcpPair pair;
        protocol::Packet source{protocol::Type::MSG, 17, {1, 2, 3, 4}};
        require(protocol::sendTcpPacket(pair.client, source), "plain TCP packet send failed");
        protocol::Packet decoded;
        require(protocol::receiveTcpPacket(pair.peer, decoded), "plain TCP packet receive failed");
        require(decoded.type == source.type && decoded.sequence == source.sequence &&
                decoded.payload == source.payload, "plain TCP packet changed");
        protocol::Packet wrongTransport{protocol::Type::HEARTBEAT, 1, {}};
        require(!protocol::sendTcpPacket(pair.client, wrongTransport),
                "UDP type was accepted by TCP sender");
    }
    {
        TcpPair pair;
        protocol::EncryptionKey key{};
        key.fill(0x6C);
        protocol::Packet source{protocol::Type::PRIVATE_MSG, 99, {9, 8, 7}};
        require(protocol::sendTcpPacketSecure(pair.client, source, key),
                "secure TCP packet send failed");
        protocol::Packet decoded;
        require(protocol::receiveTcpPacketSecure(pair.peer, decoded, key),
                "secure TCP packet receive failed");
        require(decoded.type == source.type && decoded.sequence == source.sequence &&
                decoded.payload == source.payload, "secure TCP packet changed");
    }
    {
        TcpPair pair;
        protocol::EncryptionKey key{}, wrong{};
        key.fill(0x21); wrong.fill(0x22);
        protocol::Packet source{protocol::Type::MSG, 3, {1}};
        require(protocol::sendTcpPacketSecure(pair.client, source, key),
                "wrong-key fixture send failed");
        protocol::Packet decoded;
        require(!protocol::receiveTcpPacketSecure(pair.peer, decoded, wrong),
                "secure TCP receive accepted wrong key");
    }
}
void testPlainAndSecureUdp() {
    SocketHandle receiver = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    SocketHandle sender = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    require(receiver != INVALID_SOCKET_HANDLE && sender != INVALID_SOCKET_HANDLE,
            "UDP socket creation failed");
    setSocketTimeout(receiver, 1000, 0);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = 0;
    require(::bind(receiver, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) == 0,
            "UDP receiver bind failed");
    SocketLength length = sizeof(destination);
    require(getsockname(receiver, reinterpret_cast<sockaddr*>(&destination), &length) == 0,
            "UDP getsockname failed");

    protocol::Packet source{protocol::Type::HEARTBEAT, 1, {1, 2}};
    require(protocol::sendUdpPacket(sender, source, destination), "plain UDP send failed");
    std::array<std::uint8_t, protocol::MAX_UDP_DATAGRAM + 1> bytes{};
    sockaddr_in from{};
    SocketLength fromLength = sizeof(from);
#ifdef _WIN32
    int n = recvfrom(receiver, reinterpret_cast<char*>(bytes.data()),
        static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
#else
    auto n = recvfrom(receiver, bytes.data(), bytes.size(), 0,
        reinterpret_cast<sockaddr*>(&from), &fromLength);
#endif
    require(n > 0, "plain UDP datagram missing");
    protocol::Packet decoded;
    require(protocol::decodePacket(bytes.data(), static_cast<std::size_t>(n), decoded,
                                   protocol::MAX_UDP_DATAGRAM),
            "bounded UDP decode failed");
    require(decoded.payload == source.payload, "plain UDP payload changed");
    protocol::Packet tcp{protocol::Type::MSG, 2, {}};
    require(!protocol::sendUdpPacket(sender, tcp, destination),
            "TCP type was accepted by UDP sender");

    protocol::EncryptionKey key{};
    key.fill(0xA7);
    source.sequence = 3;
    require(protocol::sendUdpPacketSecure(sender, source, destination, key),
            "secure UDP send failed");
#ifdef _WIN32
    n = recvfrom(receiver, reinterpret_cast<char*>(bytes.data()),
        static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&from), &fromLength);
#else
    n = recvfrom(receiver, bytes.data(), bytes.size(), 0,
        reinterpret_cast<sockaddr*>(&from), &fromLength);
#endif
    require(n > 0 && protocol::decodeSecureDatagram(bytes.data(),
        static_cast<std::size_t>(n), key, decoded), "secure UDP receive/decode failed");
    require(decoded.sequence == 3 && decoded.payload == source.payload,
            "secure UDP payload changed");
    closeSocket(sender);
    closeSocket(receiver);
}
}
int main() {
    try {
        require(initializeSocketRuntime(), "socket runtime initialization failed");
        testIntegerAndStringHelpers();
        testPlainAndSecureTcp();
        testPlainAndSecureUdp();
        cleanupSocketRuntime();
        std::cout << "Transport protocol tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        cleanupSocketRuntime();
        std::cerr << "Transport protocol tests failed: " << e.what() << '\n';
        return 1;
    }
}