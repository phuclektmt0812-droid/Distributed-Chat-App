#pragma once

#include "../clients/client.h"
#include "../platform/socket_compat.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

class LiveWebGateway {
public:
    LiveWebGateway(std::string serverAddress, std::uint16_t serverPort,
                   std::uint16_t httpPort, std::filesystem::path webRoot);
    ~LiveWebGateway();

    bool start();
    void stop();

private:
    struct Session;

    void acceptLoop();
    void expireIdleSessions();
    void handleConnection(SocketHandle socket);
    std::shared_ptr<Session> findSession(const std::string& token);
    std::string sessionTokenFromCookie(const std::string& cookie) const;

    std::string server_address_;
    std::uint16_t server_port_;
    std::uint16_t http_port_;
    std::filesystem::path web_root_;

    SocketHandle listener_ = INVALID_SOCKET_HANDLE;
    std::atomic<bool> running_{false};
    bool socket_runtime_initialized_ = false;
    std::thread accept_thread_;

    std::mutex workers_mutex_;
    std::condition_variable workers_cv_;
    std::size_t active_workers_ = 0;

    std::mutex sessions_mutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
};
