#pragma once

#include <winsock2.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <functional>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <optional>

#include "../protocol/udp_packet.h"
#include "../protocol/message.h"

enum class TransferStatus {
    PENDING,
    IN_PROGRESS,
    COMPLETED,
    FAILED,
    CANCELLED
};

struct FileTransferState {
    std::uint32_t transferId;

    std::string senderUsername;
    std::string recipientUsername;
    std::string filename;

    std::uint64_t totalSize;
    std::uint64_t bytesTransferred;

    std::uint32_t nextSequenceNumber;
    std::uint32_t lastAcknowledgedSequence;

    TransferStatus status;

    std::filesystem::path filePath;

    sockaddr_in peerAddress;

    std::shared_ptr<std::ifstream> sourceFile;
    std::mutex acknowledgementMutex;
    std::condition_variable acknowledgementChanged;
    bool acknowledgementReceived = false;
    bool finishAcknowledged = false;
};

class FileTransfer {
private:
    std::atomic<SOCKET> udpSocket;

    std::unordered_map<
        std::uint32_t,
        std::shared_ptr<FileTransferState>
    > activeTransfers;

    std::mutex transferMutex;

    std::atomic<bool> running;
    bool winsockInitialized;
    std::thread receiveThread;
    std::vector<std::thread> sendThreads;
    std::uint16_t localPort;
    std::string authenticatedUsername;
    std::function<bool(const Message&)> sendTcpMessage;
    std::function<std::optional<Message>(
        std::uint32_t,
        std::chrono::milliseconds
    )> waitForTcpResponse;

public:
    FileTransfer();
    ~FileTransfer();

    bool initializeUdpSocket(std::uint16_t port);
    std::uint16_t getLocalPort() const;

    void configureSession(
        const std::string& username,
        std::function<bool(const Message&)> sendMessage,
        std::function<std::optional<Message>(
            std::uint32_t,
            std::chrono::milliseconds
        )> waitForResponse
    );

    bool startSendFile(
        const std::string& recipient,
        const std::filesystem::path& filePath,
        const sockaddr_in& recipientAddress
    );

    bool startSendFile(
        const std::string& recipient,
        const std::filesystem::path& filePath
    );

    bool receiveFileStart(
        const UdpPacket& packet,
        const sockaddr_in& senderAddress
    );

    bool sendNextChunk(std::uint32_t transferId);

    bool processAcknowledgement(const UdpPacket& packet);

    bool receiveChunk(
        const UdpPacket& packet,
        const sockaddr_in& senderAddress
    );

    bool sendAcknowledgement(
        const UdpPacket& packet,
        const sockaddr_in& recipientAddress
    );

    bool finishTransfer(std::uint32_t transferId);

    bool cancelTransfer(std::uint32_t transferId);

    double calculateProgress(std::uint32_t transferId);

    void receiveLoop();

    void stop();
};