#include "tcp_session.h"
#include "file_relay.h"
#include "../protocol/packet.h"
#include <chrono>
#include <iostream>
#include <random>

using namespace protocol;
static std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}
static void fillRandom(std::uint8_t* p,std::size_t n) {
    std::random_device rd;
    for(std::size_t i=0;i<n;++i) p[i]=static_cast<std::uint8_t>(rd());
}
TcpSession::TcpSession(Server& s,SocketHandle fd):server_(s),socket_(fd){}
void TcpSession::sendAuthFail(std::uint16_t code,const std::string& reason) {
    Packet p; p.type=Type::AUTH_FAIL; putU16(p.payload,code); putString(p.payload,reason);
    sendTcpPacket(socket_,p);
}
bool TcpSession::authenticate(Packet& first) {
    if(first.type!=Type::AUTH) { sendAuthFail(ERR_NOT_AUTHENTICATED,"AUTH must be first"); return false; }
    std::size_t off=0; std::string username;
    if(!getString(first.payload,off,username,MAX_USERNAME_BYTES) ||
       off!=first.payload.size() || username.empty()) {
        sendAuthFail(ERR_INVALID_USERNAME,"Invalid username"); return false;
    }
    for(unsigned char c:username) if(c<0x20 || c==0x7f) {
        sendAuthFail(ERR_INVALID_USERNAME,"Control character in username"); return false;
    }
    session_=std::make_shared<ClientSession>();
    session_->username=username; session_->socket=socket_; session_->authenticated=true;
    fillRandom(session_->sessionId.data(),session_->sessionId.size());
    fillRandom(session_->udpToken.data(),session_->udpToken.size());
    if(!server_.users().add(session_)) {
        // Do not let cleanup for a rejected duplicate username remove the
        // already-authenticated session that owns that username.
        session_.reset();
        sendAuthFail(ERR_USERNAME_IN_USE,"Username already online"); return false;
    }

    Packet ok; ok.type=Type::AUTH_OK;
    putString(ok.payload,username);
    ok.payload.insert(ok.payload.end(),session_->sessionId.begin(),session_->sessionId.end());
    putU64(ok.payload,nowMs());
    if(!server_.sendTo(session_,ok)) return false;

    Packet info; info.type=Type::SESSION_INFO;
    info.payload.insert(info.payload.end(),session_->udpToken.begin(),session_->udpToken.end());
    putU16(info.payload,UDP_PORT); putU32(info.payload,HEARTBEAT_INTERVAL_MS);
    if(!server_.sendTo(session_,info)) return false;

    server_.broadcastUserList();
    std::cout << "[LOGIN] " << username << std::endl;
    return true;
}
void TcpSession::sendAck(const Packet& req,std::uint8_t status,const std::string& detail) {
    Packet p; p.type=Type::ACK; p.sequence=0;
    p.payload.push_back(static_cast<std::uint8_t>(req.type));
    putU32(p.payload,req.sequence); p.payload.push_back(status); putString(p.payload,detail);
    server_.sendTo(session_,p);
}
void TcpSession::sendError(const Packet& req,std::uint16_t code,const std::string& detail) {
    Packet p; p.type=Type::ERROR;
    putU16(p.payload,code); p.payload.push_back(static_cast<std::uint8_t>(req.type));
    putU32(p.payload,req.sequence); putString(p.payload,detail);
    server_.sendTo(session_,p);
}
void TcpSession::handle(const Packet& p) {
    std::size_t off=0;
    if(p.type==Type::MSG) {
        if(p.payload.size()<4) { sendError(p,ERR_MALFORMED_PACKET,"Bad MSG"); return; }
        std::uint32_t id=readU32(p.payload.data()); off=4;
        std::string msg;
        if(!getString(p.payload,off,msg,MAX_CHAT_BYTES)||off!=p.payload.size()||msg.empty()) {
            sendError(p,ERR_MESSAGE_TOO_LARGE,"Invalid message"); return;
        }
        server_.broadcastMessage(session_,id,msg,p.sequence);
        return;
    }
    if(p.type==Type::PRIVATE_MSG) {
        if(p.payload.size()<4) { sendError(p,ERR_MALFORMED_PACKET,"Bad PRIVATE_MSG"); return; }
        std::uint32_t id=readU32(p.payload.data()); off=4;
        std::string recipient,msg;
        if(!getString(p.payload,off,recipient,MAX_USERNAME_BYTES) ||
           !getString(p.payload,off,msg,MAX_CHAT_BYTES) || off!=p.payload.size() || msg.empty()) {
            sendError(p,ERR_MALFORMED_PACKET,"Invalid private message"); return;
        }
        auto target=server_.users().find(recipient);
        if(!target) { sendError(p,ERR_USER_NOT_FOUND,"Recipient is offline"); return; }
        Packet forwarded; forwarded.type=Type::PRIVATE_MSG;
        putU32(forwarded.payload,id); putString(forwarded.payload,session_->username);
        putString(forwarded.payload,recipient); putU64(forwarded.payload,nowMs());
        putString(forwarded.payload,msg);
        if(server_.sendTo(target,forwarded)) sendAck(p,1,"Private message routed");
        else sendError(p,ERR_USER_NOT_FOUND,"Recipient connection failed");
        return;
    }
    if(p.type==Type::PING) {
        if(p.payload.size()!=16) { sendError(p,ERR_MALFORMED_PACKET,"Bad PING"); return; }
        Packet pong; pong.type=Type::PONG; pong.payload=p.payload;
        putU64(pong.payload,nowMs()); server_.sendTo(session_,pong); return;
    }
    if(p.type==Type::LOGOUT) {
        sendAck(p,1,"Logout"); shutdownSocket(socket_); return;
    }
    if(p.type==Type::FILE_OFFER || p.type==Type::FILE_DECISION || p.type==Type::FILE_CANCEL) {
        std::string error; std::uint16_t code=ERR_MALFORMED_PACKET; bool ok=false;
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
    Packet first;
    if(!receiveTcpPacket(socket_,first) || !authenticate(first)) {
        if(session_) server_.removeSession(session_);
        shutdownSocket(socket_); closeSocket(socket_); return;
    }
    Packet p;
    while(server_.running() && receiveTcpPacket(socket_,p)) {
        if(isUdpType(p.type)) break;
        handle(p);
    }
    if(session_) server_.removeSession(session_);
    shutdownSocket(socket_); closeSocket(socket_);
}
