#pragma once
#include "../platform/socket_compat.h"
#include <array>
#include <cstdint>
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

class UserManager {
public:
    bool add(const std::shared_ptr<ClientSession>& session);
    void remove(const std::string& username);
    std::shared_ptr<ClientSession> find(const std::string& username);
    std::vector<std::shared_ptr<ClientSession>> snapshot();
    std::vector<std::string> usernames();
    bool bindUdp(const std::array<std::uint8_t,32>& token,
                 const sockaddr_in& endpoint);
    std::shared_ptr<ClientSession> findByUdpEndpoint(const sockaddr_in& endpoint);
    std::vector<std::shared_ptr<ClientSession>> expiredUdp(std::uint64_t nowMs);
    std::uint32_t revision() const;
private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string,std::shared_ptr<ClientSession>> users_;
    std::uint32_t revision_=0;
};
