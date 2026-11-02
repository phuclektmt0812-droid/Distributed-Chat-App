#include "../server/message_router.h"

#include "../server/user_manager.h"
#include "../server/client_session.h"
#include "../protocol/message.h"

#include <limits>

bool MessageRouter::broadcastMessage(
    const string& sender,
    const string& content
) {
    if (sender.empty() || content.empty()) {
        return false;
    }

    const vector<string> usernames =
        userManager->getOnlineUsers();

    Message message;
    message.type = MessageType::CHAT_DELIVER;
    message.payload = {
        {"sender", sender},
        {"content", content},
        {"private", false}
    };

    bool deliveredToAtLeastOneUser = false;

    for (const string& username : usernames) {
        auto user = userManager->findUser(username);

        if (!user.has_value()) {
            continue;
        }

        auto session = user->session.lock();

        if (!session || !session->isConnected()
            || !session->isAuthenticated()) {
            continue;
        }

        if (session->sendMessage(message)) {
            deliveredToAtLeastOneUser = true;
        }
    }

    return deliveredToAtLeastOneUser;
}

bool MessageRouter::sendPrivateMessage(
    const string& sender,
    const string& recipient,
    const string& content
) {
    if (sender.empty()
        || recipient.empty()
        || content.empty()) {
        return false;
    }

    auto recipientInfo = userManager->findUser(recipient);

    if (!recipientInfo.has_value()) {
        return false;
    }

    auto recipientSession = recipientInfo->session.lock();

    if (!recipientSession
        || !recipientSession->isConnected()
        || !recipientSession->isAuthenticated()) {
        return false;
    }

    Message message;
    message.type = MessageType::CHAT_DELIVER;
    message.payload = {
        {"sender", sender},
        {"recipient", recipient},
        {"content", content},
        {"private", true}
    };

    return recipientSession->sendMessage(message);
}

bool MessageRouter::startFileTransfer(
    const string& sender,
    const nlohmann::json& payload
) {
    if (sender.empty() ||
        !payload.contains("transfer_id") ||
        !payload["transfer_id"].is_number_unsigned() ||
        !payload.contains("recipient") ||
        !payload["recipient"].is_string() ||
        !payload.contains("filename") ||
        !payload["filename"].is_string() ||
        !payload.contains("total_size") ||
        !payload["total_size"].is_number_unsigned()) {
        return false;
    }

    const std::uint64_t requestedTransferId =
        payload["transfer_id"].get<std::uint64_t>();
    const string recipient =
        payload["recipient"].get<string>();
    const string filename =
        payload["filename"].get<string>();
    const std::uint64_t totalSize =
        payload["total_size"].get<std::uint64_t>();

    if (requestedTransferId == 0 ||
        requestedTransferId > std::numeric_limits<std::uint32_t>::max() ||
        recipient.empty() ||
        filename.empty() || filename.size() > 255 ||
        filename.find_first_of("/\\:") != string::npos ||
        totalSize > 1024ull * 1024ull * 1024ull) {
        return false;
    }
    const std::uint32_t transferId =
        static_cast<std::uint32_t>(requestedTransferId);

    auto recipientInfo = userManager->findUser(recipient);
    auto senderInfo = userManager->findUser(sender);
    if (!recipientInfo.has_value() || !senderInfo.has_value() ||
        recipientInfo->udpPort == 0 ||
        senderInfo->udpPort == 0 ||
        !recipientInfo->session.lock() ||
        !senderInfo->session.lock()) {
        return false;
    }

    const auto recipientSession = recipientInfo->session.lock();
    const auto senderSession = senderInfo->session.lock();
    if (!recipientSession || !senderSession ||
        !recipientSession->isConnected() ||
        !recipientSession->isAuthenticated() ||
        !senderSession->isConnected() ||
        !senderSession->isAuthenticated()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(fileTransfersMutex);
        const auto now = std::chrono::steady_clock::now();
        for (auto entry = pendingFileTransfers.begin();
             entry != pendingFileTransfers.end();) {
            if (now - entry->second.createdAt > std::chrono::minutes(1)) {
                entry = pendingFileTransfers.erase(entry);
            } else {
                ++entry;
            }
        }

        if (pendingFileTransfers.find(transferId) !=
            pendingFileTransfers.end()) {
            return false;
        }
        pendingFileTransfers.emplace(
            transferId,
            PendingFileTransfer{sender, recipient, now}
        );
    }

    Message offer;
    offer.type = MessageType::FILE_START;
    offer.payload = {
        {"transfer_id", transferId},
        {"sender", sender},
        {"filename", filename},
        {"total_size", totalSize},
        {"sender_ip", senderInfo->ipAddress},
        {"sender_udp_port", senderInfo->udpPort}
    };

    if (!recipientSession->sendMessage(offer)) {
        std::lock_guard<std::mutex> lock(fileTransfersMutex);
        pendingFileTransfers.erase(transferId);
        return false;
    }
    return true;
}

bool MessageRouter::respondToFileTransfer(
    const string& responder,
    const nlohmann::json& payload,
    bool accepted
) {
    if (responder.empty() ||
        !payload.contains("transfer_id") ||
        !payload["transfer_id"].is_number_unsigned()) {
        return false;
    }

    const std::uint64_t requestedTransferId =
        payload["transfer_id"].get<std::uint64_t>();
    if (requestedTransferId == 0 ||
        requestedTransferId > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const std::uint32_t transferId =
        static_cast<std::uint32_t>(requestedTransferId);
    PendingFileTransfer pending;
    {
        std::lock_guard<std::mutex> lock(fileTransfersMutex);
        const auto found = pendingFileTransfers.find(transferId);
        if (found == pendingFileTransfers.end() ||
            found->second.recipient != responder) {
            return false;
        }
        if (std::chrono::steady_clock::now() -
                found->second.createdAt > std::chrono::seconds(30)) {
            pendingFileTransfers.erase(found);
            return false;
        }
        pending = found->second;
        pendingFileTransfers.erase(found);
    }

    auto senderInfo = userManager->findUser(pending.sender);
    auto recipientInfo = userManager->findUser(pending.recipient);
    if (!senderInfo.has_value() || !recipientInfo.has_value()) {
        return false;
    }

    auto senderSession = senderInfo->session.lock();
    if (!senderSession || !senderSession->isConnected() ||
        !senderSession->isAuthenticated()) {
        return false;
    }

    Message response;
    response.type = accepted
        ? MessageType::FILE_ACCEPT
        : MessageType::FILE_REJECT;
    response.payload = {
        {"transfer_id", transferId}
    };

    if (accepted) {
        if (recipientInfo->udpPort == 0) {
            response.type = MessageType::FILE_REJECT;
            response.payload["error"] = "Recipient has no registered UDP port";
        } else {
            response.payload["recipient_ip"] = recipientInfo->ipAddress;
            response.payload["recipient_udp_port"] = recipientInfo->udpPort;
        }
    }

    return senderSession->sendMessage(response);
}

void MessageRouter::sendUserList() {
    const vector<string> usernames =
        userManager->getOnlineUsers();

    Message message;
    message.type = MessageType::USER_LIST;
    message.payload = {
        {"users", usernames}
    };

    for (const string& username : usernames) {
        auto user = userManager->findUser(username);

        if (!user.has_value()) {
            continue;
        }

        auto session = user->session.lock();

        if (!session
            || !session->isConnected()
            || !session->isAuthenticated()) {
            continue;
        }

        session->sendMessage(message);
    }
}

void MessageRouter::notifyUserListChanged() {
    sendUserList();
}