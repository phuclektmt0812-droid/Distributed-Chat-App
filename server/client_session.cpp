#include "../server/client_session.h"

#include "../server/user_manager.h"
#include "../server/message_router.h"
#include "../protocol/message.h"

#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <limits>
#include <mutex>
#include <memory>

#include <nlohmann/json.hpp>

using namespace std;
using nlohmann::json;

namespace {

constexpr uint32_t MAX_MESSAGE_SIZE = 1024 * 1024;

// Send every byte, including when send() transmits only part
// of the supplied buffer.
bool sendAll(
    SOCKET socketHandle,
    const char* data,
    size_t length
) {
    size_t totalSent = 0;

    while (totalSent < length) {
        const size_t remaining = length - totalSent;

        const int chunkSize = static_cast<int>(
            remaining > static_cast<size_t>(
                numeric_limits<int>::max()
            )
                ? numeric_limits<int>::max()
                : remaining
        );

        const int sent = send(
            socketHandle,
            data + totalSent,
            chunkSize,
            0
        );

        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }

        totalSent += static_cast<size_t>(sent);
    }

    return true;
}

// Receive exactly the requested number of bytes.
// A return value of false indicates disconnection or failure.
bool receiveAll(
    SOCKET socketHandle,
    char* buffer,
    size_t length
) {
    size_t totalReceived = 0;

    while (totalReceived < length) {
        const size_t remaining = length - totalReceived;

        const int chunkSize = static_cast<int>(
            remaining > static_cast<size_t>(
                numeric_limits<int>::max()
            )
                ? numeric_limits<int>::max()
                : remaining
        );

        const int received = recv(
            socketHandle,
            buffer + totalReceived,
            chunkSize,
            0
        );

        if (received == SOCKET_ERROR || received == 0) {
            return false;
        }

        totalReceived += static_cast<size_t>(received);
    }

    return true;
}

string messageTypeToString(MessageType type) {
    switch (type) {
        case MessageType::LOGIN:
            return "LOGIN";

        case MessageType::LOGIN_OK:
            return "LOGIN_OK";

        case MessageType::LOGIN_FAIL:
            return "LOGIN_FAIL";

        case MessageType::LOGOUT:
            return "LOGOUT";

        case MessageType::USER_LIST:
            return "USER_LIST";

        case MessageType::CHAT_ALL:
            return "CHAT_ALL";

        case MessageType::CHAT_PRIVATE:
            return "CHAT_PRIVATE";

        case MessageType::CHAT_DELIVER:
            return "CHAT_DELIVER";

        case MessageType::CHAT_ERROR:
            return "CHAT_ERROR";

        case MessageType::PING:
            return "PING";

        case MessageType::PONG:
            return "PONG";

        case MessageType::FILE_START:
            return "FILE_START";

        case MessageType::FILE_ACCEPT:
            return "FILE_ACCEPT";

        case MessageType::FILE_REJECT:
            return "FILE_REJECT";

        case MessageType::FILE_END:
            return "FILE_END";

        case MessageType::FILE_COMPLETE:
            return "FILE_COMPLETE";

        case MessageType::ERROR:
            return "ERROR";
    }

    return "ERROR";
}

bool stringToMessageType(
    const string& value,
    MessageType& type
) {
    if (value == "LOGIN") {
        type = MessageType::LOGIN;
    } else if (value == "LOGIN_OK") {
        type = MessageType::LOGIN_OK;
    } else if (value == "LOGIN_FAIL") {
        type = MessageType::LOGIN_FAIL;
    } else if (value == "LOGOUT") {
        type = MessageType::LOGOUT;
    } else if (value == "USER_LIST") {
        type = MessageType::USER_LIST;
    } else if (value == "CHAT_ALL") {
        type = MessageType::CHAT_ALL;
    } else if (value == "CHAT_PRIVATE") {
        type = MessageType::CHAT_PRIVATE;
    } else if (value == "CHAT_DELIVER") {
        type = MessageType::CHAT_DELIVER;
    } else if (value == "CHAT_ERROR") {
        type = MessageType::CHAT_ERROR;
    } else if (value == "PING") {
        type = MessageType::PING;
    } else if (value == "PONG") {
        type = MessageType::PONG;
    } else if (value == "FILE_START") {
        type = MessageType::FILE_START;
    } else if (value == "FILE_ACCEPT") {
        type = MessageType::FILE_ACCEPT;
    } else if (value == "FILE_REJECT") {
        type = MessageType::FILE_REJECT;
    } else if (value == "FILE_END") {
        type = MessageType::FILE_END;
    } else if (value == "FILE_COMPLETE") {
        type = MessageType::FILE_COMPLETE;
    } else if (value == "ERROR") {
        type = MessageType::ERROR;
    } else {
        return false;
    }

    return true;
}

} // namespace

ClientSession::~ClientSession() {
    disconnect();
}

void ClientSession::run() {
    connected = true;

    while (connected) {
        Message message;

        if (!receiveMessage(message)) {
            break;
        }

        if (!processMessage(message)) {
            break;
        }
    }

    disconnect();
}

bool ClientSession::receiveMessage(Message& message) {
    if (clientSocket == INVALID_SOCKET) {
        return false;
    }

    // Read the 4-byte TCP frame length.
    uint32_t networkLength = 0;

    if (!receiveAll(
            clientSocket,
            reinterpret_cast<char*>(&networkLength),
            sizeof(networkLength)
        )) {
        return false;
    }

    const uint32_t messageLength = ntohl(networkLength);

    if (messageLength == 0 ||
        messageLength > MAX_MESSAGE_SIZE) {
        cerr << "Invalid TCP message length.\n";
        return false;
    }

    string data(messageLength, '\0');

    if (!receiveAll(
            clientSocket,
            data.data(),
            messageLength
        )) {
        return false;
    }

    try {
        const json parsed = json::parse(data);

        if (!parsed.is_object() ||
            !parsed.contains("type") ||
            !parsed.contains("payload") ||
            !parsed["type"].is_string() ||
            !parsed["payload"].is_object()) {
            return false;
        }

        MessageType parsedType;

        if (!stringToMessageType(
                parsed["type"].get<string>(),
                parsedType
            )) {
            cerr << "Unknown message type.\n";
            return false;
        }

        message.type = parsedType;
        message.payload = parsed["payload"];

        return true;
    } catch (const json::exception& error) {
        cerr << "Invalid JSON message: "
             << error.what() << '\n';

        return false;
    }
}

bool ClientSession::processMessage(
    const Message& message
) {
    if (message.type == MessageType::LOGIN) {
        if (authenticated) {
            Message response;
            response.type = MessageType::LOGIN_FAIL;
            response.payload = {
                {"error", "Already authenticated"}
            };

            sendMessage(response);
            return true;
        }

        if (!login(message)) {
            Message response;
            response.type = MessageType::LOGIN_FAIL;
            response.payload = {
                {"error", "Invalid credentials or username unavailable"}
            };

            sendMessage(response);
        }

        return true;
    }

    if (!authenticated) {
        Message response;
        response.type = MessageType::ERROR;
        response.payload = {
            {"error", "Authentication required"}
        };

        sendMessage(response);

        return true;
    }

    switch (message.type) {
        case MessageType::LOGOUT:
            connected = false;
            return true;

        case MessageType::CHAT_ALL: {
            if (!message.payload.contains("content") ||
                !message.payload["content"].is_string()) {
                return false;
            }

            const string content =
                message.payload["content"].get<string>();

            return messageRouter->broadcastMessage(
                username,
                content
            );
        }

        case MessageType::CHAT_PRIVATE: {
            if (!message.payload.contains("recipient") ||
                !message.payload.contains("content") ||
                !message.payload["recipient"].is_string() ||
                !message.payload["content"].is_string()) {
                return false;
            }

            const string recipient =
                message.payload["recipient"].get<string>();

            const string content =
                message.payload["content"].get<string>();

            const bool sent =
                messageRouter->sendPrivateMessage(
                    username,
                    recipient,
                    content
                );

            if (!sent) {
                Message response;
                response.type = MessageType::CHAT_ERROR;
                response.payload = {
                    {"error", "Recipient unavailable"},
                    {"recipient", recipient}
                };

                sendMessage(response);
            }

            return true;
        }

        case MessageType::PING: {
            Message response;
            response.type = MessageType::PONG;
            response.payload = json::object();

            return sendMessage(response);
        }

        case MessageType::FILE_START: {
            if (!messageRouter->startFileTransfer(
                    username,
                    message.payload
                )) {
                Message response;
                response.type = MessageType::FILE_REJECT;
                response.payload = {
                    {"error", "Recipient unavailable or transfer request invalid"}
                };
                if (message.payload.contains("transfer_id") &&
                    message.payload["transfer_id"].is_number_unsigned()) {
                    response.payload["transfer_id"] =
                        message.payload["transfer_id"];
                }
                return sendMessage(response);
            }
            return true;
        }

        case MessageType::FILE_ACCEPT:
        case MessageType::FILE_REJECT: {
            if (!messageRouter->respondToFileTransfer(
                    username,
                    message.payload,
                    message.type == MessageType::FILE_ACCEPT
                )) {
                Message response;
                response.type = MessageType::ERROR;
                response.payload = {
                    {"error", "File offer is no longer available"}
                };
                return sendMessage(response);
            }
            return true;
        }

        default: {
            Message response;
            response.type = MessageType::ERROR;
            response.payload = {
                {"error", "Unsupported message type"}
            };

            return sendMessage(response);
        }
    }
}

bool ClientSession::sendMessage(const Message& message) {
    if (clientSocket == INVALID_SOCKET) {
        return false;
    }

    json serialized;

    serialized["type"] = messageTypeToString(message.type);
    serialized["payload"] = message.payload;

    const string data = serialized.dump();

    if (data.empty() || data.size() > MAX_MESSAGE_SIZE) {
        return false;
    }

    const uint32_t networkLength = htonl(
        static_cast<uint32_t>(data.size())
    );

    lock_guard<mutex> lock(sendMutex);

    if (!sendAll(
            clientSocket,
            reinterpret_cast<const char*>(&networkLength),
            sizeof(networkLength)
        )) {
        connected = false;
        return false;
    }

    if (!sendAll(
            clientSocket,
            data.data(),
            data.size()
        )) {
        connected = false;
        return false;
    }

    return true;
}

bool ClientSession::login(const Message& message) {
    if (!message.payload.contains("username") ||
        !message.payload.contains("password") ||
        !message.payload["username"].is_string() ||
        !message.payload["password"].is_string()) {
        return false;
    }

    const string requestedUsername =
        message.payload["username"].get<string>();

    const string password =
        message.payload["password"].get<string>();

    std::uint16_t udpPort = 0;
    if (message.payload.contains("udp_port")) {
        if (!message.payload["udp_port"].is_number_unsigned()) {
            return false;
        }
        const std::uint64_t requestedUdpPort =
            message.payload["udp_port"].get<std::uint64_t>();
        if (requestedUdpPort > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
        udpPort = static_cast<std::uint16_t>(requestedUdpPort);
    }

    // Temporary validation only.
    // Replace with proper credential verification.
    if (requestedUsername.empty() ||
        password.empty() ||
        requestedUsername.size() > 64) {
        return false;
    }

    username = requestedUsername;

    char ipBuffer[INET_ADDRSTRLEN]{};

    if (inet_ntop(
            AF_INET,
            &clientAddress.sin_addr,
            ipBuffer,
            sizeof(ipBuffer)
        ) == nullptr) {
        username.clear();
        return false;
    }

    UserInfo user{};

    user.username = username;
    user.session = shared_from_this();
    user.ipAddress = ipBuffer;
    user.tcpPort = ntohs(clientAddress.sin_port);
    user.udpPort = udpPort;
    user.lastHeartbeat = chrono::steady_clock::now();

    if (!userManager->addUser(user)) {
        username.clear();
        return false;
    }

    authenticated = true;

    Message response;
    response.type = MessageType::LOGIN_OK;
    response.payload = {
        {"username", username}
    };

    if (!sendMessage(response)) {
        logout();
        return false;
    }

    messageRouter->notifyUserListChanged();

    cout << "User logged in: " << username << '\n';

    return true;
}

void ClientSession::logout() {
    if (!authenticated.exchange(false)) {
        return;
    }

    const string oldUsername = username;

    userManager->removeUser(oldUsername);

    username.clear();

    cout << "User logged out: "
         << oldUsername << '\n';

    messageRouter->notifyUserListChanged();
}

void ClientSession::disconnect() {
    connected = false;

    logout();

    if (clientSocket != INVALID_SOCKET) {
        shutdown(clientSocket, SD_BOTH);
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
    }
}