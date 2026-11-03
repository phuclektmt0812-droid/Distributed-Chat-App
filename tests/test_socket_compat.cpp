#include "../platform/socket_compat.h"
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void testLoopbackStream() {
    require(initializeSocketRuntime(), "socket runtime initialization failed");
    SocketHandle listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET_HANDLE, "listener socket creation failed");
    setReuseAddress(listener);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "loopback bind failed");
    require(::listen(listener, 1) == 0, "listen failed");
    SocketLength length = sizeof(address);
    require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "getsockname failed");

    SocketHandle client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(client != INVALID_SOCKET_HANDLE, "client socket creation failed");
    require(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "loopback connect failed");
    SocketHandle peer = ::accept(listener, nullptr, nullptr);
    require(peer != INVALID_SOCKET_HANDLE, "accept failed");

    require(setSocketTimeout(peer, 1000, 1000), "socket timeout configuration failed");
    require(setSocketKeepAlive(peer), "keepalive configuration failed");
    const std::array<std::uint8_t, 7> sent{{0, 1, 2, 127, 128, 254, 255}};
    require(sendAll(client, sent.data(), sent.size()), "sendAll failed");
    std::array<std::uint8_t, 7> received{};
    require(receiveExact(peer, received.data(), received.size()), "receiveExact failed");
    require(received == sent, "stream bytes changed");
    require(sendAll(client, nullptr, 0), "zero-byte sendAll should succeed");
    require(receiveExact(peer, nullptr, 0), "zero-byte receiveExact should succeed");

    shutdownSocket(client);
    closeSocket(client);
    std::uint8_t one = 0;
    require(!receiveExact(peer, &one, 1), "receiveExact must fail after peer closes");
    closeSocket(peer);
    closeSocket(listener);
    cleanupSocketRuntime();
}
}
int main() {
    try {
        testLoopbackStream();
        std::cout << "Socket compatibility tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Socket compatibility tests failed: " << e.what() << '\n';
        return 1;
    }
}