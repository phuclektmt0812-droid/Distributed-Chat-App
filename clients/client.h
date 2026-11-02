#pragma once

#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <optional>
#include <vector>
#include <condition_variable>
#include <unordered_map>
#include <chrono>
#include <filesystem>

#include "../protocol/message.h"
#include "connection.h"
#include "file_transfer.h"

struct ClientEvent {
    std::string type;
    nlohmann::json data;
};

class ChatClient {
private:
    Connection connection;

    std::string username;

    std::atomic<bool> authenticated;
    std::atomic<bool> running;

    std::thread receiverThread;

    std::queue<ClientEvent> eventQueue;
    std::mutex eventQueueMutex;

    std::mutex loginMutex;
    std::vector<std::string> latestUserList;
    mutable std::mutex userListMutex;
    FileTransfer fileTransfer;
    std::mutex fileResponseMutex;
    std::condition_variable fileResponseChanged;
    std::unordered_map<std::uint32_t, Message> fileResponses;

    std::optional<Message> waitForFileResponse(
        std::uint32_t transferId,
        std::chrono::milliseconds timeout
    );

public:
    ChatClient(
        const std::string& serverAddress,
        std::uint16_t serverPort
    ) : connection(serverAddress, serverPort),
        authenticated(false),
        running(false) {}

    ~ChatClient();

    bool connect();

    bool login(
        const std::string& username,
        const std::string& password
    );

    void logout();

    bool sendGlobalMessage(const std::string& content);

    bool sendPrivateMessage(
        const std::string& recipient,
        const std::string& content
    );

    bool sendFile(
        const std::string& recipient,
        const std::filesystem::path& filePath,
        const sockaddr_in& recipientAddress
    );

    bool sendFile(
        const std::string& recipient,
        const std::filesystem::path& filePath
    );

    bool respondToFileOffer(
        std::uint32_t transferId,
        bool accept
    );

    void receiveLoop();

    std::optional<ClientEvent> getNextEvent();

    void disconnect();

    bool isAuthenticated() const {
        return this->authenticated.load();
    }

    bool isConnected() const {
        return connection.isConnected();
    }

    std::string getUsername() const {
        return this->username;
    }

    std::vector<std::string> getLatestUserList() const;
};