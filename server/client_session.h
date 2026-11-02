#pragma once

#include <winsock2.h>
#include <string>
#include <mutex>
#include <atomic>
#include <memory>

#include "../protocol/message.h"
using namespace std;

class Server;
class UserManager;
class MessageRouter;

class ClientSession
    : public std::enable_shared_from_this<ClientSession> {
private:
    SOCKET clientSocket;
    sockaddr_in clientAddress;

    string username;

    atomic<bool> authenticated;
    atomic<bool> connected;

    mutex sendMutex;

    UserManager* userManager;
    MessageRouter* messageRouter;

public:
    ClientSession(
        SOCKET socket,
        const sockaddr_in& address,
        UserManager* users,
        MessageRouter* router
    ) {
        this->clientSocket = socket;
        this->clientAddress = address;
        this-> userManager = users;
        this->messageRouter = router;
    }

    ~ClientSession();

    void run();

    bool receiveMessage(Message& message);

    bool processMessage(const Message& message);

    bool sendMessage(const Message& message);

    bool login(const Message& message);

    void logout();

    void disconnect();

    string getUsername() const {
        return this->username;
    }

    bool isAuthenticated() const {
        return this->authenticated;
    }

    bool isConnected() const {
        return this->connected;
    }

    SOCKET getSocket() const {
        return this->clientSocket;
    }

    sockaddr_in getAddress() const {
        return this->clientAddress;
    }
};