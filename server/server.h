#pragma once

#include <winsock2.h>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
using namespace std;
class ClientSession;
class UserManager;
class MessageRouter;

class Server {
private:
    SOCKET listeningSocket;
    int serverPort;
    atomic<bool> running;

    vector<shared_ptr<ClientSession>> sessions;
    vector<thread> workerThreads;

    mutex sessionsMutex;

    UserManager* userManager;
    MessageRouter* messageRouter;

public:
    explicit Server(int port);
    ~Server();

    bool initialize();

    void startListening();

    void acceptConnections();

    void handleNewClient(
        SOCKET clientSocket,
        const sockaddr_in& clientAddress
    );

    void stop();
};