#include "file_relay.h"
#include "../protocol/file_messages.h"
#include "../protocol/packet.h"
#include <algorithm>
#include <chrono>

using namespace protocol;

std::string FileRelay::idKey(const std::uint8_t* p) {
    static const char hex[]="0123456789abcdef";
    std::string s; s.reserve(32);
    for(int i=0;i<16;++i) {
        s.push_back(hex[p[i]>>4]);
        s.push_back(hex[p[i]&15]);
    }
    return s;
}
std::uint64_t FileRelay::nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
void FileRelay::sendResult(const Transfer& t,std::uint8_t status,
                           std::uint16_t code,const std::string& detail) {
    fileproto::Result payload;
    payload.id=t.id;
    payload.status=status;
    payload.error_code=code;
    payload.bytes=(status==1)?t.fileSize:0;
    payload.detail=detail;

    Packet p; p.type=Type::FILE_RESULT;
    if(!fileproto::encode(payload,p.payload)) return;
    server_.sendTo(server_.users().find(t.sender),p);
    server_.sendTo(server_.users().find(t.recipient),p);
}
bool FileRelay::handleTcpOffer(const Packet& p,const std::shared_ptr<ClientSession>& source,
                               std::string& error,std::uint16_t& code) {
    error="Invalid FILE_OFFER";
    code=ERR_MALFORMED_PACKET;
    fileproto::Offer offer;
    if(!source || !fileproto::decode(p.payload,offer)) return false;

    auto target=server_.users().find(offer.recipient);
    if(!target || target==source) {
        error="Recipient is unavailable";
        code=ERR_USER_NOT_FOUND;
        return false;
    }

    Transfer t;
    t.id=offer.id;
    t.sender=source->username;
    t.recipient=offer.recipient;
    t.filename=offer.filename;
    t.fileSize=offer.size;
    t.chunkSize=offer.chunk_size;
    t.totalChunks=offer.chunks;
    t.sha256=offer.sha256;
    t.lastActivityMs=nowMs();

    const auto key=idKey(t.id.data());
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(transfers_.size()>=20) {
            error="Server transfer capacity reached";
            code=ERR_TRANSFER_BUSY;
            return false;
        }
        if(transfers_.count(key)) {
            error="Duplicate transfer ID";
            code=ERR_TRANSFER_BUSY;
            return false;
        }
        std::size_t perSender=0, perReceiver=0;
        for(const auto& entry:transfers_) {
            if(entry.second.sender==t.sender) ++perSender;
            if(entry.second.recipient==t.recipient) ++perReceiver;
        }
        if(perSender>=4 || perReceiver>=4) {
            error="Client transfer capacity reached";
            code=ERR_TRANSFER_BUSY;
            return false;
        }
        transfers_.emplace(key,t);
    }

    if(!server_.sendTo(target,p)) {
        std::lock_guard<std::mutex> lock(mutex_);
        transfers_.erase(key);
        error="Could not notify recipient";
        code=ERR_USER_NOT_FOUND;
        return false;
    }
    error.clear();
    code=0;
    return true;
}
bool FileRelay::handleTcpDecision(const Packet& p,const std::shared_ptr<ClientSession>& source,
                                  std::string& error,std::uint16_t& code) {
    error="Transfer not found";
    code=ERR_TRANSFER_NOT_FOUND;
    fileproto::Decision decision;
    if(!source || !fileproto::decode(p.payload,decision)) {
        error="Malformed FILE_DECISION";
        code=ERR_MALFORMED_PACKET;
        return false;
    }

    const auto key=idKey(decision.id.data());
    std::shared_ptr<ClientSession> sender;
    Transfer copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=transfers_.find(key);
        if(it==transfers_.end()) return false;
        auto& transfer=it->second;
        if(transfer.recipient!=source->username || transfer.state!=State::Pending) {
            error="Not permitted";
            code=ERR_TRANSFER_REJECTED;
            return false;
        }
        copy=transfer;
        sender=server_.users().find(transfer.sender);
        if(decision.accepted) {
            transfer.state=State::Accepted;
            transfer.lastActivityMs=nowMs();
        } else {
            transfers_.erase(it);
        }
    }

    if(sender) server_.sendTo(sender,p);
    if(!decision.accepted) {
        const std::string detail=decision.reason.empty()?"Receiver rejected transfer":decision.reason;
        sendResult(copy,0,decision.reason_code,detail);
    }
    error.clear();
    code=0;
    return true;
}
bool FileRelay::handleTcpCancel(const Packet& p,const std::shared_ptr<ClientSession>& source,
                                std::string& error,std::uint16_t& code) {
    error="Transfer not found";
    code=ERR_TRANSFER_NOT_FOUND;
    fileproto::Failure failure;
    if(!source || !fileproto::decode(p.payload,failure)) {
        error="Malformed FILE_CANCEL";
        code=ERR_MALFORMED_PACKET;
        return false;
    }

    const auto key=idKey(failure.id.data());
    Transfer copy;
    std::shared_ptr<ClientSession> peer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=transfers_.find(key);
        if(it==transfers_.end()) return false;
        if(it->second.sender!=source->username && it->second.recipient!=source->username) {
            error="Not a transfer participant";
            code=ERR_TRANSFER_REJECTED;
            return false;
        }
        copy=it->second;
        peer=server_.users().find(copy.sender==source->username?copy.recipient:copy.sender);
        transfers_.erase(it);
    }
    if(peer) server_.sendTo(peer,p);
    sendResult(copy,2,ERR_TRANSFER_CANCELLED,
               failure.detail.empty()?"Transfer cancelled":failure.detail);
    error.clear();
    code=0;
    return true;
}
bool FileRelay::relayUdp(const Packet& p,const std::shared_ptr<ClientSession>& dst) {
    return dst && dst->udpRegistered &&
           protocol::sendUdpPacket(server_.udpSocket(),p,dst->udpEndpoint);
}
void FileRelay::handleUdp(const Packet& p,const std::shared_ptr<ClientSession>& source) {
    if(!source || p.payload.size()<16) return;

    const auto key=idKey(p.payload.data());
    Transfer copy;
    std::shared_ptr<ClientSession> destination;
    bool eraseAfter=false;
    std::uint8_t resultStatus=1;
    std::uint16_t resultCode=0;
    std::string resultText;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=transfers_.find(key);
        if(it==transfers_.end()) return;
        auto& transfer=it->second;
        const bool sender=(source->username==transfer.sender);
        const bool receiver=(source->username==transfer.recipient);
        if(!sender && !receiver) return;

        bool allowed=false;
        switch(p.type) {
            case Type::FILE_START: {
                fileproto::Start message;
                allowed=sender && transfer.state==State::Accepted &&
                        fileproto::decode(p.payload,message) &&
                        message.size==transfer.fileSize &&
                        message.chunk_size==transfer.chunkSize &&
                        message.chunks==transfer.totalChunks &&
                        message.sha256==transfer.sha256;
                if(allowed) transfer.state=State::Started;
                break;
            }
            case Type::FILE_ACCEPT_UDP: {
                fileproto::Accept message;
                allowed=receiver && transfer.state==State::Started &&
                        fileproto::decode(p.payload,message) &&
                        message.window>=1 && message.window<=MAX_WINDOW;
                if(allowed && message.ready) {
                    transfer.state=State::Ready;
                } else if(allowed) {
                    eraseAfter=true;
                    resultStatus=0;
                    resultCode=message.reason_code;
                    resultText=message.reason.empty()?"Receiver could not prepare file":message.reason;
                }
                break;
            }
            case Type::FILE_CHUNK: {
                fileproto::Chunk message;
                allowed=sender && transfer.state==State::Ready &&
                        fileproto::decode(p.payload,message);
                if(allowed) {
                    const std::uint64_t expected=(message.index+1<transfer.totalChunks)
                        ? transfer.chunkSize
                        : transfer.fileSize-static_cast<std::uint64_t>(message.index)*transfer.chunkSize;
                    allowed=message.index<transfer.totalChunks && message.bytes.size()==expected;
                }
                break;
            }
            case Type::FILE_ACK: {
                fileproto::Ack message;
                allowed=receiver && transfer.state==State::Ready &&
                        fileproto::decode(p.payload,message) &&
                        message.base<=transfer.totalChunks &&
                        message.window<=MAX_WINDOW;
                break;
            }
            case Type::FILE_END: {
                fileproto::End message;
                allowed=sender && transfer.state==State::Ready &&
                        fileproto::decode(p.payload,message) &&
                        message.chunks==transfer.totalChunks &&
                        message.size==transfer.fileSize &&
                        message.sha256==transfer.sha256;
                break;
            }
            case Type::FILE_COMPLETE: {
                fileproto::Complete message;
                allowed=receiver && transfer.state==State::Ready &&
                        fileproto::decode(p.payload,message);
                if(allowed && message.verified) {
                    allowed=message.size==transfer.fileSize &&
                            message.sha256==transfer.sha256;
                }
                if(allowed) {
                    eraseAfter=true;
                    resultStatus=message.verified?1:0;
                    resultCode=message.error_code;
                    resultText=message.verified?"File transfer completed":"File transfer failed";
                }
                break;
            }
            case Type::FILE_ERROR_UDP:
            case Type::FILE_CANCEL_UDP: {
                fileproto::Failure message;
                allowed=fileproto::decode(p.payload,message);
                if(allowed) {
                    eraseAfter=true;
                    resultStatus=(p.type==Type::FILE_CANCEL_UDP)?2:0;
                    resultCode=message.code;
                    resultText=message.detail.empty()
                        ?"Transfer terminated by participant":message.detail;
                }
                break;
            }
            default:
                return;
        }

        if(!allowed) return;
        transfer.lastActivityMs=nowMs();
        copy=transfer;
        destination=server_.users().find(sender?transfer.recipient:transfer.sender);
        if(eraseAfter) transfers_.erase(it);
    }

    if(!relayUdp(p,destination)) {
        if(!eraseAfter) {
            std::lock_guard<std::mutex> lock(mutex_);
            transfers_.erase(key);
        }
        sendResult(copy,0,ERR_USER_NOT_FOUND,"Peer UDP endpoint is unavailable");
        return;
    }
    if(eraseAfter) sendResult(copy,resultStatus,resultCode,resultText);
}
void FileRelay::removeSession(const std::shared_ptr<ClientSession>& session) {
    if(!session) return;
    std::vector<Transfer> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto it=transfers_.begin();it!=transfers_.end();) {
            if(it->second.sender==session->username || it->second.recipient==session->username) {
                removed.push_back(it->second);
                it=transfers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for(const auto& transfer:removed) {
        auto peer=server_.users().find(
            transfer.sender==session->username?transfer.recipient:transfer.sender);
        if(peer) {
            fileproto::Failure failure;
            failure.id=transfer.id;
            failure.code=ERR_TRANSFER_CANCELLED;
            failure.detail="Peer disconnected";
            Packet p; p.type=Type::FILE_CANCEL;
            if(fileproto::encode(failure,p.payload)) server_.sendTo(peer,p);
        }
        sendResult(transfer,0,ERR_TRANSFER_CANCELLED,
                   "Transfer stopped because a participant disconnected");
    }
}
void FileRelay::expire(std::uint64_t now) {
    std::vector<Transfer> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(auto it=transfers_.begin();it!=transfers_.end();) {
            if(now>it->second.lastActivityMs && now-it->second.lastActivityMs>120000) {
                expired.push_back(it->second);
                it=transfers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for(const auto& transfer:expired)
        sendResult(transfer,0,ERR_TRANSFER_TIMEOUT,"Transfer expired due to inactivity");
}
