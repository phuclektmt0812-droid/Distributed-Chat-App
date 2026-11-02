#pragma once
#include "server.h"
class UdpService {
public:
    explicit UdpService(Server& server):server_(server){}
    void handle(const std::uint8_t* data,std::size_t length,const sockaddr_in& source);
private:
    Server& server_;
};
