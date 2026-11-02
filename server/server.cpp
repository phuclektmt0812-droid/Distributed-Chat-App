#include "server.h"
#include "tcp_session.h"
#include "udp_service.h"
#include "file_relay.h"
#include "../protocol/message_payloads.h"
#include "../protocol/packet.h"
#include <chrono>
#include <cstring>
#include <iostream>

Server::Server() {
    udpService_=std::make_unique<UdpService>(*this);
    fileRelay_=std::make_unique<FileRelay>(*this);
}
Server::~Server() { stop(); }
FileRelay& Server::fileRelay() { return *fileRelay_; }

bool Server::start() {
    if(running_) return true;
    try { encryptionKey_=protocol::loadOrCreateKeyFile(protocol::keyFilePathFromEnvironment());
        std::cout<<"[SECURITY] Using persistent key file: "<<protocol::keyFilePathFromEnvironment()<<std::endl; }
    catch(const std::exception& error) { std::cerr<<"[SECURITY] "<<error.what()<<std::endl; return false; }
    tcpListener_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    udpSocket_=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(tcpListener_==INVALID_SOCKET_HANDLE || udpSocket_==INVALID_SOCKET_HANDLE) {
        std::cerr<<"[ERROR] socket() failed: "<<lastSocketError()<<std::endl; stop(); return false;
    }
    setReuseAddress(tcpListener_); setReuseAddress(udpSocket_);
    sockaddr_in tcp{}; tcp.sin_family=AF_INET; tcp.sin_port=htons(protocol::TCP_PORT);
    tcp.sin_addr.s_addr=htonl(INADDR_ANY);
    sockaddr_in udp{}; udp.sin_family=AF_INET; udp.sin_port=htons(protocol::UDP_PORT);
    udp.sin_addr.s_addr=htonl(INADDR_ANY);
    if(bind(tcpListener_,reinterpret_cast<sockaddr*>(&tcp),sizeof(tcp))!=0 ||
       listen(tcpListener_,SOMAXCONN)!=0 ||
       bind(udpSocket_,reinterpret_cast<sockaddr*>(&udp),sizeof(udp))!=0) {
        std::cerr<<"[ERROR] bind/listen failed: "<<lastSocketError()<<std::endl; stop(); return false;
    }
    running_=true;
    acceptThread_=std::thread(&Server::acceptLoop,this);
    udpThread_=std::thread(&Server::udpLoop,this);
    maintenanceThread_=std::thread(&Server::maintenanceLoop,this);
    std::cout<<"Server listening TCP "<<protocol::TCP_PORT
             <<" / UDP "<<protocol::UDP_PORT<<std::endl;
    return true;
}
void Server::stop() {
    if(!running_ && tcpListener_==INVALID_SOCKET_HANDLE && udpSocket_==INVALID_SOCKET_HANDLE) return;
    running_=false;
    if(tcpListener_!=INVALID_SOCKET_HANDLE) { shutdownSocket(tcpListener_); closeSocket(tcpListener_); tcpListener_=INVALID_SOCKET_HANDLE; }
    if(udpSocket_!=INVALID_SOCKET_HANDLE) { shutdownSocket(udpSocket_); closeSocket(udpSocket_); udpSocket_=INVALID_SOCKET_HANDLE; }
    for(auto& s:users_.snapshot()) if(s->socket!=INVALID_SOCKET_HANDLE) shutdownSocket(s->socket);
    if(acceptThread_.joinable()) acceptThread_.join();
    if(udpThread_.joinable()) udpThread_.join();
    if(maintenanceThread_.joinable()) maintenanceThread_.join();
    {
        std::lock_guard<std::mutex> lock(workersMutex_);
        for(auto& t:workers_) if(t.joinable()) t.join();
        workers_.clear();
    }
}
void Server::acceptLoop() {
    while(running_) {
        sockaddr_in addr{}; SocketLength len=sizeof(addr);
        SocketHandle client=accept(tcpListener_,reinterpret_cast<sockaddr*>(&addr),&len);
        if(client==INVALID_SOCKET_HANDLE) {
            if(!running_) break;
            if(isInterruptedError(lastSocketError())) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::lock_guard<std::mutex> lock(workersMutex_);
        workers_.emplace_back([this,client] { TcpSession(*this,client).run(); });
    }
}
void Server::udpLoop() {
    std::uint8_t buffer[protocol::MAX_UDP_DATAGRAM+1];
    while(running_) {
        sockaddr_in src{}; SocketLength len=sizeof(src);
#ifdef _WIN32
        int n=recvfrom(udpSocket_,reinterpret_cast<char*>(buffer),sizeof(buffer),0,
                       reinterpret_cast<sockaddr*>(&src),&len);
#else
        ssize_t n=recvfrom(udpSocket_,buffer,sizeof(buffer),0,
                           reinterpret_cast<sockaddr*>(&src),&len);
#endif
        if(n<=0) { if(!running_) break; std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        if(static_cast<std::size_t>(n)>protocol::MAX_UDP_DATAGRAM) continue;
        udpService_->handle(buffer,static_cast<std::size_t>(n),src);
    }
}
void Server::maintenanceLoop() {
    while(running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if(!running_) break;
        auto now=static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        fileRelay_->expire(now);
        auto expired=users_.expiredUdp(now);
        for(auto& s:expired) std::cout<<"[UDP TIMEOUT] "<<s->username<<std::endl;
    }
}
bool Server::sendTo(const std::shared_ptr<ClientSession>& s,const protocol::Packet& p) {
    if(!s || s->socket==INVALID_SOCKET_HANDLE) return false;
    std::lock_guard<std::mutex> lock(s->sendMutex);
    return protocol::sendTcpPacketSecure(s->socket,p,encryptionKey_);
}
bool Server::sendUdpTo(const protocol::Packet& p,const sockaddr_in& destination) {
    if(udpSocket_==INVALID_SOCKET_HANDLE) return false;
    return protocol::sendUdpPacketSecure(udpSocket_,p,destination,encryptionKey_);
}
void Server::broadcastUserList() {
    messages::UserList payload;
    payload.revision=users_.revision();
    payload.users=users_.usernames();
    protocol::Packet p; p.type=protocol::Type::USER_LIST;
    if(!messages::encode(payload,p.payload)) return;
    auto clients=users_.snapshot();
    for(auto& s:clients) sendTo(s,p);
}
void Server::broadcastMessage(const std::shared_ptr<ClientSession>& sender,
                              std::uint32_t clientId,const std::string& message,
                              std::uint32_t requestSequence) {
    messages::ChatEvent event;
    event.server_id=serverMessageId_.fetch_add(1,std::memory_order_relaxed);
    event.sender=sender->username;
    event.server_time_ms=static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    event.text=message;
    (void)clientId; // Client ID remains in the API; the server assigns its own ID.

    protocol::Packet p; p.type=protocol::Type::MSG;
    if(!messages::encode(event,p.payload)) return;
    for(auto& s:users_.snapshot()) sendTo(s,p);

    messages::Ack ackPayload;
    ackPayload.acknowledged_type=static_cast<std::uint8_t>(protocol::Type::MSG);
    ackPayload.request_sequence=requestSequence;
    ackPayload.status=1;
    ackPayload.detail="Broadcast delivered";
    protocol::Packet ack; ack.type=protocol::Type::ACK;
    if(messages::encode(ackPayload,ack.payload)) sendTo(sender,ack);
}
void Server::removeSession(const std::shared_ptr<ClientSession>& s) {
    if(!s) return;
    fileRelay_->removeSession(s);
    users_.remove(s->username);
    std::cout<<"[DISCONNECT] "<<s->username<<std::endl;
    broadcastUserList();
}
