#include "../clients/connection.h"

#include <ws2tcpip.h>
#include <climits>
#include <cstdint>
#include <limits>
#include <string>

namespace {
    constexpr std::uint32_t MAX_MESSAGE_SIZE = 1024 * 1024;

    bool sendAll(
        SOCKET socketHandle,
        const char* data,
        std::size_t length
    ) {
        std::size_t totalSent = 0;

        while (totalSent < length) {
            const std::size_t remaining = length - totalSent;

            const int chunkSize = static_cast<int>(
                remaining > static_cast<std::size_t>(INT_MAX)
                    ? INT_MAX
                    : remaining
            );

            const int sent = ::send(
                socketHandle,
                data + totalSent,
                chunkSize,
                0
            );

            if (sent == SOCKET_ERROR || sent == 0) {
                return false;
            }

            totalSent += static_cast<std::size_t>(sent);
        }

        return true;
    }

    bool receiveAll(
        SOCKET socketHandle,
        char* buffer,
        std::size_t length
    ) {
        std::size_t totalReceived = 0;

        while (totalReceived < length) {
            const std::size_t remaining = length - totalReceived;

            const int chunkSize = static_cast<int>(
                remaining > static_cast<std::size_t>(INT_MAX)
                    ? INT_MAX
                    : remaining
            );

            const int received = ::recv(
                socketHandle,
                buffer + totalReceived,
                chunkSize,
                0
            );

            if (received == SOCKET_ERROR || received == 0) {
                return false;
            }

            totalReceived += static_cast<std::size_t>(received);
        }

        return true;
    }
}

Connection::~Connection() {
    disconnect();
}

bool Connection::connectToServer(
    const string& address,
    uint16_t port
) {
    if (address.empty() || port == 0) {
        return false;
    }

    if (connected.load() &&
        socketHandle.load() != INVALID_SOCKET) {
        return true;
    }

    serverAddress = address;
    serverPort = port;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const string portString = std::to_string(port);

    addrinfo* addresses = nullptr;

    const int resolveResult = getaddrinfo(
        address.c_str(),
        portString.c_str(),
        &hints,
        &addresses
    );

    if (resolveResult != 0 || addresses == nullptr) {
        connected.store(false);
        return false;
    }

    SOCKET newSocket = INVALID_SOCKET;

    for (
        addrinfo* current = addresses;
        current != nullptr;
        current = current->ai_next
    ) {
        newSocket = ::socket(
            current->ai_family,
            current->ai_socktype,
            current->ai_protocol
        );

        if (newSocket == INVALID_SOCKET) {
            continue;
        }

        if (::connect(
                newSocket,
                current->ai_addr,
                static_cast<int>(current->ai_addrlen)
            ) != SOCKET_ERROR) {
            break;
        }

        closesocket(newSocket);
        newSocket = INVALID_SOCKET;
    }

    freeaddrinfo(addresses);

    if (newSocket == INVALID_SOCKET) {
        connected.store(false);
        socketHandle.store(INVALID_SOCKET);
        return false;
    }

    socketHandle.store(newSocket);
    connected.store(true);

    return true;
}

bool Connection::sendFramedMessage(const Message& message) {
    std::lock_guard<std::mutex> lock(sendMutex);

    const SOCKET socket = socketHandle.load();

    if (!connected.load() || socket == INVALID_SOCKET) {
        return false;
    }

    const string serialized = serializeMessage(message);

    if (serialized.empty() ||
        serialized.size() > MAX_MESSAGE_SIZE) {
        return false;
    }

    const std::uint32_t messageLength =
        static_cast<std::uint32_t>(serialized.size());

    const std::uint32_t networkLength = htonl(messageLength);

    const bool headerSent = sendAll(
        socket,
        reinterpret_cast<const char*>(&networkLength),
        sizeof(networkLength)
    );

    const bool bodySent = headerSent &&
        sendAll(
            socket,
            serialized.data(),
            serialized.size()
        );

    if (!headerSent || !bodySent) {
        connected.store(false);

        const SOCKET failedSocket =
            socketHandle.exchange(INVALID_SOCKET);

        if (failedSocket != INVALID_SOCKET) {
            shutdown(failedSocket, SD_BOTH);
            closesocket(failedSocket);
        }

        return false;
    }

    return true;
}

bool Connection::receiveFramedMessage(Message& message) {
    std::lock_guard<std::mutex> lock(receiveMutex);

    const SOCKET socket = socketHandle.load();

    if (!connected.load() || socket == INVALID_SOCKET) {
        return false;
    }

    std::uint32_t networkLength = 0;

    if (!receiveAll(
            socket,
            reinterpret_cast<char*>(&networkLength),
            sizeof(networkLength)
        )) {
        connected.store(false);

        const SOCKET failedSocket =
            socketHandle.exchange(INVALID_SOCKET);

        if (failedSocket != INVALID_SOCKET) {
            shutdown(failedSocket, SD_BOTH);
            closesocket(failedSocket);
        }

        return false;
    }

    const std::uint32_t messageLength = ntohl(networkLength);

    if (messageLength == 0 ||
        messageLength > MAX_MESSAGE_SIZE) {
        connected.store(false);

        const SOCKET failedSocket =
            socketHandle.exchange(INVALID_SOCKET);

        if (failedSocket != INVALID_SOCKET) {
            shutdown(failedSocket, SD_BOTH);
            closesocket(failedSocket);
        }

        return false;
    }

    string serialized(messageLength, '\0');

    if (!receiveAll(
            socket,
            serialized.data(),
            serialized.size()
        )) {
        connected.store(false);

        const SOCKET failedSocket =
            socketHandle.exchange(INVALID_SOCKET);

        if (failedSocket != INVALID_SOCKET) {
            shutdown(failedSocket, SD_BOTH);
            closesocket(failedSocket);
        }

        return false;
    }

    if (!deserializeMessage(serialized, message)) {
        connected.store(false);

        const SOCKET failedSocket =
            socketHandle.exchange(INVALID_SOCKET);

        if (failedSocket != INVALID_SOCKET) {
            shutdown(failedSocket, SD_BOTH);
            closesocket(failedSocket);
        }

        return false;
    }

    return true;
}

void Connection::disconnect() {
    connected.store(false);

    const SOCKET socket = socketHandle.exchange(INVALID_SOCKET);

    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
}