#pragma once
#include "dcap_client.h"
#include "../platform/socket_compat.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

class DcapHttpBridge {
public:
    DcapHttpBridge(const std::string& serverAddress, std::uint16_t serverPort = 54000,
                   std::uint16_t httpPort = 8080);
    ~DcapHttpBridge();
    bool start();
    void stop();

private:
    void acceptLoop();
    void handleConnection(SocketHandle socket);
    void respond(SocketHandle socket, int status, const std::string& contentType,
                 const std::string& body, const std::string& origin = "");
    DcapClient client_;
    std::uint16_t server_port_;
    std::uint16_t http_port_;
    SocketHandle listener_ = INVALID_SOCKET_HANDLE;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex workers_mutex_;
    std::vector<std::thread> workers_;
    std::mutex session_mutex_;
    std::mutex events_mutex_;
    std::vector<AgentEvent> pending_events_;
};
