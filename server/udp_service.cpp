#include "udp_service.h"
#include "file_relay.h"
#include "../protocol/packet.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#ifdef _WIN32
#define CLOSESOCK closesocket
#else
#define CLOSESOCK close
#endif
using namespace protocol;
static std::uint64_t udpNowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}
static bool sendUdp(SocketHandle s,const Packet& p,const sockaddr_in& to) {
    std::vector<std::uint8_t> bytes;
    if(!encodePacket(p,bytes)||bytes.size()>MAX_UDP_DATAGRAM) return false;
#ifdef _WIN32
    return sendto(s,reinterpret_cast<const char*>(bytes.data()),
                  static_cast<int>(bytes.size()),0,
                  reinterpret_cast<const sockaddr*>(&to),sizeof(to))==static_cast<int>(bytes.size());
#else
    return sendto(s,bytes.data(),bytes.size(),0,
                  reinterpret_cast<const sockaddr*>(&to),sizeof(to))==static_cast<ssize_t>(bytes.size());
#endif
}
void UdpService::handle(const std::uint8_t* data,std::size_t len,const sockaddr_in& src) {
    Packet p;
    if(!decodePacket(data,len,p,MAX_UDP_DATAGRAM)) return;
    if(!isUdpType(p.type)) return;
    if(p.type==Type::UDP_REGISTER) {
        if(p.payload.size()!=32) return;
        std::array<std::uint8_t,32> token{};
        std::copy(p.payload.begin(),p.payload.end(),token.begin());
        bool ok=server_.users().bindUdp(token,src);
        Packet reply; reply.type=Type::UDP_REGISTER_OK;
        reply.payload.push_back(ok?1:0); putU16(reply.payload,ok?0:ERR_INVALID_SESSION);
        sendUdp(server_.udpSocket(),reply,src); return;
    }
    auto session=server_.users().findByUdpEndpoint(src);
    if(!session) return;
    if(p.type==Type::HEARTBEAT) {
        if(p.payload.size()!=16) return;
        // Heartbeat updates only for a packet from the registered endpoint.
        session->lastHeartbeatMs=udpNowMs();
        Packet ack; ack.type=Type::HEARTBEAT_ACK;
        ack.payload=p.payload; putU64(ack.payload,udpNowMs());
        sendUdp(server_.udpSocket(),ack,src); return;
    }
    if(p.type==Type::DISCOVERY) {
        if(!p.payload.empty()) return;
        Packet reply; reply.type=Type::DISCOVERY_REPLY;
        putU32(reply.payload,server_.users().revision());
        auto names=server_.users().usernames();
        putU16(reply.payload,static_cast<std::uint16_t>(names.size()));
        for(const auto& name:names) putString(reply.payload,name);
        sendUdp(server_.udpSocket(),reply,src); return;
    }
    // Authenticated UDP endpoint is the source of session identity.
    server_.fileRelay().handleUdp(p,session);
}
