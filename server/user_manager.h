#pragma once

#include <winsock2.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <chrono>
#include <cstdint>
#include <optional>
#include <algorithm>
#include <utility>

using namespace std;
class ClientSession;

struct UserInfo {
    string username;

    weak_ptr<ClientSession> session;

    string ipAddress;

    uint16_t tcpPort;
    uint16_t udpPort;

    chrono::steady_clock::time_point lastHeartbeat;
};

class UserManager {
private:
    unordered_map<string, UserInfo> users;
    mutable mutex usersMutex;

public:
    bool addUser(const UserInfo& user);

    bool removeUser(const string& username);

    optional<UserInfo> findUser(
        const string& username
    ) const;

    bool isOnline(const string& username) const;

    vector<string> getOnlineUsers() const;

    void updateHeartbeat(const string& username);

    vector<string> removeExpiredUsers(
        chrono::seconds timeout
    );
};