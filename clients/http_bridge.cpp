#include "../clients/http_bridge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <climits>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;

constexpr size_t MAX_HEADER_SIZE = 16 * 1024;
constexpr size_t MAX_REQUEST_BODY_SIZE = 8 * 1024 * 1024;
constexpr size_t MAX_CHAT_CONTENT_SIZE = 4096;
constexpr auto LOGIN_TIMEOUT = std::chrono::seconds(5);

thread_local std::string responseOrigin;

std::string toLower(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );
    return value;
}

bool isAllowedLocalOrigin(const std::string& origin) {
    if (origin == "http://localhost" ||
        origin == "http://127.0.0.1" ||
        origin == "https://localhost" ||
        origin == "https://127.0.0.1") {
        return true;
    }

    const size_t schemeEnd = origin.find("://");
    if (schemeEnd == std::string::npos) {
        return false;
    }

    const std::string scheme = origin.substr(0, schemeEnd);
    if (scheme != "http" && scheme != "https") {
        return false;
    }

    const std::string authority = origin.substr(schemeEnd + 3);
    if (authority.empty() ||
        authority.find_first_of("/?#@") != std::string::npos) {
        return false;
    }

    const size_t colon = authority.find(':');
    const std::string host = authority.substr(0, colon);
    const std::string port = colon == std::string::npos
        ? std::string()
        : authority.substr(colon + 1);

    if (host != "localhost" && host != "127.0.0.1") {
        return false;
    }
    if (colon != std::string::npos &&
        (port.empty() ||
         !std::all_of(port.begin(), port.end(), [](unsigned char ch) {
             return std::isdigit(ch) != 0;
         }))) {
        return false;
    }

    return true;
}

bool sendAll(SOCKET socket, const char* data, size_t length) {
    size_t sentTotal = 0;
    while (sentTotal < length) {
        const size_t remaining = length - sentTotal;
        const int chunkLength = static_cast<int>(
            std::min(remaining, static_cast<size_t>(INT_MAX))
        );
        const int sent = send(
            socket,
            data + sentTotal,
            chunkLength,
            0
        );
        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }
        sentTotal += static_cast<size_t>(sent);
    }
    return true;
}

std::string reasonPhrase(int statusCode) {
    switch (statusCode) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Entity";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Error";
    }
}

std::string jsonResponse(const json& value) {
    return value.dump();
}

void sendJsonError(
    HttpBridge* bridge,
    SOCKET socket,
    int statusCode,
    const std::string& error
) {
    bridge->sendHttpResponse(
        socket,
        statusCode,
        "application/json",
        jsonResponse({{"error", error}})
    );
}

bool parseJsonObject(const std::string& body, json& parsed) {
    try {
        parsed = json::parse(body);
        return parsed.is_object();
    } catch (const json::exception&) {
        return false;
    }
}

bool readHttpHeader(
    const std::string& request,
    std::string& header,
    std::string& body,
    size_t& contentLength
) {
    const size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos || headerEnd > MAX_HEADER_SIZE) {
        return false;
    }

    header = request.substr(0, headerEnd);
    body = request.substr(headerEnd + 4);
    contentLength = 0;
    bool hasContentLength = false;
    std::istringstream lines(header);
    std::string line;
    std::getline(lines, line);

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return false;
        }

        const std::string name = toLower(line.substr(0, colon));
        size_t valueStart = colon + 1;
        while (valueStart < line.size() &&
               std::isspace(static_cast<unsigned char>(line[valueStart]))) {
            ++valueStart;
        }
        const std::string value = line.substr(valueStart);

        if (name == "content-length") {
            if (hasContentLength || value.empty()) {
                return false;
            }

            try {
                size_t parsedLength = 0;
                const unsigned long long parsed =
                    std::stoull(value, &parsedLength);
                if (parsedLength != value.size() ||
                    parsed > MAX_REQUEST_BODY_SIZE) {
                    return false;
                }
                contentLength = static_cast<size_t>(parsed);
                hasContentLength = true;
            } catch (const std::exception&) {
                return false;
            }
        } else if (name == "transfer-encoding" &&
                   toLower(value) != "identity") {
            return false;
        } else if (name == "origin") {
            responseOrigin = value;
        }
    }

    return body.size() <= contentLength;
}

std::string getQueryValue(
    const std::string& target,
    const std::string& key
) {
    const size_t queryStart = target.find('?');
    if (queryStart == std::string::npos) {
        return {};
    }

    const std::string query = target.substr(queryStart + 1);
    size_t entryStart = 0;
    while (entryStart <= query.size()) {
        const size_t entryEnd = query.find('&', entryStart);
        const std::string entry = query.substr(
            entryStart,
            entryEnd == std::string::npos
                ? std::string::npos
                : entryEnd - entryStart
        );
        const size_t equals = entry.find('=');
        if (equals != std::string::npos &&
            entry.substr(0, equals) == key) {
            return entry.substr(equals + 1);
        }
        if (entryEnd == std::string::npos) {
            break;
        }
        entryStart = entryEnd + 1;
    }
    return {};
}

} // namespace

HttpBridge::HttpBridge(
        const string& serverAddress,
        uint16_t serverPort,
        uint16_t httpPort
    )
    : listeningSocket(INVALID_SOCKET),
      httpPort(httpPort),
      running(false),
      chatClient(serverAddress, serverPort),
      serverAddress(serverAddress),
      serverPort(serverPort),
      sessionAuthenticated(false),
      winsockInitialized(false) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("Failed to initialize Winsock");
    }
    winsockInitialized = true;
}

HttpBridge::~HttpBridge() {
    stop();
    if (winsockInitialized) {
        WSACleanup();
    }
}

bool HttpBridge::start() {
    if (running.load()) {
        return true;
    }

    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        cerr << "Failed to create listening socket" << endl;
        return false;
    }
    listeningSocket.store(socket);

    sockaddr_in localAddress{};
    localAddress.sin_family = AF_INET;
    localAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    localAddress.sin_port = htons(httpPort);

    if (bind(
            socket,
            reinterpret_cast<sockaddr*>(&localAddress),
            sizeof(localAddress)
        ) == SOCKET_ERROR) {
        cerr << "Failed to bind listening socket to loopback" << endl;
        closesocket(socket);
        listeningSocket.store(INVALID_SOCKET);
        return false;
    }

    if (listen(socket, SOMAXCONN) == SOCKET_ERROR) {
        cerr << "Failed to listen on socket" << endl;
        closesocket(socket);
        listeningSocket.store(INVALID_SOCKET);
        return false;
    }

    running = true;
    acceptThread = thread(&HttpBridge::acceptHttpConnections, this);
    return true;
}

void HttpBridge::acceptHttpConnections() {
    while (running.load()) {
        const SOCKET socket = listeningSocket.load();
        if (socket == INVALID_SOCKET) {
            break;
        }

        SOCKET clientSocket = accept(socket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET) {
            if (running.load()) {
                cerr << "Failed to accept HTTP connection" << endl;
            }
            continue;
        }

        {
            lock_guard<mutex> lock(workersMutex);
            httpWorkerThreads.emplace_back(
                &HttpBridge::handleHttpConnection,
                this,
                clientSocket
            );
        }
    }
}

void HttpBridge::handleHttpConnection(SOCKET clientSocket) {
    DWORD timeoutMs = 5000;
    if (setsockopt(
        clientSocket,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeoutMs),
        sizeof(timeoutMs)
    ) == SOCKET_ERROR) {
        cerr << "Failed to configure HTTP receive timeout: "
             << WSAGetLastError() << endl;
        sendJsonError(this, clientSocket, 500, "Could not configure HTTP connection");
        shutdown(clientSocket, SD_BOTH);
        closesocket(clientSocket);
        return;
    }

    std::string request;
    std::array<char, 4096> buffer{};
    size_t expectedRequestSize = 0;

    while (request.size() <= MAX_HEADER_SIZE + MAX_REQUEST_BODY_SIZE) {
        const int received = recv(
            clientSocket,
            buffer.data(),
            static_cast<int>(buffer.size()),
            0
        );
        if (received == SOCKET_ERROR || received == 0) {
            break;
        }
        request.append(buffer.data(), static_cast<size_t>(received));

        const size_t headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            if (request.size() > MAX_HEADER_SIZE) {
                responseOrigin.clear();
                sendJsonError(this, clientSocket, 413, "HTTP headers are too large");
                closesocket(clientSocket);
                return;
            }
            continue;
        }

        std::string header;
        std::string body;
        size_t contentLength = 0;
        if (!readHttpHeader(request, header, body, contentLength)) {
            responseOrigin.clear();
            sendJsonError(this, clientSocket, 400, "Invalid HTTP headers or body size");
            closesocket(clientSocket);
            return;
        }

        expectedRequestSize = headerEnd + 4 + contentLength;
        if (request.size() >= expectedRequestSize) {
            request.resize(expectedRequestSize);
            break;
        }
    }

    if (expectedRequestSize == 0 || request.size() < expectedRequestSize) {
        responseOrigin.clear();
        sendJsonError(this, clientSocket, 400, "Incomplete HTTP request");
    } else {
        handleHttpRequest(clientSocket, request);
    }

    responseOrigin.clear();
    shutdown(clientSocket, SD_BOTH);
    closesocket(clientSocket);
}

void HttpBridge::handleHttpRequest(
        SOCKET clientSocket,
        const string& request
    ) {
    const size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd == string::npos) {
        sendJsonError(this, clientSocket, 400, "Malformed HTTP request");
        return;
    }

    std::string header;
    std::string body;
    size_t contentLength = 0;
    if (!readHttpHeader(request, header, body, contentLength) ||
        body.size() != contentLength) {
        sendJsonError(this, clientSocket, 400, "Malformed HTTP request body");
        return;
    }

    std::istringstream firstLine(header);
    std::string method;
    std::string target;
    std::string httpVersion;
    firstLine >> method >> target >> httpVersion;
    if (method.empty() || target.empty() ||
        (httpVersion != "HTTP/1.0" && httpVersion != "HTTP/1.1")) {
        sendJsonError(this, clientSocket, 400, "Invalid request line");
        return;
    }

    if (!responseOrigin.empty() && !isAllowedLocalOrigin(responseOrigin)) {
        sendJsonError(this, clientSocket, 403, "Origin is not allowed");
        return;
    }

    if (method == "OPTIONS") {
        sendHttpResponse(clientSocket, 204, "application/json", "");
        return;
    }

    const size_t queryStart = target.find('?');
    const std::string path = target.substr(0, queryStart);

    if (method == "POST" && path == "/login") {
        handleLogin(clientSocket, body);
    } else if (method == "POST" && path == "/logout") {
        handleLogout(clientSocket);
    } else if (method == "POST" && path == "/global_message") {
        handleGlobalMessage(clientSocket, body);
    } else if (method == "POST" && path == "/private_message") {
        handlePrivateMessage(clientSocket, body);
    } else if (method == "GET" && path == "/user_list") {
        handleUserListRequest(clientSocket);
    } else if (method == "GET" && path == "/events") {
        handleEventsRequest(clientSocket);
    } else if (method == "GET" && path == "/status") {
        handleStatusRequest(clientSocket);
    } else if (method == "POST" && path == "/file_upload") {
        handleFileUpload(clientSocket, body);
    } else if (method == "GET" && path == "/file_download") {
        handleFileDownload(
            clientSocket,
            getQueryValue(target, "transfer_id")
        );
    } else {
        sendJsonError(this, clientSocket, 404, "Endpoint not found");
    }
}

void HttpBridge::handleLogin(
        SOCKET clientSocket,
        const string& requestBody
    ) {
    json payload;
    if (!parseJsonObject(requestBody, payload) ||
        !payload.contains("username") ||
        !payload.contains("password") ||
        !payload["username"].is_string() ||
        !payload["password"].is_string()) {
        sendJsonError(this, clientSocket, 400, "Username and password are required");
        return;
    }

    const std::string requestedUsername =
        payload["username"].get<std::string>();
    const std::string password =
        payload["password"].get<std::string>();
    if (requestedUsername.empty() || requestedUsername.size() > 64 ||
        password.empty() || password.size() > 256) {
        sendJsonError(this, clientSocket, 400, "Username or password is invalid");
        return;
    }

    std::unique_lock<std::mutex> sessionLock(sessionMutex);
    if (sessionAuthenticated && chatClient.isAuthenticated()) {
        sendJsonError(this, clientSocket, 409, "A browser session is already authenticated");
        return;
    }

    if (!chatClient.isConnected() && !chatClient.connect()) {
        lastConnectionError = "Could not connect to the chat server";
        sendJsonError(this, clientSocket, 503, lastConnectionError);
        return;
    }

    if (!chatClient.login(requestedUsername, password)) {
        lastConnectionError = "Could not send login request";
        sendJsonError(this, clientSocket, 503, lastConnectionError);
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + LOGIN_TIMEOUT;
    while (std::chrono::steady_clock::now() < deadline) {
        auto event = chatClient.getNextEvent();
        if (!event.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (event->type == "LOGIN_OK") {
            sessionAuthenticated = true;
            sessionUsername = requestedUsername;
            lastConnectionError.clear();
            sendHttpResponse(
                clientSocket,
                200,
                "application/json",
                jsonResponse({
                    {"authenticated", true},
                    {"username", requestedUsername},
                    {"result", event->data}
                })
            );
            return;
        }

        if (event->type == "LOGIN_FAIL") {
            lastConnectionError = "Chat server rejected the credentials";
            sendJsonError(this, clientSocket, 401, lastConnectionError);
            return;
        }

        if (event->type == "USER_LIST" &&
            event->data.contains("users") &&
            event->data["users"].is_array()) {
            latestUserList = event->data["users"];
        }

        pendingEvents.push_back(std::move(*event));
    }

    lastConnectionError = "Timed out waiting for the chat server login response";
    chatClient.disconnect();
    while (chatClient.getNextEvent().has_value()) {
    }
    pendingEvents.clear();
    sessionAuthenticated = false;
    sessionUsername.clear();
    sendJsonError(this, clientSocket, 504, lastConnectionError);
}

void HttpBridge::handleLogout(SOCKET clientSocket) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    chatClient.logout();
    sessionAuthenticated = false;
    sessionUsername.clear();
    latestUserList = nlohmann::json::array();
    pendingEvents.clear();
    sendHttpResponse(
        clientSocket,
        200,
        "application/json",
        jsonResponse({{"authenticated", false}})
    );
}

void HttpBridge::handleGlobalMessage(
        SOCKET clientSocket,
        const string& requestBody
    ) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (!sessionAuthenticated || !chatClient.isAuthenticated()) {
        sendJsonError(this, clientSocket, 401, "Authentication required");
        return;
    }

    json payload;
    if (!parseJsonObject(requestBody, payload) ||
        !payload.contains("content") ||
        !payload["content"].is_string()) {
        sendJsonError(this, clientSocket, 400, "Message content is required");
        return;
    }

    const std::string content = payload["content"].get<std::string>();
    if (content.empty() || content.size() > MAX_CHAT_CONTENT_SIZE) {
        sendJsonError(this, clientSocket, 422, "Message content length is invalid");
        return;
    }

    if (!chatClient.sendGlobalMessage(content)) {
        lastConnectionError = "Could not send global message to the chat server";
        sendJsonError(this, clientSocket, 503, lastConnectionError);
        return;
    }

    sendHttpResponse(
        clientSocket,
        202,
        "application/json",
        jsonResponse({{"status", "accepted"}})
    );
}

void HttpBridge::handlePrivateMessage(
        SOCKET clientSocket,
        const string& requestBody
    ) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (!sessionAuthenticated || !chatClient.isAuthenticated()) {
        sendJsonError(this, clientSocket, 401, "Authentication required");
        return;
    }

    json payload;
    if (!parseJsonObject(requestBody, payload) ||
        !payload.contains("recipient") ||
        !payload.contains("content") ||
        !payload["recipient"].is_string() ||
        !payload["content"].is_string()) {
        sendJsonError(this, clientSocket, 400, "Recipient and content are required");
        return;
    }

    const std::string recipient = payload["recipient"].get<std::string>();
    const std::string content = payload["content"].get<std::string>();
    if (recipient.empty() || recipient.size() > 64 ||
        content.empty() || content.size() > MAX_CHAT_CONTENT_SIZE) {
        sendJsonError(this, clientSocket, 422, "Recipient or content is invalid");
        return;
    }

    if (!chatClient.sendPrivateMessage(recipient, content)) {
        lastConnectionError = "Could not send private message to the chat server";
        sendJsonError(this, clientSocket, 503, lastConnectionError);
        return;
    }

    sendHttpResponse(
        clientSocket,
        202,
        "application/json",
        jsonResponse({{"status", "accepted"}})
    );
}

void HttpBridge::handleUserListRequest(SOCKET clientSocket) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (!sessionAuthenticated || !chatClient.isAuthenticated()) {
        sendJsonError(this, clientSocket, 401, "Authentication required");
        return;
    }

    latestUserList = chatClient.getLatestUserList();
    while (auto event = chatClient.getNextEvent()) {
        if (event->type == "USER_LIST" &&
            event->data.contains("users") &&
            event->data["users"].is_array()) {
            latestUserList = event->data["users"];
        } else {
            pendingEvents.push_back(std::move(*event));
        }
    }

    sendHttpResponse(
        clientSocket,
        200,
        "application/json",
        jsonResponse({{"users", latestUserList}})
    );
}

void HttpBridge::handleEventsRequest(SOCKET clientSocket) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (!sessionAuthenticated || !chatClient.isAuthenticated()) {
        sendJsonError(this, clientSocket, 401, "Authentication required");
        return;
    }

    json events = json::array();
    while (!pendingEvents.empty()) {
        ClientEvent event = std::move(pendingEvents.front());
        pendingEvents.pop_front();
        if (event.type == "USER_LIST" &&
            event.data.contains("users") &&
            event.data["users"].is_array()) {
            latestUserList = event.data["users"];
        }
        events.push_back({
            {"type", event.type},
            {"data", event.data}
        });
    }

    while (auto event = chatClient.getNextEvent()) {
        if (event->type == "USER_LIST" &&
            event->data.contains("users") &&
            event->data["users"].is_array()) {
            latestUserList = event->data["users"];
        }
        events.push_back({
            {"type", event->type},
            {"data", event->data}
        });
    }

    sendHttpResponse(
        clientSocket,
        200,
        "application/json",
        jsonResponse({{"events", events}})
    );
}

void HttpBridge::handleStatusRequest(SOCKET clientSocket) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    const bool connected = chatClient.isConnected();
    const bool authenticated =
        sessionAuthenticated && chatClient.isAuthenticated() && connected;
    if (!connected && lastConnectionError.empty()) {
        lastConnectionError = "TCP connection to the chat server was lost";
    }

    sendHttpResponse(
        clientSocket,
        200,
        "application/json",
        jsonResponse({
            {"connected", connected},
            {"authenticated", authenticated},
            {"username", authenticated ? sessionUsername : ""},
            {"server_address", serverAddress},
            {"server_port", serverPort},
            {"last_connection_error", lastConnectionError}
        })
    );
}

void HttpBridge::handleFileUpload(
        SOCKET clientSocket,
        const string&
    ) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (!sessionAuthenticated || !chatClient.isAuthenticated()) {
        sendJsonError(this, clientSocket, 401, "Authentication required");
        return;
    }

    sendJsonError(
        this,
        clientSocket,
        501,
        "File upload is unavailable until UDP transfer is integrated"
    );
}

void HttpBridge::handleFileDownload(
        SOCKET clientSocket,
        const string&
    ) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    if (!sessionAuthenticated || !chatClient.isAuthenticated()) {
        sendJsonError(this, clientSocket, 401, "Authentication required");
        return;
    }

    sendJsonError(
        this,
        clientSocket,
        501,
        "File download is unavailable until UDP transfer is integrated"
    );
}

void HttpBridge::sendHttpResponse(
        SOCKET clientSocket,
        int statusCode,
        const string& contentType,
        const string& body
    ) {
    std::ostringstream response;
    response << "HTTP/1.1 " << statusCode << ' '
             << reasonPhrase(statusCode) << "\r\n";
    response << "Content-Type: " << contentType << "\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    response << "Connection: close\r\n";
    response << "X-Content-Type-Options: nosniff\r\n";
    response << "Cache-Control: no-store\r\n";
    if (!responseOrigin.empty() && isAllowedLocalOrigin(responseOrigin)) {
        response << "Access-Control-Allow-Origin: "
                 << responseOrigin << "\r\n";
        response << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
        response << "Access-Control-Allow-Headers: Content-Type\r\n";
        response << "Vary: Origin\r\n";
    }
    response << "\r\n";

    const std::string headers = response.str();
    if (!sendAll(clientSocket, headers.data(), headers.size())) {
        cerr << "Failed to send HTTP response headers: "
             << WSAGetLastError() << endl;
        return;
    }
    if (!body.empty()) {
        if (!sendAll(clientSocket, body.data(), body.size())) {
            cerr << "Failed to send HTTP response body: "
                 << WSAGetLastError() << endl;
        }
    }
}

void HttpBridge::stop() {
    running = false;
    const SOCKET socket = listeningSocket.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }

    if (acceptThread.joinable()) {
        acceptThread.join();
    }

    {
        lock_guard<mutex> lock(workersMutex);
        for (thread& worker : httpWorkerThreads) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        httpWorkerThreads.clear();
    }

    std::lock_guard<std::mutex> sessionLock(sessionMutex);
    sessionAuthenticated = false;
    chatClient.disconnect();
}
