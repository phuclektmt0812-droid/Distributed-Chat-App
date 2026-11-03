#pragma once

#include "../platform/socket_compat.h"
#include "../protocol/message_payloads.h"
#include "../protocol/secure_payload.h"
#include "../protocol/file_messages.h"
#include "file_sender.h"
#include "file_receiver.h"

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct AgentEvent {
    std::string type;
    nlohmann::json data;
};

class DcapClient {
public:
    DcapClient(std::string server_address, std::uint16_t server_port = 54000,
               std::uint16_t udp_port = 54001);
    ~DcapClient();

    bool connect();
    bool login(const std::string& username);
    void logout();
    void disconnect();

    bool connected() const { return connected_.load(); }
    bool authenticated() const { return authenticated_.load(); }
    std::string username() const;
    std::vector<std::string> users() const;

    bool sendGlobal(const std::string& text);
    bool sendPrivate(const std::string& recipient, const std::string& text);
    std::string sendFile(const std::string& recipient, const std::filesystem::path& path);
    bool decideFile(const std::string& transfer_id_hex, bool accept);

    std::vector<AgentEvent> takeEvents();
    std::string lastError() const;

private:
    bool sendTcp(protocol::Packet packet);
    bool sendUdp(protocol::Packet packet);
    void tcpLoop();
    void udpLoop();
    void maintenanceLoop();
    void handleTcp(const protocol::Packet& packet);
    void handleUdp(const protocol::Packet& packet);
    void emit(std::string type, nlohmann::json data = nlohmann::json::object());
    static std::string timestamp(std::uint64_t milliseconds);

    std::string server_address_;
    std::uint16_t server_port_;
    std::uint16_t udp_port_;
    sockaddr_in server_udp_{};
    protocol::EncryptionKey encryption_key_{};

    SocketHandle tcp_socket_ = INVALID_SOCKET_HANDLE;
    SocketHandle udp_socket_ = INVALID_SOCKET_HANDLE;
    std::atomic<bool> connected_{false};
    std::atomic<bool> authenticated_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> udp_registered_{false};
    std::atomic<bool> socket_runtime_initialized_{false};

    mutable std::mutex state_mutex_;
    std::string username_;
    std::string last_error_;
    std::vector<std::string> users_;
    std::array<std::uint8_t,32> udp_token_{};

    protocol::SequenceCounter tcp_sequence_;
    protocol::SequenceCounter udp_sequence_;
    std::mutex tcp_send_mutex_;
    std::mutex udp_send_mutex_;
    std::thread tcp_thread_;
    std::thread udp_thread_;
    std::thread maintenance_thread_;

    std::mutex events_mutex_;
    std::deque<AgentEvent> events_;

    std::mutex transfers_mutex_;
    std::unordered_map<std::string,std::unique_ptr<filetransfer::FileSender>> senders_;
    std::unordered_map<std::uint32_t,std::string> pending_file_offers_;
    std::unordered_map<std::string,fileproto::Offer> incoming_offers_;
    std::unordered_map<std::string,std::unique_ptr<filetransfer::FileReceiver>> receivers_;
};
