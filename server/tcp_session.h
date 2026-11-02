#include "../protocol/protocol.h"
#pragma once
#include "server.h"
class TcpSession {
public:
    TcpSession(Server& server, SocketHandle socket);
    void run();
private:
    bool authenticate(protocol::Packet& first);
    void handle(const protocol::Packet& packet);
    void sendAuthFail(std::uint16_t code,const std::string& reason);
    void sendAck(const protocol::Packet& request,std::uint8_t status,
                 const std::string& detail);
    void sendError(const protocol::Packet& request,std::uint16_t code,
                   const std::string& detail);
    Server& server_;
    std::shared_ptr<ClientSession> session_;
    SocketHandle socket_;
};
