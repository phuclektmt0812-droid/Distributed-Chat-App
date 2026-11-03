#pragma once
#include "../platform/socket_compat.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ClientSession {
    std::string username;
    SocketHandle socket = INVALID_SOCKET_HANDLE;
    std::array<std::uint8_t,16> sessionId{};
    std::array<std::uint8_t,32> udpToken{};
    sockaddr_in udpEndpoint{};
    bool authenticated=false;
    bool udpRegistered=false;
    std::uint64_t lastHeartbeatMs=0;
    std::mutex sendMutex;
};

enum class SignUpStatus {
    SUCCESS,
    ACCOUNT_EXISTS,
    INVALID_USERNAME,
    WEAK_PASSWORD,
    STORAGE_ERROR
};

enum class AccountLoginStatus {
    SUCCESS,
    ACCOUNT_NOT_FOUND,
    INVALID_PASSWORD,
    ALREADY_ONLINE
};

class UserManager {
public:
    UserManager();
    ~UserManager();
    UserManager(const UserManager&) = delete;
    UserManager& operator=(const UserManager&) = delete;

    SignUpStatus signUp(const std::string& username, const std::string& password);
    AccountLoginStatus login(const std::shared_ptr<ClientSession>& session,
                             const std::string& password);
    void remove(const std::shared_ptr<ClientSession>& session);
    std::shared_ptr<ClientSession> find(const std::string& username);
    std::vector<std::shared_ptr<ClientSession>> snapshot();
    std::vector<std::string> usernames();
    bool bindUdp(const std::array<std::uint8_t,32>& token,
                 const sockaddr_in& endpoint);
    std::shared_ptr<ClientSession> findByUdpEndpoint(const sockaddr_in& endpoint);
    std::vector<std::shared_ptr<ClientSession>> expiredUdp(std::uint64_t nowMs);
    std::uint32_t revision() const;
    std::size_t accountCount() const;

private:
    struct AccountNode {
        std::string username;
        std::array<std::uint8_t,16> salt{};
        std::array<std::uint8_t,32> passwordHash{};
        std::weak_ptr<ClientSession> activeSession;
        std::unique_ptr<AccountNode> next;
    };

    AccountNode* findAccount(const std::string& username);
    bool appendAccountRecord(const AccountNode& account);
    void loadAccounts();

    mutable std::mutex mutex_;
    std::unique_ptr<AccountNode> accounts_;
    std::unordered_map<std::string,std::shared_ptr<ClientSession>> users_;
    std::filesystem::path account_store_path_;
    bool storage_ready_=true;
    std::uint32_t revision_=0;
};
