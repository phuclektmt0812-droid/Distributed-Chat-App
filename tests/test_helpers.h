#pragma once

#include "../clients/client.h"
#include "../server/server.h"

#include <ws2tcpip.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

namespace test {

inline void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline std::uint16_t findAvailablePort() {
    WSADATA data{};
    require(WSAStartup(MAKEWORD(2, 2), &data) == 0,
        "WSAStartup failed while finding an available port");

    SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe == INVALID_SOCKET) {
        WSACleanup();
        throw std::runtime_error("Could not create port-probe socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    int addressLength = sizeof(address);
    const bool bound =
        bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != SOCKET_ERROR &&
        getsockname(
            probe,
            reinterpret_cast<sockaddr*>(&address),
            &addressLength
        ) != SOCKET_ERROR;
    const std::uint16_t port = bound ? ntohs(address.sin_port) : 0;
    closesocket(probe);
    WSACleanup();

    require(port != 0, "Could not reserve an available TCP port");
    return port;
}

class TestServer {
public:
    TestServer()
        : port_(findAvailablePort()),
          server_(port_),
          listener_([this]() {
              server_.startListening();
          }) {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);

        while (std::chrono::steady_clock::now() < deadline) {
            SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (probe != INVALID_SOCKET) {
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                address.sin_port = htons(port_);

                const bool connected = connect(
                    probe,
                    reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)
                ) != SOCKET_ERROR;
                closesocket(probe);
                if (connected) {
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        stop();
        throw std::runtime_error("Test server did not start listening");
    }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    ~TestServer() {
        stop();
    }

    std::uint16_t port() const {
        return port_;
    }

private:
    void stop() {
        server_.stop();
        if (listener_.joinable()) {
            listener_.join();
        }
    }

    std::uint16_t port_;
    Server server_;
    std::thread listener_;
};

inline ClientEvent waitForEvent(
    ChatClient& client,
    const std::string& expectedType,
    std::chrono::milliseconds timeout = std::chrono::seconds(3)
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto event = client.getNextEvent();
        if (event.has_value() && event->type == expectedType) {
            return *event;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    throw std::runtime_error("Timed out waiting for event: " + expectedType);
}

inline void login(ChatClient& client, const std::string& username) {
    require(client.connect(), "Could not connect " + username);
    require(client.login(username, "test-password"),
        "Could not send login for " + username);
    const ClientEvent event = waitForEvent(client, "LOGIN_OK");
    require(event.data.value("username", std::string()) == username,
        "Login response returned an unexpected username");
}

} // namespace test
