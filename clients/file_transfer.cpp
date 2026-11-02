#include "../clients/file_transfer.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <system_error>
#include <utility>

namespace {

constexpr std::size_t UDP_CHUNK_SIZE = 1200;
constexpr int MAX_CHUNK_RETRIES = 5;
constexpr auto ACK_TIMEOUT = std::chrono::seconds(1);
constexpr auto NEGOTIATION_TIMEOUT = std::chrono::seconds(10);
constexpr std::uint64_t MAX_FILE_SIZE = 1024ull * 1024ull * 1024ull;

std::uint32_t createTransferId() {
    static std::mutex generatorMutex;
    static std::mt19937 generator(std::random_device{}());
    static std::uniform_int_distribution<std::uint32_t> distribution(
        1,
        std::numeric_limits<std::uint32_t>::max()
    );

    std::lock_guard<std::mutex> lock(generatorMutex);
    return distribution(generator);
}

bool sameEndpoint(
    const sockaddr_in& left,
    const sockaddr_in& right
) {
    return left.sin_family == right.sin_family &&
        left.sin_addr.s_addr == right.sin_addr.s_addr &&
        left.sin_port == right.sin_port;
}

} // namespace

FileTransfer::FileTransfer()
    : udpSocket(INVALID_SOCKET),
      running(false),
      winsockInitialized(false),
      receiveThread(),
      sendThreads(),
      localPort(0) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::cerr << "Failed to initialize Winsock for UDP transfer\n";
    } else {
        winsockInitialized = true;
    }
}

FileTransfer::~FileTransfer() {
    stop();
    if (winsockInitialized) {
        WSACleanup();
    }
}

bool FileTransfer::initializeUdpSocket(std::uint16_t port) {
    if (!winsockInitialized) {
        return false;
    }
    if (running.load() && udpSocket.load() != INVALID_SOCKET) {
        return port == 0 || port == localPort;
    }

    const SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == INVALID_SOCKET) {
        std::cerr << "Failed to create UDP socket: "
                  << WSAGetLastError() << '\n';
        return false;
    }

    sockaddr_in localAddress{};
    localAddress.sin_family = AF_INET;
    localAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    localAddress.sin_port = htons(port);
    if (bind(
            socket,
            reinterpret_cast<sockaddr*>(&localAddress),
            sizeof(localAddress)
        ) == SOCKET_ERROR) {
        std::cerr << "Failed to bind UDP socket: "
                  << WSAGetLastError() << '\n';
        closesocket(socket);
        return false;
    }

    int addressLength = sizeof(localAddress);
    if (getsockname(
            socket,
            reinterpret_cast<sockaddr*>(&localAddress),
            &addressLength
        ) == SOCKET_ERROR) {
        std::cerr << "Failed to determine bound UDP port: "
                  << WSAGetLastError() << '\n';
        closesocket(socket);
        return false;
    }

    udpSocket.store(socket);
    localPort = ntohs(localAddress.sin_port);
    running.store(true);
    try {
        receiveThread = std::thread(&FileTransfer::receiveLoop, this);
    } catch (const std::system_error& error) {
        running.store(false);
        udpSocket.store(INVALID_SOCKET);
        closesocket(socket);
        localPort = 0;
        std::cerr << "Failed to start UDP receive thread: "
                  << error.what() << '\n';
        return false;
    }

    return true;
}

std::uint16_t FileTransfer::getLocalPort() const {
    return localPort;
}

void FileTransfer::configureSession(
    const std::string& username,
    std::function<bool(const Message&)> sendMessage,
    std::function<std::optional<Message>(
        std::uint32_t,
        std::chrono::milliseconds
    )> waitForResponse
) {
    authenticatedUsername = username;
    sendTcpMessage = std::move(sendMessage);
    waitForTcpResponse = std::move(waitForResponse);
}

bool FileTransfer::startSendFile(
    const std::string& recipient,
    const std::filesystem::path& filePath,
    const sockaddr_in& recipientAddress
) {
    if (!running.load() || udpSocket.load() == INVALID_SOCKET ||
        authenticatedUsername.empty() || !sendTcpMessage ||
        !waitForTcpResponse || recipient.empty() ||
        recipient.size() > 64) {
        return false;
    }

    std::error_code fileError;
    if (!std::filesystem::is_regular_file(filePath, fileError) ||
        fileError) {
        return false;
    }

    const std::uintmax_t size = std::filesystem::file_size(filePath, fileError);
    if (fileError || size > MAX_FILE_SIZE) {
        return false;
    }

    auto input = std::make_shared<std::ifstream>(
        filePath,
        std::ios::binary
    );
    if (!input->is_open()) {
        return false;
    }

    const std::string filename = filePath.filename().string();
    if (filename.empty() || filename.size() > 255) {
        return false;
    }

    std::uint32_t transferId = createTransferId();
    std::shared_ptr<FileTransferState> state;
    {
        std::lock_guard<std::mutex> lock(transferMutex);
        while (activeTransfers.find(transferId) != activeTransfers.end()) {
            transferId = createTransferId();
        }

        state = std::make_shared<FileTransferState>();
        state->transferId = transferId;
        state->senderUsername = authenticatedUsername;
        state->recipientUsername = recipient;
        state->filename = filename;
        state->totalSize = static_cast<std::uint64_t>(size);
        state->bytesTransferred = 0;
        state->nextSequenceNumber = 0;
        state->lastAcknowledgedSequence =
            std::numeric_limits<std::uint32_t>::max();
        state->status = TransferStatus::PENDING;
        state->filePath = filePath;
        state->peerAddress = recipientAddress;
        state->sourceFile = std::move(input);
        activeTransfers.emplace(transferId, state);
    }

    Message offer;
    offer.type = MessageType::FILE_START;
    offer.payload = {
        {"transfer_id", transferId},
        {"recipient", recipient},
        {"filename", filename},
        {"total_size", state->totalSize},
        {"udp_port", localPort}
    };

    if (!sendTcpMessage(offer)) {
        cancelTransfer(transferId);
        return false;
    }

    const auto response = waitForTcpResponse(
        transferId,
        NEGOTIATION_TIMEOUT
    );
    if (!response.has_value() ||
        !response->payload.contains("transfer_id") ||
        !response->payload["transfer_id"].is_number_unsigned() ||
        response->payload["transfer_id"].get<std::uint64_t>() != transferId) {
        cancelTransfer(transferId);
        return false;
    }

    if (response->type != MessageType::FILE_ACCEPT) {
        cancelTransfer(transferId);
        return false;
    }

    if (!response->payload.contains("recipient_ip") ||
        !response->payload["recipient_ip"].is_string() ||
        !response->payload.contains("recipient_udp_port") ||
        !response->payload["recipient_udp_port"].is_number_unsigned()) {
        cancelTransfer(transferId);
        return false;
    }

    sockaddr_in peer = recipientAddress;
    const std::string peerIp =
        response->payload["recipient_ip"].get<std::string>();
    const std::uint64_t peerPort =
        response->payload["recipient_udp_port"].get<std::uint64_t>();
    if (peerPort == 0 || peerPort > std::numeric_limits<std::uint16_t>::max() ||
        InetPtonA(AF_INET, peerIp.c_str(), &peer.sin_addr) != 1) {
        cancelTransfer(transferId);
        return false;
    }
    peer.sin_family = AF_INET;
    peer.sin_port = htons(static_cast<std::uint16_t>(peerPort));

    {
        std::lock_guard<std::mutex> lock(transferMutex);
        const auto found = activeTransfers.find(transferId);
        if (found == activeTransfers.end()) {
            return false;
        }
        found->second->peerAddress = peer;
        found->second->status = TransferStatus::IN_PROGRESS;

        try {
            sendThreads.emplace_back([this, transferId]() {
                while (sendNextChunk(transferId)) {
                }
            });
        } catch (const std::system_error& error) {
            found->second->status = TransferStatus::FAILED;
            std::cerr << "Failed to start file transfer worker: "
                      << error.what() << '\n';
            return false;
        }
    }

    return true;
}

bool FileTransfer::startSendFile(
    const std::string& recipient,
    const std::filesystem::path& filePath
) {
    sockaddr_in unknownPeer{};
    return startSendFile(recipient, filePath, unknownPeer);
}

bool FileTransfer::receiveFileStart(
    const UdpPacket&,
    const sockaddr_in&
) {
    return false;
}

bool FileTransfer::sendNextChunk(std::uint32_t transferId) {
    std::shared_ptr<FileTransferState> state;
    {
        std::lock_guard<std::mutex> lock(transferMutex);
        const auto found = activeTransfers.find(transferId);
        if (found == activeTransfers.end() ||
            found->second->status != TransferStatus::IN_PROGRESS) {
            return false;
        }
        state = found->second;
    }

    std::uint64_t bytesTransferred = 0;
    std::uint32_t sequenceNumber = 0;
    {
        std::lock_guard<std::mutex> lock(state->acknowledgementMutex);
        bytesTransferred = state->bytesTransferred;
        sequenceNumber = state->nextSequenceNumber;
    }

    if (bytesTransferred >= state->totalSize) {
        UdpPacket finish{};
        finish.type = UdpPacketType::FILE_FIN;
        finish.transferId = transferId;
        finish.sequenceNumber = sequenceNumber;
        finish.payloadLength = 0;
        const std::vector<std::uint8_t> encoded = encodeUdpPacket(finish);
        const SOCKET socket = udpSocket.load();
        bool finishAcknowledged = false;
        for (int attempt = 0;
             attempt < MAX_CHUNK_RETRIES && running.load();
             ++attempt) {
            {
                std::lock_guard<std::mutex> lock(
                    state->acknowledgementMutex
                );
                state->finishAcknowledged = false;
            }
            if (socket == INVALID_SOCKET ||
                sendto(
                    socket,
                    reinterpret_cast<const char*>(encoded.data()),
                    static_cast<int>(encoded.size()),
                    0,
                    reinterpret_cast<const sockaddr*>(&state->peerAddress),
                    sizeof(state->peerAddress)
                ) == SOCKET_ERROR) {
                cancelTransfer(transferId);
                return false;
            }

            std::unique_lock<std::mutex> lock(
                state->acknowledgementMutex
            );
            finishAcknowledged = state->acknowledgementChanged.wait_for(
                lock,
                ACK_TIMEOUT,
                [&state]() {
                    return state->finishAcknowledged;
                }
            );
            if (finishAcknowledged) {
                break;
            }
        }
        if (finishAcknowledged) {
            finishTransfer(transferId);
        } else {
            cancelTransfer(transferId);
        }
        return false;
    }

    UdpPacket packet{};
    packet.type = UdpPacketType::FILE_DATA;
    packet.transferId = transferId;
    packet.sequenceNumber = sequenceNumber;
    packet.payload.resize(UDP_CHUNK_SIZE);
    state->sourceFile->read(
        reinterpret_cast<char*>(packet.payload.data()),
        static_cast<std::streamsize>(packet.payload.size())
    );
    const std::streamsize bytesRead = state->sourceFile->gcount();
    if (bytesRead <= 0) {
        cancelTransfer(transferId);
        return false;
    }
    packet.payload.resize(static_cast<std::size_t>(bytesRead));
    packet.payloadLength = static_cast<std::uint16_t>(bytesRead);
    const std::vector<std::uint8_t> encoded = encodeUdpPacket(packet);
    if (encoded.empty()) {
        cancelTransfer(transferId);
        return false;
    }

    const SOCKET socket = udpSocket.load();
    bool acknowledged = false;
    for (int attempt = 0; attempt < MAX_CHUNK_RETRIES && running.load(); ++attempt) {
        {
            std::lock_guard<std::mutex> lock(state->acknowledgementMutex);
            state->acknowledgementReceived = false;
        }
        if (socket == INVALID_SOCKET ||
            sendto(
                socket,
                reinterpret_cast<const char*>(encoded.data()),
                static_cast<int>(encoded.size()),
                0,
                reinterpret_cast<const sockaddr*>(&state->peerAddress),
                sizeof(state->peerAddress)
            ) == SOCKET_ERROR) {
            cancelTransfer(transferId);
            return false;
        }
        std::unique_lock<std::mutex> lock(state->acknowledgementMutex);
        acknowledged = state->acknowledgementChanged.wait_for(
            lock,
            ACK_TIMEOUT,
            [&state, &packet]() {
                return state->acknowledgementReceived &&
                    state->lastAcknowledgedSequence ==
                        packet.sequenceNumber;
            }
        );
        if (acknowledged) {
            break;
        }
    }

    if (!acknowledged) {
        cancelTransfer(transferId);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state->acknowledgementMutex);
        state->bytesTransferred += packet.payload.size();
        ++state->nextSequenceNumber;
    }
    return true;
}

bool FileTransfer::processAcknowledgement(const UdpPacket& packet) {
    std::shared_ptr<FileTransferState> state;
    {
        std::lock_guard<std::mutex> lock(transferMutex);
        const auto found = activeTransfers.find(packet.transferId);
        if (found == activeTransfers.end() ||
            found->second->status != TransferStatus::IN_PROGRESS) {
            return false;
        }
        state = found->second;
    }

    if (packet.type == UdpPacketType::FILE_FIN_ACK) {
        {
            std::lock_guard<std::mutex> lock(state->acknowledgementMutex);
            state->finishAcknowledged = true;
        }
        state->acknowledgementChanged.notify_all();
        return true;
    }

    if (packet.type != UdpPacketType::FILE_ACK) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(state->acknowledgementMutex);
        if (packet.sequenceNumber != state->nextSequenceNumber) {
            return false;
        }
        state->lastAcknowledgedSequence = packet.sequenceNumber;
        state->acknowledgementReceived = true;
    }
    state->acknowledgementChanged.notify_all();
    return true;
}

bool FileTransfer::receiveChunk(
    const UdpPacket&,
    const sockaddr_in&
) {
    return false;
}

bool FileTransfer::sendAcknowledgement(
    const UdpPacket&,
    const sockaddr_in&
) {
    return false;
}

bool FileTransfer::finishTransfer(std::uint32_t transferId) {
    std::lock_guard<std::mutex> lock(transferMutex);
    const auto found = activeTransfers.find(transferId);
    if (found == activeTransfers.end()) {
        return false;
    }

    found->second->status = TransferStatus::COMPLETED;
    if (found->second->sourceFile && found->second->sourceFile->is_open()) {
        found->second->sourceFile->close();
    }
    found->second->acknowledgementChanged.notify_all();
    return true;
}

bool FileTransfer::cancelTransfer(std::uint32_t transferId) {
    std::shared_ptr<FileTransferState> state;
    {
        std::lock_guard<std::mutex> lock(transferMutex);
        const auto found = activeTransfers.find(transferId);
        if (found == activeTransfers.end()) {
            return false;
        }
        state = found->second;
        if (state->status == TransferStatus::COMPLETED) {
            return false;
        }
        state->status = TransferStatus::FAILED;
    }

    {
        std::lock_guard<std::mutex> lock(state->acknowledgementMutex);
        state->acknowledgementReceived = true;
    }
    state->acknowledgementChanged.notify_all();
    if (state->sourceFile && state->sourceFile->is_open()) {
        state->sourceFile->close();
    }

    const SOCKET socket = udpSocket.load();
    if (socket != INVALID_SOCKET && state->peerAddress.sin_port != 0) {
        UdpPacket abort{};
        abort.type = UdpPacketType::FILE_ABORT;
        abort.transferId = transferId;
        abort.sequenceNumber = state->nextSequenceNumber;
        abort.payloadLength = 0;
        const std::vector<std::uint8_t> encoded = encodeUdpPacket(abort);
        sendto(
            socket,
            reinterpret_cast<const char*>(encoded.data()),
            static_cast<int>(encoded.size()),
            0,
            reinterpret_cast<const sockaddr*>(&state->peerAddress),
            sizeof(state->peerAddress)
        );
    }
    return true;
}

double FileTransfer::calculateProgress(std::uint32_t transferId) {
    std::shared_ptr<FileTransferState> state;
    bool completed = false;
    {
        std::lock_guard<std::mutex> lock(transferMutex);
        const auto found = activeTransfers.find(transferId);
        if (found == activeTransfers.end()) {
            return 0.0;
        }
        state = found->second;
        completed = state->status == TransferStatus::COMPLETED;
    }

    std::lock_guard<std::mutex> lock(state->acknowledgementMutex);
    if (state->totalSize == 0) {
        return completed ? 100.0 : 0.0;
    }
    return 100.0 * static_cast<double>(state->bytesTransferred) /
        static_cast<double>(state->totalSize);
}

void FileTransfer::receiveLoop() {
    std::array<std::uint8_t, 2048> buffer{};
    while (running.load()) {
        sockaddr_in senderAddress{};
        int senderLength = sizeof(senderAddress);
        const SOCKET socket = udpSocket.load();
        if (socket == INVALID_SOCKET) {
            break;
        }

        const int received = recvfrom(
            socket,
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&senderAddress),
            &senderLength
        );
        if (received == SOCKET_ERROR) {
            if (running.load()) {
                std::cerr << "UDP receive failed: "
                          << WSAGetLastError() << '\n';
            }
            break;
        }

        UdpPacket packet{};
        if (!decodeUdpPacket(
                buffer.data(),
                static_cast<std::size_t>(received),
                packet
            )) {
            continue;
        }

        std::shared_ptr<FileTransferState> state;
        {
            std::lock_guard<std::mutex> lock(transferMutex);
            const auto found = activeTransfers.find(packet.transferId);
            if (found == activeTransfers.end()) {
                continue;
            }
            state = found->second;
        }

        sockaddr_in peerAddress{};
        {
            std::lock_guard<std::mutex> lock(transferMutex);
            const auto found = activeTransfers.find(packet.transferId);
            if (found == activeTransfers.end()) {
                continue;
            }
            peerAddress = found->second->peerAddress;
        }

        if (!sameEndpoint(senderAddress, peerAddress)) {
            continue;
        }

        if (packet.type == UdpPacketType::FILE_ACK ||
            packet.type == UdpPacketType::FILE_FIN_ACK) {
            processAcknowledgement(packet);
        }
    }
}

void FileTransfer::stop() {
    running.store(false);
    const SOCKET socket = udpSocket.exchange(INVALID_SOCKET);
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }

    if (receiveThread.joinable()) {
        receiveThread.join();
    }

    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(transferMutex);
        workers.swap(sendThreads);
        for (auto& entry : activeTransfers) {
            if (entry.second->status == TransferStatus::IN_PROGRESS ||
                entry.second->status == TransferStatus::PENDING) {
                entry.second->status = TransferStatus::CANCELLED;
                entry.second->acknowledgementChanged.notify_all();
            }
        }
    }
    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    localPort = 0;
}
