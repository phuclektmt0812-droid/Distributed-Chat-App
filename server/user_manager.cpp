#include "../server/user_manager.h"

bool UserManager::addUser(const UserInfo& user) {
    if (user.username.empty()) {
        return false;
    }

    lock_guard<mutex> lock(usersMutex);

    // The username is the unique key.
    if (users.find(user.username) != users.end()) {
        return false;
    }

    users.emplace(user.username, user);

    return true;
}

bool UserManager::removeUser(const string& username) {
    lock_guard<mutex> lock(usersMutex);

    return users.erase(username) > 0;
}

optional<UserInfo> UserManager::findUser(
    const string& username
) const {
    lock_guard<mutex> lock(usersMutex);

    auto it = users.find(username);

    if (it == users.end()) {
        return nullopt;
    }

    return it->second;
}

bool UserManager::isOnline(const string& username) const {
    lock_guard<mutex> lock(usersMutex);

    return users.find(username) != users.end();
}

vector<string> UserManager::getOnlineUsers() const {
    lock_guard<mutex> lock(usersMutex);

    vector<string> onlineUsers;
    onlineUsers.reserve(users.size());

    for (const auto& entry : users) {
        onlineUsers.push_back(entry.first);
    }

    sort(onlineUsers.begin(), onlineUsers.end());

    return onlineUsers;
}

void UserManager::updateHeartbeat(const string& username) {
    lock_guard<mutex> lock(usersMutex);

    auto it = users.find(username);

    if (it != users.end()) {
        it->second.lastHeartbeat =
            chrono::steady_clock::now();
    }
}

vector<string> UserManager::removeExpiredUsers(
    chrono::seconds timeout
) {
    vector<string> expiredUsers;

    const auto now = chrono::steady_clock::now();

    lock_guard<mutex> lock(usersMutex);

    for (auto it = users.begin(); it != users.end();) {
        const auto elapsed =
            now - it->second.lastHeartbeat;

        if (elapsed >= timeout) {
            expiredUsers.push_back(it->first);
            it = users.erase(it);
        } else {
            ++it;
        }
    }

    return expiredUsers;
}