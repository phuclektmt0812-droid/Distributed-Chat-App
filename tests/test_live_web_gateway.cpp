#include "test_helpers.h"
#include "../gateway/live_web_gateway.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using nlohmann::json;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint16_t availableTcpPort() {
    SocketHandle probe = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(probe != INVALID_SOCKET_HANDLE, "Could not create gateway port probe");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    require(::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "Could not bind gateway port probe");
    SocketLength length = sizeof(address);
    require(::getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "Could not read gateway port probe");
    const auto port = ntohs(address.sin_port);
    closeSocket(probe);
    require(port != 0, "Operating system returned an invalid gateway port");
    return port;
}

std::string httpRequest(std::uint16_t port, const std::string& method,
                        const std::string& path, const std::string& body = {},
                        const std::string& cookie = {}) {
    SocketHandle socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(socket != INVALID_SOCKET_HANDLE, "Could not create HTTP test socket");
    setSocketTimeout(socket, 5000, 5000);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(socket);
        throw std::runtime_error("Could not connect to Live Web Gateway");
    }

    const auto authority = "127.0.0.1:" + std::to_string(port);
    std::string request = method + " " + path + " HTTP/1.1\r\n";
    request += "Host: " + authority + "\r\n";
    request += "Connection: close\r\n";
    if (method == "POST") {
        request += "Origin: http://" + authority + "\r\n";
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    if (!cookie.empty()) request += "Cookie: " + cookie + "\r\n";
    request += "\r\n";
    request += body;

    std::size_t sent = 0;
    while (sent < request.size()) {
        const auto remaining = request.size() - sent;
        const int amount = static_cast<int>(std::min<std::size_t>(remaining, 1024 * 1024));
#ifdef _WIN32
        const int count = ::send(socket, request.data() + sent, amount, 0);
#else
        const auto count = ::send(socket, request.data() + sent,
                                  static_cast<std::size_t>(amount), 0);
#endif
        if (count <= 0) {
            closeSocket(socket);
            throw std::runtime_error("Could not send HTTP test request");
        }
        sent += static_cast<std::size_t>(count);
    }

    std::string response;
    char buffer[4096];
    for (;;) {
#ifdef _WIN32
        const int count = ::recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
        const auto count = ::recv(socket, buffer, sizeof(buffer), 0);
#endif
        if (count == 0) break;
        if (count < 0) {
            closeSocket(socket);
            throw std::runtime_error("Could not receive HTTP test response");
        }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    closeSocket(socket);
    return response;
}

std::string responseBody(const std::string& response) {
    const auto separator = response.find("\r\n\r\n");
    require(separator != std::string::npos, "HTTP response has no header/body separator");
    return response.substr(separator + 4);
}

std::string loginAndGetCookie(std::uint16_t port, const std::string& username) {
    const auto response = httpRequest(
        port, "POST", "/login", json{{"username", username}}.dump());
    require(response.rfind("HTTP/1.1 200", 0) == 0,
            "Gateway login failed for " + username + ": " + response);

    const auto cookiePosition = response.find("Set-Cookie: dcap_session=");
    require(cookiePosition != std::string::npos, "Gateway did not issue a session cookie");
    const auto valueStart = cookiePosition + std::string("Set-Cookie: ").size();
    const auto valueEnd = response.find(';', valueStart);
    require(valueEnd != std::string::npos, "Gateway session cookie is malformed");
    return response.substr(valueStart, valueEnd - valueStart);
}

bool responseHasPrivateMessage(const std::string& response,
                               const std::string& expectedText,
                               const std::string& expectedSender,
                               const std::string& expectedRecipient) {
    const auto body = responseBody(response);
    const auto payload = json::parse(body);
    if (!payload.contains("events") || !payload["events"].is_array()) return false;
    for (const auto& event : payload["events"]) {
        if (event.value("type", std::string()) != "CHAT_PRIVATE") continue;
        const auto data = event.value("data", json::object());
        if (data.value("content", std::string()) == expectedText &&
            data.value("sender", std::string()) == expectedSender &&
            data.value("recipient", std::string()) == expectedRecipient) {
            return true;
        }
    }
    return false;
}

void testTwoIndependentBrowserSessions() {
    test::TestServer server;
    const auto gatewayPort = availableTcpPort();
    LiveWebGateway gateway("127.0.0.1", 54000, gatewayPort, "web");
    require(gateway.start(), "Live Web Gateway failed to start");

    const auto aliceCookie = loginAndGetCookie(gatewayPort, "gateway-alice");
    const auto bobCookie = loginAndGetCookie(gatewayPort, "gateway-bob");
    require(aliceCookie != bobCookie,
            "Two browser logins unexpectedly received the same session cookie");

    const auto aliceStatus = json::parse(responseBody(
        httpRequest(gatewayPort, "GET", "/status", {}, aliceCookie)));
    const auto bobStatus = json::parse(responseBody(
        httpRequest(gatewayPort, "GET", "/status", {}, bobCookie)));
    require(aliceStatus.value("authenticated", false) &&
            aliceStatus.value("username", std::string()) == "gateway-alice",
            "Alice's browser cookie did not resolve to Alice's DCAP session");
    require(bobStatus.value("authenticated", false) &&
            bobStatus.value("username", std::string()) == "gateway-bob",
            "Bob's browser cookie did not resolve to Bob's DCAP session");

    const auto sendResponse = httpRequest(
        gatewayPort, "POST", "/private_message",
        json{{"recipient", "gateway-bob"}, {"content", "hello from another device"}}.dump(),
        aliceCookie);
    require(sendResponse.rfind("HTTP/1.1 202", 0) == 0,
            "Gateway did not accept Alice's private message");

    bool aliceReceived = false;
    bool bobReceived = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && (!aliceReceived || !bobReceived)) {
        if (!aliceReceived) {
            const auto events = httpRequest(gatewayPort, "GET", "/events", {}, aliceCookie);
            aliceReceived = responseHasPrivateMessage(
                events, "hello from another device", "gateway-alice", "gateway-bob");
        }
        if (!bobReceived) {
            const auto events = httpRequest(gatewayPort, "GET", "/events", {}, bobCookie);
            bobReceived = responseHasPrivateMessage(
                events, "hello from another device", "gateway-alice", "gateway-bob");
        }
        if (!aliceReceived || !bobReceived)
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    require(aliceReceived, "Sender's separate browser session did not receive its private message");
    require(bobReceived, "Recipient's separate browser session did not receive its private message");

    gateway.stop();
}
} // namespace

int main() {
    try {
        testTwoIndependentBrowserSessions();
        std::cout << "Live Web Gateway multi-session tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Live Web Gateway tests failed: " << error.what() << '\n';
        return 1;
    }
}
