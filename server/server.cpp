#include "../server/server.h"

#include "../server/client_session.h"
#include "../server/user_manager.h"
#include "../server/message_router.h"

#include <iostream>
#include <utility>

using namespace std;

Server::Server(int port)
    : listeningSocket(INVALID_SOCKET),
      serverPort(port),
      running(false),
      userManager(new UserManager()),
      messageRouter(new MessageRouter(userManager)) {
}

Server::~Server() {
    stop();

    delete messageRouter;
    messageRouter = nullptr;

    delete userManager;
    userManager = nullptr;

    WSACleanup();
}

bool Server::initialize() {
    WSADATA wsaData;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cerr << "WSAStartup failed.\n";
        return false;
    }

    listeningSocket = socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP
    );

    if (listeningSocket == INVALID_SOCKET) {
        cerr << "Failed to create listening socket. Error: "
             << WSAGetLastError() << '\n';

        WSACleanup();
        return false;
    }

    sockaddr_in serverAddress{};

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(
        static_cast<u_short>(serverPort)
    );
    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(
            listeningSocket,
            reinterpret_cast<sockaddr*>(&serverAddress),
            sizeof(serverAddress)
        ) == SOCKET_ERROR) {

        cerr << "Bind failed. Error: "
             << WSAGetLastError() << '\n';

        closesocket(listeningSocket);
        listeningSocket = INVALID_SOCKET;

        WSACleanup();
        return false;
    }

    if (listen(listeningSocket, SOMAXCONN) == SOCKET_ERROR) {
        cerr << "Listen failed. Error: "
             << WSAGetLastError() << '\n';

        closesocket(listeningSocket);
        listeningSocket = INVALID_SOCKET;

        WSACleanup();
        return false;
    }

    running = true;

    cout << "Server listening on TCP port "
         << serverPort << '\n';

    return true;
}

void Server::startListening() {
    if (running) {
        cerr << "Server is already running.\n";
        return;
    }

    if (!initialize()) {
        return;
    }

    acceptConnections();
}

void Server::acceptConnections() {
    while (running) {
        sockaddr_in clientAddress{};
        int addressLength = sizeof(clientAddress);

        SOCKET clientSocket = accept(
            listeningSocket,
            reinterpret_cast<sockaddr*>(&clientAddress),
            &addressLength
        );

        if (clientSocket == INVALID_SOCKET) {
            if (!running) {
                break;
            }

            cerr << "Accept failed. Error: "
                 << WSAGetLastError() << '\n';

            continue;
        }

        handleNewClient(clientSocket, clientAddress);
    }
}

void Server::handleNewClient(
    SOCKET clientSocket,
    const sockaddr_in& clientAddress
) {
    if (!running) {
        closesocket(clientSocket);
        return;
    }

    auto session = make_shared<ClientSession>(
        clientSocket,
        clientAddress,
        userManager,
        messageRouter
    );

    {
        lock_guard<mutex> lock(sessionsMutex);

        sessions.push_back(session);

        workerThreads.emplace_back(
            [session]() {
                session->run();
            }
        );
    }

    cout << "New client connected from "
         << inet_ntoa(clientAddress.sin_addr)
         << ':' << ntohs(clientAddress.sin_port)
         << '\n';
}

void Server::stop() {
    if (!running.exchange(false)) {
        return;
    }

    if (listeningSocket != INVALID_SOCKET) {
        shutdown(listeningSocket, SD_BOTH);
        closesocket(listeningSocket);
        listeningSocket = INVALID_SOCKET;
    }

    {
        lock_guard<mutex> lock(sessionsMutex);

        for (const auto& session : sessions) {
            if (session) {
                session->disconnect();
            }
        }
    }

    for (auto& worker : workerThreads) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    workerThreads.clear();

    {
        lock_guard<mutex> lock(sessionsMutex);
        sessions.clear();
    }

    cout << "Server stopped.\n";
}