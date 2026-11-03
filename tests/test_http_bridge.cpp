#include "test_helpers.h"
#include "../clients/dcap_http_bridge.h"
#include "../platform/socket_compat.h"
#include <array>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
std::uint16_t freePort() {
    SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(s != INVALID_SOCKET_HANDLE, "could not create port-probe socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "could not bind port-probe socket");
    SocketLength length = sizeof(address);
    require(getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) == 0,
            "could not read port-probe socket");
    const auto port = ntohs(address.sin_port);
    closeSocket(s);
    return port;
}
std::string request(std::uint16_t port, const std::string& method,
                    const std::string& path, const std::string& body = "",
                    const std::string& contentType = "application/json",
                    const std::string& origin = "") {
    SocketHandle s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(s != INVALID_SOCKET_HANDLE, "HTTP test socket creation failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    require(::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "HTTP bridge connection failed");
    std::ostringstream wire;
    wire << method << " " << path << " HTTP/1.1\r\n"
         << "Host: 127.0.0.1\r\n"
         << "Content-Length: " << body.size() << "\r\n";
    if (!body.empty()) wire << "Content-Type: " << contentType << "\r\n";
    if (!origin.empty()) wire << "Origin: " << origin << "\r\n";
    wire << "Connection: close\r\n\r\n" << body;
    const auto data = wire.str();
    require(sendAll(s, reinterpret_cast<const std::uint8_t*>(data.data()), data.size()),
            "HTTP request send failed");
#ifdef _WIN32
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
    std::string response;
    std::array<char, 4096> buffer{};
    for (;;) {
#ifdef _WIN32
        const int n = recv(s, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const auto n = recv(s, buffer.data(), buffer.size(), 0);
#endif
        if (n <= 0) break;
        response.append(buffer.data(), static_cast<std::size_t>(n));
    }
    closeSocket(s);
    return response;
}
void testUnauthenticatedAndMalformedRequests() {
    test::TestServer server;
    const auto port = freePort();
    DcapHttpBridge bridge("127.0.0.1", protocol::TCP_PORT, port);
    require(bridge.start(), "HTTP bridge failed to start");

    auto response = request(port, "GET", "/status");
    require(response.find("HTTP/1.1 200 OK") == 0, "GET /status should return 200");
    require(response.find("\"authenticated\":false") != std::string::npos,
            "status should report unauthenticated state");

    response = request(port, "GET", "/");
    require(response.find("HTTP/1.1 200 OK") == 0 &&
            response.find("Distributed Chat") != std::string::npos,
            "GET / should serve the HTML application");
    response = request(port, "GET", "/app.js");
    require(response.find("HTTP/1.1 200 OK") == 0,
            "GET /app.js should serve JavaScript");
    response = request(port, "GET", "/style.css");
    require(response.find("HTTP/1.1 200 OK") == 0,
            "GET /style.css should serve CSS");

    response = request(port, "POST", "/global_message", R"({"content":"hello"})");
    require(response.find("HTTP/1.1 401 Unauthorized") == 0,
            "unauthenticated message send must return 401");

    response = request(port, "POST", "/login", "{");
    require(response.find("HTTP/1.1 400 Bad Request") == 0,
            "malformed login JSON must return 400");

    response = request(port, "POST", "/file_decision", R"({"transfer_id":7})");
    require(response.find("HTTP/1.1 400 Bad Request") == 0,
            "incomplete file decision must return 400");

    response = request(port, "POST", "/file_upload", "not multipart");
    require(response.find("HTTP/1.1 401 Unauthorized") == 0,
            "unauthenticated upload must return 401");

    response = request(port, "GET", "/not-found");
    require(response.find("HTTP/1.1 404 Not Found") == 0,
            "unknown endpoint must return 404");

    response = request(port, "OPTIONS", "/events", "", "", "http://localhost:3000");
    require(response.find("HTTP/1.1 204 No Content") == 0,
            "OPTIONS preflight should return 204");
    require(response.find("Access-Control-Allow-Origin: http://localhost:3000") != std::string::npos,
            "localhost origin should receive CORS permission");

    response = request(port, "OPTIONS", "/events", "", "", "https://evil.example");
    require(response.find("Access-Control-Allow-Origin:") == std::string::npos,
            "untrusted origin must not receive CORS permission");

    bridge.stop();
}
void testAuthenticatedHttpFlow() {
    test::TestServer server;
    const auto port = freePort();
    DcapHttpBridge bridge("127.0.0.1", protocol::TCP_PORT, port);
    require(bridge.start(), "HTTP bridge failed to start");
    DcapClient observer("127.0.0.1");
    test::connectAndLogin(observer, "http-observer");

    auto response = request(port, "POST", "/login", R"({"username":"http-user"})");
    require(response.find("HTTP/1.1 200 OK") == 0, "valid HTTP login should return 200");
    require(response.find("\"authenticated\":true") != std::string::npos,
            "login response must confirm authentication");

    response = request(port, "GET", "/user_list");
    require(response.find("HTTP/1.1 200 OK") == 0, "authenticated user list should return 200");
    require(response.find("http-user") != std::string::npos,
            "HTTP user list omitted logged-in user");

    response = request(port, "POST", "/global_message", R"({"content":"from-http"})");
    require(response.find("HTTP/1.1 202 Accepted") == 0,
            "valid HTTP broadcast should return 202");
    const auto message = test::waitForEvent(observer, "CHAT_ALL");
    require(message.data.value("content", std::string()) == "from-http",
            "HTTP broadcast did not reach another client");

    response = request(port, "POST", "/file_upload", "bad multipart",
                       "multipart/form-data; boundary=broken");
    require(response.find("HTTP/1.1 400 Bad Request") == 0,
            "malformed multipart upload should return 400");

    response = request(port, "POST", "/logout", "{}");
    require(response.find("HTTP/1.1 200 OK") == 0,
            "HTTP logout should return 200");
    response = request(port, "GET", "/status");
    require(response.find("\"authenticated\":false") != std::string::npos,
            "logout did not clear authentication state");

    bridge.stop();
    observer.disconnect();
}
}
int main() {
    try {
        require(initializeSocketRuntime(), "socket runtime init failed");
        testUnauthenticatedAndMalformedRequests();
        testAuthenticatedHttpFlow();
        cleanupSocketRuntime();
        std::cout << "HTTP bridge tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        cleanupSocketRuntime();
        std::cerr << "HTTP bridge tests failed: " << e.what() << '\n';
        return 1;
    }
}