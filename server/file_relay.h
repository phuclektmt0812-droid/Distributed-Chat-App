#pragma once
#include "server.h"
#include <array>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

class FileRelay {
public:
    explicit FileRelay(Server& server):server_(server){}
    bool handleTcpOffer(const protocol::Packet& packet,
                        const std::shared_ptr<ClientSession>& source,
                        std::string& error, std::uint16_t& errorCode);
    bool handleTcpDecision(const protocol::Packet& packet,
                           const std::shared_ptr<ClientSession>& source,
                           std::string& error, std::uint16_t& errorCode);
    bool handleTcpCancel(const protocol::Packet& packet,
                         const std::shared_ptr<ClientSession>& source,
                         std::string& error, std::uint16_t& errorCode);
    void handleUdp(const protocol::Packet& packet,
                   const std::shared_ptr<ClientSession>& source);
    void removeSession(const std::shared_ptr<ClientSession>& session);
    void expire(std::uint64_t nowMs);
private:
    enum class State { Pending, Accepted, Started, Ready };
    struct Transfer {
        std::array<std::uint8_t,16> id{};
        std::string sender, recipient, filename;
        std::uint64_t fileSize=0;
        std::uint16_t chunkSize=1100;
        std::uint32_t totalChunks=0;
        std::array<std::uint8_t,32> sha256{};
        State state=State::Pending;
        std::uint64_t lastActivityMs=0;
    };
    static std::string idKey(const std::uint8_t* p);
    static std::uint64_t nowMs();
    void sendResult(const Transfer& transfer, std::uint8_t status,
                    std::uint16_t errorCode, const std::string& detail);
    bool relayUdp(const protocol::Packet& packet,
                  const std::shared_ptr<ClientSession>& destination);
    Server& server_;
    std::mutex mutex_;
    std::unordered_map<std::string,Transfer> transfers_;
};
