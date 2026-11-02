#include "../protocol/protocol.h"
#pragma once
#include "user_manager.h"
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class TcpSession;
class UdpService;
class FileRelay;

class Server {
public:
    Server();
    ~Server();
    bool start();
    void stop();
    UserManager& users() { return users_; }
    FileRelay& fileRelay();
    void broadcastUserList();
    void broadcastMessage(const std::shared_ptr<ClientSession>& sender,
                          std::uint32_t clientId, const std::string& message,
                          std::uint32_t requestSequence);
    bool sendTo(const std::shared_ptr<ClientSession>& session,
                const protocol::Packet& packet);
    void removeSession(const std::shared_ptr<ClientSession>& session);
    bool running() const { return running_; }
    SocketHandle udpSocket() const { return udpSocket_; }
    const protocol::EncryptionKey& encryptionKey() const { return encryptionKey_; }
    bool sendUdpTo(const protocol::Packet& packet,const sockaddr_in& destination);
private:
    void acceptLoop();
    void udpLoop();
    void maintenanceLoop();
    UserManager users_;
    protocol::EncryptionKey encryptionKey_{};
    SocketHandle tcpListener_=INVALID_SOCKET_HANDLE;
    SocketHandle udpSocket_=INVALID_SOCKET_HANDLE;
    std::atomic<bool> running_{false};
    std::thread acceptThread_, udpThread_, maintenanceThread_;
    std::mutex workersMutex_;
    std::vector<std::thread> workers_;
    std::atomic<std::uint32_t> serverMessageId_{1};
    std::unique_ptr<UdpService> udpService_;
    std::unique_ptr<FileRelay> fileRelay_;
};
