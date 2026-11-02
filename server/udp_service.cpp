#include "udp_service.h"
#include "file_relay.h"
#include "../protocol/message_payloads.h"
#include "../protocol/packet.h"
#include <chrono>

using namespace protocol;

static std::uint64_t udpNowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}
void UdpService::handle(const std::uint8_t* data,std::size_t len,const sockaddr_in& src) {
    Packet p;
    if(!decodeSecureDatagram(data,len,server_.encryptionKey(),p)) return;

    if(p.type==Type::UDP_REGISTER) {
        messages::UdpRegister request;
        if(!messages::decode(p.payload,request)) return;
        const bool ok=server_.users().bindUdp(request.token,src);
        messages::UdpRegisterOk response;
        response.registered=ok;
        response.code=ok?0:ERR_INVALID_SESSION;
        Packet reply; reply.type=Type::UDP_REGISTER_OK;
        if(messages::encode(response,reply.payload))
            server_.sendUdpTo(reply,src);
        return;
    }

    auto session=server_.users().findByUdpEndpoint(src);
    if(!session) return;

    if(p.type==Type::HEARTBEAT) {
        messages::Heartbeat request;
        if(!messages::decode(p.payload,request)) return;
        session->lastHeartbeatMs=udpNowMs();
        messages::HeartbeatAck response;
        response.nonce=request.nonce;
        response.client_time_ms=request.client_time_ms;
        response.server_time_ms=udpNowMs();
        Packet ack; ack.type=Type::HEARTBEAT_ACK;
        if(messages::encode(response,ack.payload))
            server_.sendUdpTo(ack,src);
        return;
    }

    if(p.type==Type::DISCOVERY) {
        if(!p.payload.empty()) return;
        messages::UserList response;
        response.revision=server_.users().revision();
        response.users=server_.users().usernames();
        Packet reply; reply.type=Type::DISCOVERY_REPLY;
        if(messages::encode(response,reply.payload))
            sendUdpPacket(server_.udpSocket(),reply,src);
        return;
    }

    // The registered UDP endpoint is the authenticated session identity.
    server_.fileRelay().handleUdp(p,session);
}
