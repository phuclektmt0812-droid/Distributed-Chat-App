#pragma once

#include <winsock2.h>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <memory>
#include <deque>
#include <stdexcept>
using namespace std;

#include "../clients/client.h"

class HttpBridge {
private:
    std::atomic<SOCKET> listeningSocket;

    uint16_t httpPort;

    atomic<bool> running;

    ChatClient chatClient;
    string serverAddress;
    uint16_t serverPort;

    mutex sessionMutex;
    bool sessionAuthenticated;
    string sessionUsername;
    string lastConnectionError;
    nlohmann::json latestUserList = nlohmann::json::array();
    deque<ClientEvent> pendingEvents;
    bool winsockInitialized;

    thread acceptThread;

    vector<thread> httpWorkerThreads;

    mutex workersMutex;

public:
    HttpBridge(
        const string& serverAddress,
        uint16_t serverPort,
        uint16_t httpPort
    );

    ~HttpBridge();

    bool start();

    void acceptHttpConnections();

    void handleHttpConnection(SOCKET clientSocket);

    void handleHttpRequest(
        SOCKET clientSocket,
        const string& request
    );

    void handleLogin(
        SOCKET clientSocket,
        const string& requestBody
    );

    void handleLogout(SOCKET clientSocket);

    void handleGlobalMessage(
        SOCKET clientSocket,
        const string& requestBody
    );

    void handlePrivateMessage(
        SOCKET clientSocket,
        const string& requestBody
    );

    void handleUserListRequest(SOCKET clientSocket);

    void handleEventsRequest(SOCKET clientSocket);

    void handleStatusRequest(SOCKET clientSocket);

    void handleFileUpload(
        SOCKET clientSocket,
        const string& requestBody
    );

    void handleFileDownload(
        SOCKET clientSocket,
        const string& transferId
    );

    void sendHttpResponse(
        SOCKET clientSocket,
        int statusCode,
        const string& contentType,
        const string& body
    );

    void stop();
};