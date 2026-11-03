#include "live_web_gateway.h"

#include "../protocol/protocol.h"

#include <openssl/rand.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/time.h>
#endif

using nlohmann::json;

namespace {
constexpr std::size_t MAX_HEADER_BYTES = 16 * 1024;
constexpr std::size_t MAX_HTTP_BODY_BYTES = 110ULL * 1024ULL * 1024ULL;
constexpr std::size_t MAX_ACTIVE_SESSIONS = 64;
constexpr std::uint64_t SESSION_IDLE_TIMEOUT_MS = 12ULL * 60ULL * 60ULL * 1000ULL;

struct HttpRequest {
    std::string method;
    std::string target;
    std::string body;
    std::map<std::string, std::string> headers;

    std::string header(const std::string& name) const {
        const auto found = headers.find(name);
        return found == headers.end() ? std::string() : found->second;
    }
};

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool sendAll(SocketHandle socket, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto remaining = data.size() - offset;
        const int amount = static_cast<int>(std::min<std::size_t>(remaining, INT_MAX));
#ifdef _WIN32
        const int sent = send(socket, data.data() + offset, amount, 0);
#else
        const auto sent = send(socket, data.data() + offset,
                               static_cast<std::size_t>(amount), MSG_NOSIGNAL);
#endif
        if (sent <= 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

const char* reasonPhrase(int status) {
    switch (status) {
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
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 431: return "Request Header Fields Too Large";
        default: return "Error";
    }
}

int readRequest(SocketHandle socket, HttpRequest& request, std::string& error) {
    std::string raw;
    std::vector<char> buffer(8192);
    std::size_t headerEnd = std::string::npos;
    std::size_t contentLength = 0;

    while (headerEnd == std::string::npos) {
#ifdef _WIN32
        const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const auto received = recv(socket, buffer.data(), buffer.size(), 0);
#endif
        if (received <= 0) {
            error = "Could not read HTTP request headers";
            return 400;
        }
        raw.append(buffer.data(), static_cast<std::size_t>(received));
        if (raw.size() > MAX_HEADER_BYTES && raw.find("\r\n\r\n") == std::string::npos) {
            error = "HTTP request headers are too large";
            return 431;
        }
        headerEnd = raw.find("\r\n\r\n");
    }

    std::istringstream stream(raw.substr(0, headerEnd));
    std::string version;
    if (!(stream >> request.method >> request.target >> version) ||
        (version != "HTTP/1.1" && version != "HTTP/1.0")) {
        error = "Malformed HTTP request line";
        return 400;
    }

    std::string line;
    std::getline(stream, line);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto name = lower(line.substr(0, colon));
        auto value = line.substr(colon + 1);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());
        request.headers[name] = value;
    }

    const auto transferEncoding = lower(request.header("transfer-encoding"));
    if (!transferEncoding.empty() && transferEncoding != "identity") {
        error = "Chunked request bodies are not supported";
        return 400;
    }

    if (!request.header("content-length").empty()) {
        try {
            const auto parsed = std::stoull(request.header("content-length"));
            if (parsed > MAX_HTTP_BODY_BYTES) {
                error = "HTTP request body exceeds the configured limit";
                return 413;
            }
            contentLength = static_cast<std::size_t>(parsed);
        } catch (...) {
            error = "Invalid Content-Length";
            return 400;
        }
    }

    const std::size_t bodyStart = headerEnd + 4;
    if (raw.size() > bodyStart)
        request.body.assign(raw.data() + bodyStart,
                            std::min(contentLength, raw.size() - bodyStart));

    while (request.body.size() < contentLength) {
#ifdef _WIN32
        const int received = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const auto received = recv(socket, buffer.data(), buffer.size(), 0);
#endif
        if (received <= 0) {
            error = "Incomplete HTTP request body";
            return 400;
        }
        const auto remaining = contentLength - request.body.size();
        request.body.append(buffer.data(),
            std::min<std::size_t>(remaining, static_cast<std::size_t>(received)));
    }
    return 200;
}

bool parseJsonObject(const std::string& body, json& value) {
    try {
        value = json::parse(body);
        return value.is_object();
    } catch (...) {
        return false;
    }
}

std::string safeFilename(std::string name) {
    const auto separator = name.find_last_of("/\\");
    if (separator != std::string::npos) name = name.substr(separator + 1);
    if (name.empty() || name == "." || name == "..") return "upload.bin";
    for (char& c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 32 || c == ':' || c == '/' || c == '\\') c = '_';
    }
    if (name.size() > 255) name.resize(255);
    return name;
}

bool parseMultipart(const std::string& body, const std::string& contentType,
                    std::string& recipient, std::string& filename, std::string& bytes) {
    const auto boundaryPosition = lower(contentType).find("boundary=");
    if (boundaryPosition == std::string::npos) return false;

    std::string boundary = contentType.substr(boundaryPosition + 9);
    if (!boundary.empty() && boundary.front() == '"') {
        const auto end = boundary.find('"', 1);
        if (end == std::string::npos) return false;
        boundary = boundary.substr(1, end - 1);
    } else {
        const auto semicolon = boundary.find(';');
        if (semicolon != std::string::npos) boundary.resize(semicolon);
    }
    if (boundary.empty() || boundary.size() > 200) return false;

    const std::string marker = "--" + boundary;
    std::size_t cursor = 0;
    while ((cursor = body.find(marker, cursor)) != std::string::npos) {
        cursor += marker.size();
        if (body.compare(cursor, 2, "--") == 0) break;
        if (body.compare(cursor, 2, "\r\n") != 0) return false;
        cursor += 2;

        const auto headerEnd = body.find("\r\n\r\n", cursor);
        if (headerEnd == std::string::npos) return false;
        const std::string partHeaders = body.substr(cursor, headerEnd - cursor);
        const auto dataStart = headerEnd + 4;
        const auto dataEnd = body.find("\r\n" + marker, dataStart);
        if (dataEnd == std::string::npos) return false;

        std::string field;
        std::istringstream headers(partHeaders);
        std::string headerLine;
        while (std::getline(headers, headerLine)) {
            if (!headerLine.empty() && headerLine.back() == '\r') headerLine.pop_back();
            const auto lowered = lower(headerLine);
            if (lowered.rfind("content-disposition:", 0) != 0) continue;
            const auto namePosition = headerLine.find("name=\"");
            if (namePosition != std::string::npos) {
                const auto end = headerLine.find('"', namePosition + 6);
                if (end != std::string::npos)
                    field = headerLine.substr(namePosition + 6, end - namePosition - 6);
            }
            const auto filePosition = headerLine.find("filename=\"");
            if (filePosition != std::string::npos) {
                const auto end = headerLine.find('"', filePosition + 10);
                if (end != std::string::npos)
                    filename = safeFilename(headerLine.substr(filePosition + 10,
                                                               end - filePosition - 10));
            }
        }

        const auto value = body.substr(dataStart, dataEnd - dataStart);
        if (field == "recipient") recipient = value;
        if (field == "file" && !filename.empty()) bytes = value;
        cursor = dataEnd + 2;
    }
    return !recipient.empty() && !filename.empty();
}

std::string randomToken() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error("Secure session token generation failed");
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : bytes)
        output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

std::string cookieValue(const std::string& cookies, const std::string& name) {
    std::istringstream stream(cookies);
    std::string item;
    while (std::getline(stream, item, ';')) {
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        const auto equals = item.find('=');
        if (equals != std::string::npos && item.substr(0, equals) == name)
            return item.substr(equals + 1);
    }
    return {};
}

bool sameOrigin(const HttpRequest& request) {
    const auto origin = request.header("origin");
    if (origin.empty()) return true; // Non-browser clients may omit Origin.
    const auto scheme = origin.find("://");
    if (scheme == std::string::npos) return false;
    const auto authorityStart = scheme + 3;
    const auto authorityEnd = origin.find('/', authorityStart);
    const auto authority = origin.substr(authorityStart,
        authorityEnd == std::string::npos ? std::string::npos : authorityEnd - authorityStart);
    const auto host = request.header("host");
    return !host.empty() && lower(authority) == lower(host);
}

std::string jsonText(const json& value) {
    return value.dump();
}
} // namespace

struct LiveWebGateway::Session {
    explicit Session(std::string address, std::uint16_t port)
        : client(std::make_unique<DcapClient>(std::move(address), port)) {}

    std::unique_ptr<DcapClient> client;
    std::mutex pending_mutex;
    std::deque<AgentEvent> pending_events;
    std::atomic<std::uint64_t> last_activity_ms{nowMs()};
};

LiveWebGateway::LiveWebGateway(std::string serverAddress, std::uint16_t serverPort,
                               std::uint16_t httpPort, std::filesystem::path webRoot)
    : server_address_(std::move(serverAddress)), server_port_(serverPort),
      http_port_(httpPort), web_root_(std::move(webRoot)) {}

LiveWebGateway::~LiveWebGateway() {
    stop();
}

bool LiveWebGateway::start() {
    if (running_) return true;
    if (http_port_ == 0 || server_port_ == 0 || server_address_.empty()) return false;
    if (!std::filesystem::exists(web_root_ / "index.html") ||
        !std::filesystem::exists(web_root_ / "app.js") ||
        !std::filesystem::exists(web_root_ / "style.css")) {
        std::cerr << "[GATEWAY] Web assets not found under " << web_root_ << std::endl;
        return false;
    }
    if (!initializeSocketRuntime()) return false;
    socket_runtime_initialized_ = true;

    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCKET_HANDLE) {
        stop();
        return false;
    }
    setReuseAddress(listener_);
    setSocketTimeout(listener_, 500, 0);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(http_port_);
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
        ::listen(listener_, SOMAXCONN) != 0) {
        std::cerr << "[GATEWAY] bind/listen failed: " << lastSocketError() << std::endl;
        stop();
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&LiveWebGateway::acceptLoop, this);
    std::cout << "[GATEWAY] Listening on 0.0.0.0:" << http_port_
              << " and forwarding DCAP sessions to "
              << server_address_ << ":" << server_port_ << std::endl;
    return true;
}

void LiveWebGateway::stop() {
    const bool wasRunning = running_.exchange(false);
    if (listener_ != INVALID_SOCKET_HANDLE) {
        shutdownSocket(listener_);
        closeSocket(listener_);
        listener_ = INVALID_SOCKET_HANDLE;
    }
    if (accept_thread_.joinable()) accept_thread_.join();

    if (wasRunning || socket_runtime_initialized_) {
        std::unique_lock<std::mutex> lock(workers_mutex_);
        workers_cv_.wait(lock, [this] { return active_workers_ == 0; });
        lock.unlock();

        std::vector<std::shared_ptr<Session>> sessions;
        {
            std::lock_guard<std::mutex> sessionLock(sessions_mutex_);
            for (auto& entry : sessions_) sessions.push_back(entry.second);
            sessions_.clear();
        }
        for (auto& session : sessions) session->client->logout();

        if (socket_runtime_initialized_) {
            cleanupSocketRuntime();
            socket_runtime_initialized_ = false;
        }
    }
}

void LiveWebGateway::acceptLoop() {
    while (running_) {
        sockaddr_in address{};
        SocketLength length = sizeof(address);
        const auto client = accept(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        if (client == INVALID_SOCKET_HANDLE) {
            if (!running_) break;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            ++active_workers_;
        }
        std::thread([this, client] {
            handleConnection(client);
            shutdownSocket(client);
            closeSocket(client);
            {
                std::lock_guard<std::mutex> lock(workers_mutex_);
                --active_workers_;
            }
            workers_cv_.notify_all();
        }).detach();
    }
}

std::string LiveWebGateway::sessionTokenFromCookie(const std::string& cookie) const {
    return cookieValue(cookie, "dcap_session");
}

std::shared_ptr<LiveWebGateway::Session> LiveWebGateway::findSession(const std::string& token) {
    if (token.empty()) return {};
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const auto found = sessions_.find(token);
    if (found == sessions_.end()) return {};
    const auto now = nowMs();
    const auto last = found->second->last_activity_ms.load();
    if (now >= last && now - last > SESSION_IDLE_TIMEOUT_MS) {
        sessions_.erase(found);
        return {};
    }
    found->second->last_activity_ms = now;
    return found->second;
}

void LiveWebGateway::handleConnection(SocketHandle socket) {
#ifdef _WIN32
    DWORD timeoutMs = 30000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    timeval timeout{30, 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

    HttpRequest request;
    std::string readError;
    const int readStatus = readRequest(socket, request, readError);
    int status = readStatus;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
    std::string extraHeaders;
    json response = json::object();
    bool rawBody = false;

    if (readStatus != 200) {
        response = {{"error", readError}};
    } else if (request.method == "POST" && !sameOrigin(request)) {
        status = 403;
        response = {{"error", "Cross-origin requests are not allowed"}};
    } else {
        const auto query = request.target.find('?');
        const auto path = request.target.substr(0, query);

        if (request.method == "GET" &&
            (path == "/" || path == "/index.html" || path == "/app.js" || path == "/style.css")) {
            const auto filename = path == "/" ? "index.html" : path.substr(1);
            std::ifstream input(web_root_ / filename, std::ios::binary);
            if (!input) {
                status = 404;
                response = {{"error", "Web asset not found"}};
            } else {
                body.assign(std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>());
                contentType = filename == "index.html" ? "text/html; charset=utf-8" :
                    filename == "app.js" ? "application/javascript; charset=utf-8" :
                                           "text/css; charset=utf-8";
                status = 200;
                rawBody = true;
            }
        } else if (request.method == "OPTIONS") {
            status = 204;
        } else {
            const auto token = sessionTokenFromCookie(request.header("cookie"));

            if (request.method == "POST" && path == "/login") {
                json data;
                if (!parseJsonObject(request.body, data) ||
                    !data.contains("username") || !data["username"].is_string()) {
                    status = 400;
                    response = {{"error", "Username is required"}};
                } else {
                    const auto username = data["username"].get<std::string>();
                    if (username.empty() || username.size() > protocol::MAX_USERNAME_BYTES ||
                        !protocol::validUtf8(username)) {
                        status = 400;
                        response = {{"error", "Username must be valid UTF-8 and at most 32 bytes"}};
                    } else {
                        {
                            std::lock_guard<std::mutex> lock(sessions_mutex_);
                            if (sessions_.size() >= MAX_ACTIVE_SESSIONS) {
                                status = 429;
                                response = {{"error", "The public gateway is at session capacity"}};
                            }
                        }
                        if (status == 200) {
                            auto session = std::make_shared<Session>(server_address_, server_port_);
                            if (!session->client->connect()) {
                                status = 503;
                                response = {{"error", session->client->lastError()}};
                            } else if (!session->client->login(username)) {
                                status = 400;
                                response = {{"error", "Could not send login request"}};
                                session->client->disconnect();
                            } else {
                                bool authenticated = false;
                                bool rejected = false;
                                std::string rejectionReason;
                                const auto deadline = std::chrono::steady_clock::now() +
                                                      std::chrono::seconds(8);
                                while (std::chrono::steady_clock::now() < deadline &&
                                       !authenticated && !rejected) {
                                    for (auto& event : session->client->takeEvents()) {
                                        if (event.type == "LOGIN_OK") {
                                            authenticated = true;
                                        } else if (event.type == "LOGIN_FAIL") {
                                            rejected = true;
                                            rejectionReason = event.data.value(
                                                "error", std::string("Login rejected"));
                                        } else {
                                            std::lock_guard<std::mutex> eventLock(
                                                session->pending_mutex);
                                            session->pending_events.push_back(std::move(event));
                                        }
                                    }
                                    if (!authenticated && !rejected)
                                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                                }

                                if (authenticated) {
                                    try {
                                        const auto newToken = randomToken();
                                        {
                                            std::lock_guard<std::mutex> lock(sessions_mutex_);
                                            if (sessions_.size() >= MAX_ACTIVE_SESSIONS) {
                                                status = 429;
                                                response = {{"error", "The public gateway is at session capacity"}};
                                            } else {
                                                sessions_.emplace(newToken, session);
                                                const bool secureCookie = !std::getenv("DCAP_COOKIE_SECURE") ||
                                                    std::string(std::getenv("DCAP_COOKIE_SECURE")) != "0";
                                                extraHeaders = "Set-Cookie: dcap_session=" + newToken +
                                                    "; Path=/; HttpOnly; SameSite=Lax; Max-Age=43200" +
                                                    (secureCookie ? "; Secure" : "") + "\r\n";
                                                status = 200;
                                                response = {{"authenticated", true}, {"username", username}};
                                            }
                                        }
                                        if (status != 200) session->client->logout();
                                    } catch (const std::exception& error) {
                                        status = 500;
                                        response = {{"error", error.what()}};
                                        session->client->logout();
                                    }
                                } else {
                                    status = rejected ? 401 : 504;
                                    response = {{"error", rejected ? rejectionReason :
                                        "Timed out waiting for server authentication"}};
                                    session->client->disconnect();
                                }
                            }
                        }
                    }
                }
            } else if (request.method == "POST" && path == "/logout") {
                auto session = findSession(token);
                if (session) {
                    {
                        std::lock_guard<std::mutex> lock(sessions_mutex_);
                        sessions_.erase(token);
                    }
                    session->client->logout();
                }
                extraHeaders = "Set-Cookie: dcap_session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0" +
                    (std::getenv("DCAP_COOKIE_SECURE") &&
                     std::string(std::getenv("DCAP_COOKIE_SECURE")) == "0" ? "" : "; Secure") +
                    "\r\n";
                status = 200;
                response = {{"authenticated", false}};
            } else if (request.method == "GET" && path == "/status") {
                auto session = findSession(token);
                status = 200;
                response = {
                    {"connected", session && session->client->connected()},
                    {"authenticated", session && session->client->authenticated()},
                    {"username", session ? session->client->username() : std::string()},
                    {"server_port", server_port_},
                    {"last_connection_error", session ? session->client->lastError() : std::string()}
                };
            } else {
                auto session = findSession(token);
                if (!session) {
                    status = 401;
                    response = {{"error", "Authentication required"}};
                } else if (request.method == "GET" && path == "/user_list") {
                    if (!session->client->authenticated()) {
                        status = 401;
                        response = {{"error", "Authentication required"}};
                    } else {
                        status = 200;
                        response = {{"users", session->client->users()}};
                    }
                } else if (request.method == "GET" && path == "/events") {
                    json events = json::array();
                    {
                        std::lock_guard<std::mutex> lock(session->pending_mutex);
                        for (auto& event : session->pending_events)
                            events.push_back({{"type", event.type}, {"data", event.data}});
                        session->pending_events.clear();
                    }
                    for (auto& event : session->client->takeEvents())
                        events.push_back({{"type", event.type}, {"data", event.data}});
                    status = 200;
                    response = {{"events", events}};
                } else if (request.method == "POST" &&
                           (path == "/global_message" || path == "/private_message")) {
                    json data;
                    if (!session->client->authenticated()) {
                        status = 401;
                        response = {{"error", "Authentication required"}};
                    } else if (!parseJsonObject(request.body, data)) {
                        status = 400;
                        response = {{"error", "Invalid JSON body"}};
                    } else {
                        const auto content = data.value("content", std::string());
                        const bool sent = path == "/global_message"
                            ? session->client->sendGlobal(content)
                            : session->client->sendPrivate(
                                data.value("recipient", std::string()), content);
                        status = sent ? 202 : 422;
                        response = sent ? json{{"status", "accepted"}} :
                            json{{"error", "Invalid message or recipient"}};
                    }
                } else if (request.method == "POST" && path == "/file_decision") {
                    json data;
                    if (!session->client->authenticated()) {
                        status = 401;
                        response = {{"error", "Authentication required"}};
                    } else if (!parseJsonObject(request.body, data) ||
                               !data.contains("transfer_id") || !data.contains("accept") ||
                               !data["transfer_id"].is_string() || !data["accept"].is_boolean()) {
                        status = 400;
                        response = {{"error", "transfer_id and boolean accept are required"}};
                    } else {
                        const bool ok = session->client->decideFile(
                            data["transfer_id"].get<std::string>(),
                            data["accept"].get<bool>());
                        status = ok ? 202 : 404;
                        response = ok ? json{{"status", "accepted"}} :
                            json{{"error", "Unknown transfer offer"}};
                    }
                } else if (request.method == "POST" && path == "/file_upload") {
                    if (!session->client->authenticated()) {
                        status = 401;
                        response = {{"error", "Authentication required"}};
                    } else {
                        std::string recipient;
                        std::string filename;
                        std::string fileBytes;
                        if (!parseMultipart(request.body, request.header("content-type"),
                                            recipient, filename, fileBytes)) {
                            status = 400;
                            response = {{"error", "Expected multipart fields recipient and file"}};
                        } else if (fileBytes.size() > protocol::MAX_FILE_SIZE) {
                            status = 413;
                            response = {{"error", "File exceeds the 100 MiB protocol limit"}};
                        } else {
                            try {
                                const auto directory = std::filesystem::current_path() /
                                    "uploads" / "live-web" / token;
                                std::filesystem::create_directories(directory);
                                const auto uploadPath = directory /
                                    (std::to_string(nowMs()) + "-" + filename);
                                std::ofstream output(uploadPath, std::ios::binary);
                                if (!output) throw std::runtime_error("Could not create upload file");
                                output.write(fileBytes.data(),
                                    static_cast<std::streamsize>(fileBytes.size()));
                                output.close();
                                if (!output) throw std::runtime_error("Could not save uploaded file");
                                const auto transferId = session->client->sendFile(recipient, uploadPath);
                                if (transferId.empty()) {
                                    status = 422;
                                    response = {{"error", session->client->lastError().empty()
                                        ? "Could not start file transfer" : session->client->lastError()}};
                                } else {
                                    status = 202;
                                    response = {{"transfer_id", transferId}, {"status", "offered"}};
                                }
                            } catch (const std::exception& error) {
                                status = 500;
                                response = {{"error", error.what()}};
                            }
                        }
                    }
                } else if (request.method != "GET" && request.method != "POST") {
                    status = 405;
                    response = {{"error", "Method not allowed"}};
                } else {
                    status = 404;
                    response = {{"error", "Endpoint not found"}};
                }
            }
        }
    }

    if (!rawBody) body = status == 204 ? std::string() : jsonText(response);
    std::ostringstream headers;
    headers << "HTTP/1.1 " << status << " " << reasonPhrase(status) << "\r\n"
            << "Content-Type: " << contentType << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "Cache-Control: no-store\r\n"
            << "X-Content-Type-Options: nosniff\r\n"
            << "X-Frame-Options: DENY\r\n"
            << "Referrer-Policy: no-referrer\r\n"
            << "Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; "
               "connect-src 'self'; img-src 'self' data:; base-uri 'self'; form-action 'self'; "
               "frame-ancestors 'none'\r\n"
            << extraHeaders << "\r\n";
    sendAll(socket, headers.str());
    if (!body.empty()) sendAll(socket, body);
}
