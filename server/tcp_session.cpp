#include "tcp_session.h"
#include "file_relay.h"
#include "../protocol/message_payloads.h"
#include "../protocol/packet.h"
#include <chrono>
#include <iostream>
#include <openssl/rand.h>

using namespace protocol;
static std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}
static bool fillRandom(std::uint8_t* p,std::size_t n) {
    return RAND_bytes(p,static_cast<int>(n))==1;
}
TcpSession::TcpSession(Server& s,SocketHandle fd):server_(s),socket_(fd){}
void TcpSession::sendAuthFail(std::uint16_t code,const std::string& reason) {
    Packet p; p.type=Type::AUTH_FAIL;
    if(!messages::encode(messages::AuthFail{code,reason},p.payload)) return;
    sendTcpPacketSecure(socket_,p,server_.encryptionKey());
}
bool TcpSession::authenticate(Packet& first) {
    if(first.type!=Type::AUTH) {
        sendAuthFail(ERR_NOT_AUTHENTICATED,"AUTH must be first");
        return false;
    }
    messages::Auth request;
    if(!messages::decode(first.payload,request)) {
        sendAuthFail(ERR_INVALID_USERNAME,"Invalid username");
        return false;
    }

    session_=std::make_shared<ClientSession>();
    session_->username=request.username;
    session_->socket=socket_;
    session_->authenticated=true;
    if(!fillRandom(session_->sessionId.data(),session_->sessionId.size()) ||
       !fillRandom(session_->udpToken.data(),session_->udpToken.size())) {
        session_.reset();
        sendAuthFail(ERR_INVALID_SESSION,"Secure random generation failed");
        return false;
    }
    if(!server_.users().add(session_)) {
        // A rejected duplicate login must not remove the existing session.
        session_.reset();
        sendAuthFail(ERR_USERNAME_IN_USE,"Username already online");
        return false;
    }

    messages::AuthOk authOk;
    authOk.username=session_->username;
    authOk.session_id=session_->sessionId;
    authOk.server_time_ms=nowMs();
    Packet ok; ok.type=Type::AUTH_OK;
    if(!messages::encode(authOk,ok.payload) || !server_.sendTo(session_,ok)) return false;

    messages::SessionInfo sessionInfo;
    sessionInfo.token=session_->udpToken;
    sessionInfo.udp_port=UDP_PORT;
    sessionInfo.heartbeat_ms=HEARTBEAT_INTERVAL_MS;
    Packet info; info.type=Type::SESSION_INFO;
    if(!messages::encode(sessionInfo,info.payload) || !server_.sendTo(session_,info)) return false;

    server_.broadcastUserList();
    std::cout << "[LOGIN] " << session_->username << std::endl;
    return true;
}
void TcpSession::sendAck(const Packet& req,std::uint8_t status,const std::string& detail) {
    messages::Ack payload;
    payload.acknowledged_type=static_cast<std::uint8_t>(req.type);
    payload.request_sequence=req.sequence;
    payload.status=status;
    payload.detail=detail;
    Packet p; p.type=Type::ACK;
    if(messages::encode(payload,p.payload)) server_.sendTo(session_,p);
}
void TcpSession::sendError(const Packet& req,std::uint16_t code,const std::string& detail) {
    messages::Error payload;
    payload.code=code;
    payload.related_type=static_cast<std::uint8_t>(req.type);
    payload.related_sequence=req.sequence;
    payload.detail=detail;
    Packet p; p.type=Type::ERROR;
    if(messages::encode(payload,p.payload)) server_.sendTo(session_,p);
}
void TcpSession::handle(const Packet& p) {
    if(p.type==Type::MSG) {
        messages::ChatRequest request;
        if(!messages::decode(p.payload,request)) {
            sendError(p,ERR_MALFORMED_PACKET,"Invalid MSG payload");
            return;
        }
        server_.broadcastMessage(session_,request.client_id,request.text,p.sequence);
        return;
    }
    if(p.type==Type::PRIVATE_MSG) {
        messages::PrivateRequest request;
        if(!messages::decode(p.payload,request)) {
            sendError(p,ERR_MALFORMED_PACKET,"Invalid PRIVATE_MSG payload");
            return;
        }
        auto target=server_.users().find(request.recipient);
        if(!target) {
            sendError(p,ERR_USER_NOT_FOUND,"Recipient is offline");
            return;
        }

        messages::PrivateEvent event;
        event.server_id=request.client_id;
        event.sender=session_->username;
        event.recipient=request.recipient;
        event.server_time_ms=nowMs();
        event.text=request.text;
        Packet forwarded; forwarded.type=Type::PRIVATE_MSG;
        if(!messages::encode(event,forwarded.payload)) {
            sendError(p,ERR_MALFORMED_PACKET,"Could not encode private message");
            return;
        }
        if(server_.sendTo(target,forwarded)) sendAck(p,1,"Private message routed");
        else sendError(p,ERR_USER_NOT_FOUND,"Recipient connection failed");
        return;
    }
    if(p.type==Type::PING) {
        if(p.payload.size()!=16) {
            sendError(p,ERR_MALFORMED_PACKET,"Bad PING");
            return;
        }
        Packet pong; pong.type=Type::PONG; pong.payload=p.payload;
        putU64(pong.payload,nowMs());
        server_.sendTo(session_,pong);
        return;
    }
    if(p.type==Type::LOGOUT) {
        sendAck(p,1,"Logout");
        shutdownSocket(socket_);
        return;
    }
    if(p.type==Type::FILE_OFFER || p.type==Type::FILE_DECISION || p.type==Type::FILE_CANCEL) {
        std::string error;
        std::uint16_t code=ERR_MALFORMED_PACKET;
        bool ok=false;
        if(p.type==Type::FILE_OFFER)
            ok=server_.fileRelay().handleTcpOffer(p,session_,error,code);
        else if(p.type==Type::FILE_DECISION)
            ok=server_.fileRelay().handleTcpDecision(p,session_,error,code);
        else
            ok=server_.fileRelay().handleTcpCancel(p,session_,error,code);
        if(ok) sendAck(p,1,"File control message processed");
        else sendError(p,code,error.empty()?"File control request rejected":error);
        return;
    }
    sendError(p,ERR_UNKNOWN_MESSAGE_TYPE,"Unsupported TCP message type");
}
void TcpSession::run() {
    // Unauthenticated sockets must not hold shutdown open; authenticated sessions get a longer idle window.
    setSocketTimeout(socket_,10000,10000);
    setSocketKeepAlive(socket_);
    Packet first;
    if(!receiveTcpPacketSecure(socket_,first,server_.encryptionKey()) || !authenticate(first)) {
        if(session_) server_.removeSession(session_);
        shutdownSocket(socket_); closeSocket(socket_); return;
    }
    setSocketTimeout(socket_,120000,10000);
    Packet p;
    while(server_.running() && receiveTcpPacketSecure(socket_,p,server_.encryptionKey())) {
        if(isUdpType(p.type)) break;
        handle(p);
    }
    if(session_) server_.removeSession(session_);
    shutdownSocket(socket_); closeSocket(socket_);
}
