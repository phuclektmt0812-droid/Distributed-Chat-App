#pragma once

#include <winsock2.h>
#include <string>
#include <mutex>
#include <cstdint>
#include <atomic>

#include "../protocol/message.h"
using namespace std;
class Connection {
private:
    std::atomic<SOCKET> socketHandle;

    std::atomic<bool> connected;

public:
    string serverAddress;
    uint16_t serverPort;

    mutex sendMutex;
    mutex receiveMutex;

public:
    Connection(
        const string& address,
        uint16_t port
    ) {
        serverAddress = address;
        serverPort = port;
        connected.store(false);
        socketHandle.store(INVALID_SOCKET);
    }

    ~Connection();

    bool connectToServer(const string& address,
        uint16_t port);

    bool sendFramedMessage(const Message& message);

    bool receiveFramedMessage(Message& message);

    void disconnect();

    bool isConnected() const {
        return connected.load();
    }
};