#pragma once

#include <string>
#include <iostream>
#include <memory>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <chrono>
#include "../server/user_manager.h"
#include "../protocol/message.h"
using namespace std;

class UserManager;
class ClientSession;

class MessageRouter {
private:
    struct PendingFileTransfer {
        string sender;
        string recipient;
        std::chrono::steady_clock::time_point createdAt;
    };

    UserManager* userManager;
    mutex fileTransfersMutex;
    unordered_map<std::uint32_t, PendingFileTransfer> pendingFileTransfers;

public:
    explicit MessageRouter(UserManager* users) {
        this->userManager = users;
    }

    bool broadcastMessage(
        const string& sender,
        const string& content
    );

    bool sendPrivateMessage(
        const string& sender,
        const string& recipient,
        const string& content
    );

    bool startFileTransfer(
        const string& sender,
        const nlohmann::json& payload
    );

    bool respondToFileTransfer(
        const string& responder,
        const nlohmann::json& payload,
        bool accepted
    );

    void sendUserList();

    void notifyUserListChanged();
};