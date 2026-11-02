#include "file_relay.h"
#include "../protocol/packet.h"
#include <algorithm>
#include <chrono>
#include <cstring>

using namespace protocol;

std::string FileRelay::idKey(const std::uint8_t* p) {
    static const char hex[]="0123456789abcdef";
    std::string s; s.reserve(32);
    for(int i=0;i<16;++i) { s.push_back(hex[p[i]>>4]); s.push_back(hex[p[i]&15]); }
    return s;
}
std::uint64_t FileRelay::nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
bool FileRelay::validFilename(const std::string& n) {
    if(n.empty() || n.size()>255 || !validUtf8(n) || n=="." || n=="..") return false;
    for(unsigned char c:n) if(c<0x20 || c==0x7f || c=='/' || c=='\\' || c==':') return false;
    return n.find("..") == std::string::npos;
}
void FileRelay::sendResult(const Transfer& t,std::uint8_t status,
                           std::uint16_t code,const std::string& detail) {
    Packet p; p.type=Type::FILE_RESULT;
    p.payload.insert(p.payload.end(),t.id.begin(),t.id.end());
    p.payload.push_back(status); putU16(p.payload,code); putU64(p.payload,(status==1? t.fileSize:0)); putString(p.payload,detail);
    server_.sendTo(server_.users().find(t.sender),p);
    server_.sendTo(server_.users().find(t.recipient),p);
}
bool FileRelay::handleTcpOffer(const Packet& p,const std::shared_ptr<ClientSession>& source,
                               std::string& error,std::uint16_t& code) {
    error="Invalid FILE_OFFER"; code=ERR_MALFORMED_PACKET;
    if(!source || p.payload.size()<16) return false;
    std::size_t off=16; std::string recipient,filename;
    if(!getString(p.payload,off,recipient,MAX_USERNAME_BYTES) ||
       !getString(p.payload,off,filename,255) || off+8+2+4+32!=p.payload.size()) return false;
    const std::uint64_t size=readU64(p.payload.data()+off); off+=8;
    const std::uint16_t chunk=readU16(p.payload.data()+off); off+=2;
    const std::uint32_t total=readU32(p.payload.data()+off); off+=4;
    if(!validFilename(filename)) { error="Invalid filename"; code=ERR_INVALID_FILENAME; return false; }
    if(size>104857600ULL) { error="File exceeds 100 MiB"; code=ERR_FILE_TOO_LARGE; return false; }
    const std::uint32_t expected=static_cast<std::uint32_t>((size+1099)/1100);
    if(chunk!=1100 || total!=expected) { error="Invalid file metadata"; return false; }
    auto target=server_.users().find(recipient);
    if(!target || target==source) { error="Recipient is unavailable"; code=ERR_USER_NOT_FOUND; return false; }
    Transfer t; std::copy_n(p.payload.data(),16,t.id.begin()); t.sender=source->username;
    t.recipient=recipient; t.filename=filename; t.fileSize=size; t.chunkSize=chunk; t.totalChunks=total;
    std::copy_n(p.payload.data()+off,32,t.sha256.begin()); t.lastActivityMs=nowMs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(transfers_.size()>=20) { error="Server transfer capacity reached"; code=ERR_TRANSFER_BUSY; return false; }
        const auto key=idKey(t.id.data());
        if(transfers_.count(key)) { error="Duplicate transfer ID"; code=ERR_TRANSFER_BUSY; return false; }
        std::size_t perSender=0, perReceiver=0;
        for(const auto& kv:transfers_) {
            if(kv.second.sender==t.sender) ++perSender;
            if(kv.second.recipient==t.recipient) ++perReceiver;
        }
        if(perSender>=4 || perReceiver>=4) { error="Client transfer capacity reached"; code=ERR_TRANSFER_BUSY; return false; }
        transfers_.emplace(key,t);
    }
    if(!server_.sendTo(target,p)) {
        std::lock_guard<std::mutex> lock(mutex_); transfers_.erase(idKey(t.id.data()));
        error="Could not notify recipient"; code=ERR_USER_NOT_FOUND; return false;
    }
    error.clear(); code=0; return true;
}
bool FileRelay::handleTcpDecision(const Packet& p,const std::shared_ptr<ClientSession>& source,
                                  std::string& error,std::uint16_t& code) {
    error="Transfer not found"; code=ERR_TRANSFER_NOT_FOUND;
    if(!source || p.payload.size()<21) { error="Malformed FILE_DECISION"; code=ERR_MALFORMED_PACKET; return false; }
    std::size_t parseOff=19; std::string reason;
    if(!getString(p.payload,parseOff,reason,1024) || parseOff!=p.payload.size()) { error="Malformed FILE_DECISION"; code=ERR_MALFORMED_PACKET; return false; }
    const auto key=idKey(p.payload.data());
    if(p.payload[16]>1) { error="Invalid decision"; code=ERR_MALFORMED_PACKET; return false; }
    std::shared_ptr<ClientSession> sender;
    Transfer copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=transfers_.find(key); if(it==transfers_.end()) return false;
        auto& t=it->second;
        if(t.recipient!=source->username || t.state!=State::Pending) { error="Not permitted"; code=ERR_TRANSFER_REJECTED; return false; }
        copy=t; sender=server_.users().find(t.sender);
        if(p.payload[16]==1) { t.state=State::Accepted; t.lastActivityMs=nowMs(); }
        else transfers_.erase(it);
    }
    if(sender) server_.sendTo(sender,p);
    if(p.payload[16]==0) sendResult(copy,0,readU16(p.payload.data()+17),"Receiver rejected transfer");
    error.clear(); code=0; return true;
}
bool FileRelay::handleTcpCancel(const Packet& p,const std::shared_ptr<ClientSession>& source,
                                std::string& error,std::uint16_t& code) {
    error="Transfer not found"; code=ERR_TRANSFER_NOT_FOUND;
    if(!source || p.payload.size()<20) { error="Malformed FILE_CANCEL"; code=ERR_MALFORMED_PACKET; return false; }
    std::size_t parseOff=18; std::string reason;
    if(!getString(p.payload,parseOff,reason,1024) || parseOff!=p.payload.size()) { error="Malformed FILE_CANCEL"; code=ERR_MALFORMED_PACKET; return false; }
    const auto key=idKey(p.payload.data()); Transfer copy;
    std::shared_ptr<ClientSession> peer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=transfers_.find(key); if(it==transfers_.end()) return false;
        if(it->second.sender!=source->username && it->second.recipient!=source->username) {
            error="Not a transfer participant"; code=ERR_TRANSFER_REJECTED; return false;
        }
        copy=it->second; peer=server_.users().find(copy.sender==source->username?copy.recipient:copy.sender);
        transfers_.erase(it);
    }
    if(peer) server_.sendTo(peer,p);
    sendResult(copy,2,ERR_TRANSFER_CANCELLED,"Transfer cancelled");
    error.clear(); code=0; return true;
}
bool FileRelay::relayUdp(const Packet& p,const std::shared_ptr<ClientSession>& dst) {
    if(!dst || !dst->udpRegistered) return false;
    std::vector<std::uint8_t> bytes;
    if(!encodePacket(p,bytes) || bytes.size()>MAX_UDP_DATAGRAM) return false;
#ifdef _WIN32
    return sendto(server_.udpSocket(),reinterpret_cast<const char*>(bytes.data()),static_cast<int>(bytes.size()),0,
        reinterpret_cast<const sockaddr*>(&dst->udpEndpoint),sizeof(sockaddr_in))==static_cast<int>(bytes.size());
#else
    return sendto(server_.udpSocket(),bytes.data(),bytes.size(),0,
        reinterpret_cast<const sockaddr*>(&dst->udpEndpoint),sizeof(sockaddr_in))==static_cast<ssize_t>(bytes.size());
#endif
}
void FileRelay::handleUdp(const Packet& p,const std::shared_ptr<ClientSession>& source) {
    if(!source || p.payload.size()<16) return;
    const auto key=idKey(p.payload.data()); Transfer copy; std::shared_ptr<ClientSession> destination;
    bool eraseAfter=false; std::uint8_t resultStatus=1; std::uint16_t resultCode=0; std::string resultText;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=transfers_.find(key); if(it==transfers_.end()) return;
        auto& t=it->second;
        const bool sender=(source->username==t.sender), receiver=(source->username==t.recipient);
        if(!sender && !receiver) return;
        bool allowed=false;
        switch(p.type) {
            case Type::FILE_START: {
                if(sender && t.state==State::Accepted && p.payload.size()==16+8+2+4+32) {
                    const auto* d=p.payload.data();
                    allowed=readU64(d+16)==t.fileSize && readU16(d+24)==t.chunkSize &&
                        readU32(d+26)==t.totalChunks && std::equal(t.sha256.begin(),t.sha256.end(),d+30);
                    if(allowed) t.state=State::Started;
                } break;
            }
            case Type::FILE_ACCEPT_UDP: {
                std::size_t reasonOffset=21; std::string reason;
                const bool validReason=p.payload.size()>=23 &&
                    getString(p.payload,reasonOffset,reason,1024) && reasonOffset==p.payload.size();
                allowed=receiver && t.state==State::Started && validReason &&
                    p.payload[16]<=1 && readU16(p.payload.data()+17)>=1 && readU16(p.payload.data()+17)<=32;
                if(allowed && p.payload[16]==1) t.state=State::Ready;
                if(allowed && p.payload[16]==0) { eraseAfter=true; resultStatus=0; resultCode=readU16(p.payload.data()+19); resultText="Receiver could not prepare file"; }
                break;
            }
            case Type::FILE_CHUNK: {
                if(sender && t.state==State::Ready && p.payload.size()>=22) {
                    const auto index=readU32(p.payload.data()+16); const auto n=readU16(p.payload.data()+20);
                    const std::uint64_t expected=(index+1<t.totalChunks)?1100:(t.fileSize-static_cast<std::uint64_t>(index)*1100);
                    allowed=index<t.totalChunks && n==expected && p.payload.size()==static_cast<std::size_t>(22+n);
                } break;
            }
            case Type::FILE_ACK:
                allowed=receiver && t.state==State::Ready && p.payload.size()>=24 &&
                    readU32(p.payload.data()+16)<=t.totalChunks && readU16(p.payload.data()+20)<=4 &&
                    p.payload.size()==static_cast<std::size_t>(24+readU16(p.payload.data()+20)) &&
                    readU16(p.payload.data()+22+readU16(p.payload.data()+20))<=32;
                break;
            case Type::FILE_END:
                allowed=sender && t.state==State::Ready && p.payload.size()==44 &&
                    readU32(p.payload.data()+16)==t.totalChunks && readU64(p.payload.data()+20)==t.fileSize &&
                    std::equal(t.sha256.begin(),t.sha256.end(),p.payload.data()+28);
                break;
            case Type::FILE_COMPLETE:
                allowed=receiver && t.state==State::Ready && p.payload.size()==59 && p.payload[16]<=1;
                if(allowed && p.payload[16]==1)
                    allowed=readU64(p.payload.data()+19)==t.fileSize &&
                        std::equal(t.sha256.begin(),t.sha256.end(),p.payload.data()+27);
                if(allowed) { eraseAfter=true; resultStatus=p.payload[16]?1:0;
                    resultCode=readU16(p.payload.data()+17); resultText=p.payload[16]?"File transfer completed":"File transfer failed"; }
                break;
            case Type::FILE_ERROR_UDP: case Type::FILE_CANCEL_UDP: {
                std::size_t detailOffset=18; std::string detail;
                allowed=p.payload.size()>=20 && getString(p.payload,detailOffset,detail,1024) &&
                    detailOffset==p.payload.size();
                if(allowed) { eraseAfter=true; resultStatus=(p.type==Type::FILE_CANCEL_UDP)?2:0;
                    resultCode=readU16(p.payload.data()+16); resultText=detail.empty()?"Transfer terminated by participant":detail; }
                break;
            }
            default: return;
        }
        if(!allowed) return;
        t.lastActivityMs=nowMs(); copy=t;
        destination=server_.users().find(sender?t.recipient:t.sender);
        if(eraseAfter) transfers_.erase(it);
    }
    if(!relayUdp(p,destination)) {
        if(!eraseAfter) { std::lock_guard<std::mutex> lock(mutex_); transfers_.erase(key); }
        sendResult(copy,0,ERR_USER_NOT_FOUND,"Peer UDP endpoint is unavailable"); return;
    }
    if(eraseAfter) sendResult(copy,resultStatus,resultCode,resultText);
}
void FileRelay::removeSession(const std::shared_ptr<ClientSession>& s) {
    if(!s) return;
    std::vector<Transfer> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto it=transfers_.begin();it!=transfers_.end();) {
            if(it->second.sender==s->username || it->second.recipient==s->username) {
                removed.push_back(it->second); it=transfers_.erase(it);
            } else ++it;
        }
    }
    for(const auto& t:removed) {
        auto peer=server_.users().find(t.sender==s->username?t.recipient:t.sender);
        if(peer) { Packet p; p.type=Type::FILE_CANCEL; p.payload.insert(p.payload.end(),t.id.begin(),t.id.end());
            putU16(p.payload,ERR_TRANSFER_CANCELLED); putString(p.payload,"Peer disconnected"); server_.sendTo(peer,p); }
        sendResult(t,0,ERR_TRANSFER_CANCELLED,"Transfer stopped because a participant disconnected");
    }
}
void FileRelay::expire(std::uint64_t now) {
    std::vector<Transfer> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto it=transfers_.begin();it!=transfers_.end();) {
            if(now>it->second.lastActivityMs && now-it->second.lastActivityMs>120000) {
                expired.push_back(it->second); it=transfers_.erase(it);
            } else ++it;
        }
    }
    for(const auto& t:expired) sendResult(t,0,ERR_TRANSFER_TIMEOUT,"Transfer expired due to inactivity");
}
