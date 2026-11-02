#include "file_receiver.h"

#include <algorithm>
#include <stdexcept>

namespace filetransfer {
namespace fs = std::filesystem;

FileReceiver::FileReceiver(const fileproto::Offer& offer, const fs::path& directory,
                           protocol::SequenceCounter& sequence)
    : offer_(offer), directory_(directory), sequence_(sequence) {
    protocol::Bytes checked;
    if (!fileproto::encode(offer_,checked))
        throw std::invalid_argument("cannot receive invalid or unapproved FILE_OFFER");
    received_.assign(offer_.chunks,0);
}
FileReceiver::~FileReceiver() { if (state_!=ReceiverState::COMPLETED) removePartial(); }

protocol::Packet FileReceiver::acceptPacket() {
    fileproto::Accept a{offer_.id,true,protocol::MAX_WINDOW,0,""};
    protocol::Bytes data;
    if (!fileproto::encode(a,data)) throw std::runtime_error("encode FILE_ACCEPT_UDP failed");
    return {protocol::Type::FILE_ACCEPT_UDP,sequence_.next(),data};
}
protocol::Packet FileReceiver::ackPacket() {
    fileproto::Ack ack; ack.id=offer_.id; ack.base=ack_base_;
    ack.window=protocol::MAX_WINDOW;
    ack.bitmap.assign(4,0);
    for (std::uint32_t i=0;i<32;++i) {
        const auto index=std::uint64_t(ack_base_)+i;
        if (index>=offer_.chunks) break;
        if (received_[static_cast<std::size_t>(index)]) ack.bitmap[i/8]|=(1u<<(i%8));
    }
    protocol::Bytes data;
    if (!fileproto::encode(ack,data)) throw std::runtime_error("encode FILE_ACK failed");
    return {protocol::Type::FILE_ACK,sequence_.next(),data};
}
protocol::Packet FileReceiver::completePacket(bool verified, std::uint16_t code) {
    fileproto::Complete c{offer_.id,verified,code,received_bytes_,offer_.sha256};
    if (!verified) c.sha256.fill(0);
    protocol::Bytes bytes;
    if (!fileproto::encode(c,bytes)) throw std::runtime_error("encode FILE_COMPLETE failed");
    return {protocol::Type::FILE_COMPLETE,sequence_.next(),bytes};
}
protocol::Packet FileReceiver::failurePacket(std::uint16_t code, const std::string& detail) {
    fileproto::Failure f{offer_.id,code,detail};
    protocol::Bytes bytes;
    if (!fileproto::encode(f,bytes)) throw std::runtime_error("encode FILE_ERROR_UDP failed");
    return {protocol::Type::FILE_ERROR_UDP,sequence_.next(),bytes};
}
bool FileReceiver::matches(const fileproto::Start& s) const {
    return s.id==offer_.id && s.size==offer_.size && s.chunk_size==offer_.chunk_size &&
           s.chunks==offer_.chunks && s.sha256==offer_.sha256;
}
bool FileReceiver::matches(const fileproto::End& s) const {
    return s.id==offer_.id && s.size==offer_.size &&
           s.chunks==offer_.chunks && s.sha256==offer_.sha256;
}

std::vector<protocol::Packet> FileReceiver::onPacket(const protocol::Packet& p) {
    std::vector<protocol::Packet> reply;
    if (state_==ReceiverState::FAILED) return reply;
    if (p.type==protocol::Type::FILE_START) {
        fileproto::Start start;
        if (!fileproto::decode(p.payload,start) || !matches(start)) {
            fail("invalid FILE_START metadata");
            reply.push_back(failurePacket(204,error_)); return reply;
        }
        if (state_==ReceiverState::COMPLETED) {
            reply.push_back(completePacket(true,0)); return reply;
        }
        if (state_==ReceiverState::WAIT_START) {
            try {
                fs::create_directories(directory_);
                temporary_path_=directory_ / (fileproto::idHex(offer_.id)+".part");
                if (fs::exists(temporary_path_)) {
                    fail("another transfer already owns the .part filename");
                    reply.push_back(failurePacket(209,error_)); return reply;
                }
                output_.open(temporary_path_,std::ios::in|std::ios::out|std::ios::binary|std::ios::trunc);
                if (!output_) {
                    fail("cannot open .part file");
                    reply.push_back(failurePacket(207,error_)); return reply;
                }
            } catch (const fs::filesystem_error&) {
                fail("cannot create receive directory");
                reply.push_back(failurePacket(207,error_)); return reply;
            }
            owns_partial_=true;
            state_=ReceiverState::RECEIVING;
        }
        reply.push_back(acceptPacket());
        return reply;
    }

    if (state_==ReceiverState::COMPLETED) {
        if (p.type==protocol::Type::FILE_END || p.type==protocol::Type::FILE_CHUNK)
            reply.push_back(completePacket(true,0));
        return reply;
    }
    if (state_!=ReceiverState::RECEIVING) return reply;
    if (p.type==protocol::Type::FILE_CHUNK) {
        fileproto::Chunk chunk;
        if (!fileproto::decode(p.payload,chunk) || chunk.id!=offer_.id) return reply;
        const auto index=chunk.index;
        if (index>=offer_.chunks) {
            fail("out-of-range FILE_CHUNK index");
            reply.push_back(failurePacket(204,error_)); return reply;
        }
        const auto position=std::uint64_t(index)*protocol::FILE_CHUNK_SIZE;
        const auto expected=static_cast<std::size_t>(std::min<std::uint64_t>(
            protocol::FILE_CHUNK_SIZE,offer_.size-position));
        if (chunk.bytes.size()!=expected) {
            fail("wrong FILE_CHUNK byte count");
            reply.push_back(failurePacket(204,error_)); return reply;
        }
        if (!received_[index]) {
            output_.seekp(static_cast<std::streamoff>(position));
            output_.write(reinterpret_cast<const char*>(chunk.bytes.data()),
                          static_cast<std::streamsize>(chunk.bytes.size()));
            if (!output_) {
                fail("failed to write received file");
                reply.push_back(failurePacket(207,error_)); return reply;
            }
            received_[index]=1; ++received_count_; received_bytes_+=chunk.bytes.size();
            while (ack_base_<offer_.chunks && received_[ack_base_]) ++ack_base_;
        }
        reply.push_back(ackPacket()); // immediate ACK is allowed; group size = 1
        if (end_seen_ && received_count_==offer_.chunks) {
            if (finalize()) reply.push_back(completePacket(true,0));
            else reply.push_back(completePacket(false,206));
        }
    } else if (p.type==protocol::Type::FILE_END) {
        fileproto::End end;
        if (!fileproto::decode(p.payload,end) || !matches(end)) return reply;
        end_seen_=true;
        if (received_count_==offer_.chunks) {
            if (finalize()) reply.push_back(completePacket(true,0));
            else reply.push_back(completePacket(false,206));
        } else reply.push_back(ackPacket());
    } else if (p.type==protocol::Type::FILE_CANCEL_UDP ||
               p.type==protocol::Type::FILE_ERROR_UDP) {
        fileproto::Failure f;
        if (fileproto::decode(p.payload,f) && f.id==offer_.id) fail("transfer cancelled/failed remotely");
    }
    return reply;
}

bool FileReceiver::finalize() {
    output_.flush();
    if (!output_) { fail("failed to flush received file"); return false; }
    output_.close();
    try {
        if (fs::file_size(temporary_path_)!=offer_.size ||
            fileproto::sha256File(temporary_path_)!=offer_.sha256) {
            fail("SHA-256 mismatch"); return false;
        }
        fs::path base=directory_/offer_.filename;
        fs::path final=base;
        for (std::uint32_t n=1;fs::exists(final);++n) {
            if (n>10000) { fail("could not select collision-free name"); return false; }
            final=base.parent_path()/(base.stem().string()+" ("+std::to_string(n)+")"+base.extension().string());
        }
        fs::rename(temporary_path_,final);
        owns_partial_=false;
        completed_path_=final;
        state_=ReceiverState::COMPLETED;
        return true;
    } catch (const fs::filesystem_error& e) {
        fail(e.what()); return false;
    }
}

void FileReceiver::removePartial() {
    if (output_.is_open()) output_.close();
    if (owns_partial_) {
        std::error_code ignored;
        fs::remove(temporary_path_,ignored); // only file this instance created
        owns_partial_=false;
    }
}
void FileReceiver::fail(const std::string& why) {
    error_=why; state_=ReceiverState::FAILED; removePartial();
}
void FileReceiver::cancel() { fail("cancelled by local user"); }
} // namespace filetransfer
