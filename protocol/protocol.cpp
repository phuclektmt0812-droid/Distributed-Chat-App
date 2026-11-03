#include "protocol.h"

#include <algorithm>
#include <climits>

namespace protocol {
void putU16(Bytes& bytes,std::uint16_t v) {
    bytes.push_back(std::uint8_t(v>>8)); bytes.push_back(std::uint8_t(v));
}
void putU32(Bytes& bytes,std::uint32_t v) {
    for(int i=3;i>=0;--i) bytes.push_back(std::uint8_t(v>>(8*i)));
}
void putU64(Bytes& bytes,std::uint64_t v) {
    for(int i=7;i>=0;--i) bytes.push_back(std::uint8_t(v>>(8*i)));
}
bool putString(Bytes& bytes,const std::string& value) {
    if(value.size()>65535 || !validUtf8(value)) return false;
    putU16(bytes,static_cast<std::uint16_t>(value.size()));
    bytes.insert(bytes.end(),value.begin(),value.end()); return true;
}
std::uint16_t readU16(const std::uint8_t* p) { return (std::uint16_t(p[0])<<8)|p[1]; }
std::uint32_t readU32(const std::uint8_t* p) {
    return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|
           (std::uint32_t(p[2])<<8)|p[3];
}
std::uint64_t readU64(const std::uint8_t* p) {
    std::uint64_t v=0;
    for(int i=0;i<8;++i) v=(v<<8)|p[i];
    return v;
}
bool getString(const Bytes& b,std::size_t& offset,std::string& output,std::size_t maximum) {
    if(offset>b.size() || b.size()-offset<2) return false;
    const auto n=readU16(b.data()+offset);
    if(n>maximum || b.size()-offset-2<n) return false;
    std::string value(reinterpret_cast<const char*>(b.data()+offset+2),n);
    if(!validUtf8(value)) return false;
    offset+=2+n; output=std::move(value); return true;
}

// Read exactly N bytes. TCP can split an application packet across recv calls.
static bool readExactly(SocketHandle socket,std::uint8_t* p,std::size_t n) {
    while(n) {
        const int amount=static_cast<int>(std::min<std::size_t>(n,INT_MAX));
#ifdef _WIN32
        const int received=recv(socket,reinterpret_cast<char*>(p),amount,0);
#else
        const int received=static_cast<int>(recv(socket,p,static_cast<std::size_t>(amount),0));
#endif
        if(received==0) return false;
        if(received<0) {
            if(isInterruptedError(lastSocketError())) continue;
            return false;
        }
        p+=received; n-=static_cast<std::size_t>(received);
    }
    return true;
}
bool receiveTcpPacket(SocketHandle socket,Packet& packet) {
    std::uint8_t header[HEADER_SIZE]{};
    if(!readExactly(socket,header,sizeof(header))) return false;
    if(readU32(header)!=MAGIC || header[4]!=VERSION ||
       !isTcpType(static_cast<Type>(header[5]))) return false;
    const auto length=readU32(header+10);
    if(length>MAX_TCP_PAYLOAD) return false;
    Bytes frame(header,header+sizeof(header));
    frame.resize(HEADER_SIZE+length);
    if(length && !readExactly(socket,frame.data()+HEADER_SIZE,length)) return false;
    return decodePacket(frame,packet);
}
bool sendTcpPacket(SocketHandle socket,const Packet& packet) {
    if(!isTcpType(packet.type)) return false;
    Bytes frame;
    if(!encodePacket(packet,frame)) return false;
    std::size_t offset=0;
    while(offset<frame.size()) {
        const auto amount=static_cast<int>(std::min<std::size_t>(frame.size()-offset,INT_MAX));
#ifdef _WIN32
        const int n=send(socket,reinterpret_cast<const char*>(frame.data()+offset),amount,0);
#else
        const int n=static_cast<int>(send(socket,frame.data()+offset,
                                         static_cast<std::size_t>(amount),0));
#endif
        if(n<=0) {
            if(n<0 && isInterruptedError(lastSocketError())) continue;
            return false;
        }
        offset+=static_cast<std::size_t>(n);
    }
    return true;
}
bool sendUdpPacket(SocketHandle socket,const Packet& packet,const sockaddr_in& destination) {
    if(!isUdpType(packet.type)) return false;
    Bytes datagram;
    if(!encodePacket(packet,datagram) || datagram.size()>MAX_UDP_DATAGRAM) return false;
#ifdef _WIN32
    const int sent=sendto(socket,reinterpret_cast<const char*>(datagram.data()),
                          static_cast<int>(datagram.size()),0,
                          reinterpret_cast<const sockaddr*>(&destination),sizeof(destination));
    return sent==static_cast<int>(datagram.size());
#else
    const auto sent=sendto(socket,datagram.data(),datagram.size(),0,
                           reinterpret_cast<const sockaddr*>(&destination),sizeof(destination));
    return sent==static_cast<ssize_t>(datagram.size());
#endif
}
bool sendTcpPacketSecure(SocketHandle socket,const Packet& packet,const EncryptionKey& key) {
    if(!isTcpType(packet.type)) return false;
    Bytes frame;
    if(!encodeSecurePacket(packet,key,frame)) return false;
    std::size_t offset=0;
    while(offset<frame.size()) {
        const auto amount=static_cast<int>(std::min<std::size_t>(frame.size()-offset,INT_MAX));
#ifdef _WIN32
        const int n=send(socket,reinterpret_cast<const char*>(frame.data()+offset),amount,0);
#else
        const int n=static_cast<int>(send(socket,frame.data()+offset,static_cast<std::size_t>(amount),0));
#endif
        if(n<=0) { if(n<0 && isInterruptedError(lastSocketError())) continue; return false; }
        offset+=static_cast<std::size_t>(n);
    }
    return true;
}
bool receiveTcpPacketSecure(SocketHandle socket,Packet& packet,const EncryptionKey& key) {
    std::uint8_t header[HEADER_SIZE]{};
    if(!readExactly(socket,header,sizeof(header))) return false;
    if(readU32(header)!=MAGIC || header[4]!=VERSION || !isTcpType(static_cast<Type>(header[5]))) return false;
    const auto length=readU32(header+10);
    if(length<28 || length>MAX_TCP_PAYLOAD) return false;
    Bytes frame(header,header+sizeof(header));
    frame.resize(HEADER_SIZE+length);
    if(!readExactly(socket,frame.data()+HEADER_SIZE,length)) return false;
    return decodeSecurePacket(frame.data(),frame.size(),key,packet);
}
bool sendUdpPacketSecure(SocketHandle socket,const Packet& packet,const sockaddr_in& destination,const EncryptionKey& key) {
    if(!isUdpType(packet.type)) return false;
    Bytes datagram;
    if(!encodeSecurePacket(packet,key,datagram) || datagram.size()>MAX_UDP_DATAGRAM) return false;
#ifdef _WIN32
    const int sent=sendto(socket,reinterpret_cast<const char*>(datagram.data()),static_cast<int>(datagram.size()),0,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination));
    return sent==static_cast<int>(datagram.size());
#else
    const auto sent=sendto(socket,datagram.data(),datagram.size(),0,reinterpret_cast<const sockaddr*>(&destination),sizeof(destination));
    return sent==static_cast<ssize_t>(datagram.size());
#endif
}
bool decodePacket(const std::uint8_t* data,std::size_t n,Packet& output,std::size_t max_wire) {
    return n<=max_wire && decodePacket(data,n,output);
}
} // namespace protocol
