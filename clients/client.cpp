#include "../clients/client.h"

#include <utility>

namespace {

std::string messageTypeToString(MessageType type) {
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

} // namespace

ChatClient::~ChatClient() {
    disconnect();
    if (receiverThread.joinable()) {
        receiverThread.join();
    }
}

bool ChatClient::connect() {
    if (receiverThread.joinable()) {
        if (running.load()) {
            return connection.isConnected();
        }
        receiverThread.join();
    }

    if (fileTransfer.getLocalPort() == 0 &&
        !fileTransfer.initializeUdpSocket(0)) {
        return false;
    }

    if (!connection.connectToServer(
            connection.serverAddress,
            connection.serverPort
        )) {
        return false;
    }

    running = true;
    if (!receiverThread.joinable()) {
        receiverThread = std::thread(&ChatClient::receiveLoop, this);
    }

    return true;
}

bool ChatClient::login(
        const std::string& username,
        const std::string& password
    ) {
    if (!connection.isConnected()) {
        return false;
    }

    this->username = username;

    Message loginMessage;
    loginMessage.type = MessageType::LOGIN;
    loginMessage.payload = {
        {"username", username},
        {"password", password},
        {"udp_port", fileTransfer.getLocalPort()}
    };

    const bool sent = connection.sendFramedMessage(loginMessage);
    if (!sent) {
        this->username.clear();
    }

    return sent;
}

void ChatClient::logout() {
    if (!connection.isConnected()) {
        authenticated = false;
        return;
    }

    Message logoutMessage;
    logoutMessage.type = MessageType::LOGOUT;
    logoutMessage.payload = nlohmann::json::object();

    connection.sendFramedMessage(logoutMessage);
    authenticated = false;
    username.clear();
    connection.disconnect();
}

bool ChatClient::sendGlobalMessage(const std::string& content) {
    if (!connection.isConnected() || !authenticated.load()) {
        return false;
    }

    if (content.empty()) {
        return false;
    }

    Message message;
    message.type = MessageType::CHAT_ALL;
    message.payload = {
        {"content", content}
    };

    return connection.sendFramedMessage(message);
}

bool ChatClient::sendPrivateMessage(
        const std::string& recipient,
        const std::string& content
    ) {
    if (!connection.isConnected() || !authenticated.load()) {
        return false;
    }

    if (recipient.empty() || content.empty()) {
        return false;
    }

    Message message;
    message.type = MessageType::CHAT_PRIVATE;
    message.payload = {
        {"recipient", recipient},
        {"content", content}
    };

    return connection.sendFramedMessage(message);
}

bool ChatClient::sendFile(
    const std::string& recipient,
    const std::filesystem::path& filePath,
    const sockaddr_in& recipientAddress
) {
    if (!connection.isConnected() || !authenticated.load() ||
        fileTransfer.getLocalPort() == 0) {
        return false;
    }

    fileTransfer.configureSession(
        username,
        [this](const Message& message) {
            return connection.sendFramedMessage(message);
        },
        [this](
            std::uint32_t transferId,
            std::chrono::milliseconds timeout
        ) {
            return waitForFileResponse(transferId, timeout);
        }
    );
    return fileTransfer.startSendFile(
        recipient,
        filePath,
        recipientAddress
    );
}

bool ChatClient::sendFile(
    const std::string& recipient,
    const std::filesystem::path& filePath
) {
    sockaddr_in unknownPeer{};
    return sendFile(recipient, filePath, unknownPeer);
}

bool ChatClient::respondToFileOffer(
    std::uint32_t transferId,
    bool accept
) {
    if (!connection.isConnected() || !authenticated.load() ||
        transferId == 0) {
        return false;
    }

    Message response;
    response.type = accept
        ? MessageType::FILE_ACCEPT
        : MessageType::FILE_REJECT;
    response.payload = {
        {"transfer_id", transferId}
    };
    return connection.sendFramedMessage(response);
}

std::optional<Message> ChatClient::waitForFileResponse(
    std::uint32_t transferId,
    std::chrono::milliseconds timeout
) {
    std::unique_lock<std::mutex> lock(fileResponseMutex);
    if (!fileResponseChanged.wait_for(lock, timeout, [this, transferId]() {
            return fileResponses.find(transferId) != fileResponses.end() ||
                !running.load();
        })) {
        return std::nullopt;
    }

    const auto found = fileResponses.find(transferId);
    if (found == fileResponses.end()) {
        return std::nullopt;
    }
    Message response = std::move(found->second);
    fileResponses.erase(found);
    return response;
}

void ChatClient::receiveLoop() {
    while (running.load()) {
        Message message;
        if (!connection.receiveFramedMessage(message)) {
            break;
        }

        switch (message.type) {
            case MessageType::USER_LIST: {
                if (message.payload.contains("users") &&
                    message.payload["users"].is_array()) {
                    std::vector<std::string> users;
                    for (const auto& user : message.payload["users"]) {
                        if (user.is_string()) {
                            users.push_back(user.get<std::string>());
                        }
                    }
                    std::lock_guard<std::mutex> lock(userListMutex);
                    latestUserList = std::move(users);
                }
                break;
            }
            case MessageType::LOGIN_OK:
                authenticated = true;
                break;
            case MessageType::LOGIN_FAIL:
                authenticated = false;
                break;
            case MessageType::FILE_ACCEPT:
            case MessageType::FILE_REJECT:
                if (message.payload.contains("transfer_id") &&
                    message.payload["transfer_id"].is_number_unsigned()) {
                    const std::uint32_t transferId =
                        message.payload["transfer_id"].get<std::uint32_t>();
                    {
                        std::lock_guard<std::mutex> lock(fileResponseMutex);
                        fileResponses.insert_or_assign(transferId, message);
                    }
                    fileResponseChanged.notify_all();
                }
                break;
            default:
                break;
        }

        const std::string eventType = messageTypeToString(message.type);
        ClientEvent event{eventType, message.payload};

        {
            std::lock_guard<std::mutex> lock(eventQueueMutex);
            eventQueue.push(std::move(event));
        }
    }

    running = false;
    authenticated = false;
    fileResponseChanged.notify_all();
}

std::optional<ClientEvent> ChatClient::getNextEvent() {
    std::lock_guard<std::mutex> lock(eventQueueMutex);

    if (eventQueue.empty()) {
        return std::nullopt;
    }

    ClientEvent event = std::move(eventQueue.front());
    eventQueue.pop();
    return event;
}

std::vector<std::string> ChatClient::getLatestUserList() const {
    std::lock_guard<std::mutex> lock(userListMutex);
    return latestUserList;
}

void ChatClient::disconnect() {
    running = false;

    if (connection.isConnected()) {
        Message logoutMessage;
        logoutMessage.type = MessageType::LOGOUT;
        logoutMessage.payload = nlohmann::json::object();
        connection.sendFramedMessage(logoutMessage);
    }

    connection.disconnect();
    authenticated = false;
    username.clear();
}