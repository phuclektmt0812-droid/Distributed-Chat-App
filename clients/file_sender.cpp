#include "file_sender.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace filetransfer {
namespace fs = std::filesystem;
namespace {
protocol::Packet packet(protocol::Type type, std::uint32_t seq, const protocol::Bytes& data) {
    return {type,seq,data};
}
} // namespace

FileSender::FileSender(const std::string& path, const std::string& recipient,
                       protocol::SequenceCounter& sequence)
    : sequence_(sequence), source_(path, std::ios::binary) {
    if (!source_ || !fs::is_regular_file(path)) throw std::runtime_error("input must be a regular file");
    offer_.id = fileproto::randomId(); offer_.recipient=recipient;
    offer_.filename=fs::path(path).filename().string();
    offer_.size=fs::file_size(path);
    offer_.chunks=fileproto::chunkCount(offer_.size);
    offer_.sha256=fileproto::sha256File(path);
    protocol::Bytes valid;
    if (!fileproto::encode(offer_,valid)) throw std::runtime_error("invalid FILE_OFFER metadata");
    acked_.assign(offer_.chunks,0);
}

protocol::Packet FileSender::makeOffer(std::uint32_t tcp_seq) const {
    protocol::Bytes bytes;
    if (!fileproto::encode(offer_,bytes)) throw std::runtime_error("cannot encode FILE_OFFER");
    return packet(protocol::Type::FILE_OFFER,tcp_seq,bytes);
}

bool FileSender::handleDecision(const protocol::Packet& p) {
    if (state_!=SenderState::WAIT_DECISION || p.type!=protocol::Type::FILE_DECISION) return false;
    fileproto::Decision decision;
    if (!fileproto::decode(p.payload,decision) || decision.id!=offer_.id) return false;
    if (!decision.accepted) fail("receiver rejected transfer: "+decision.reason);
    else state_=SenderState::WAIT_UDP_READY;
    return true;
}

FileSender::Pending FileSender::createPending(const protocol::Packet& p, TimePoint now) const {
    Pending result; result.packet=p;
    result.deadline=now+std::chrono::milliseconds(protocol::INITIAL_RTO_MS);
    return result;
}

protocol::Packet FileSender::makeChunk(std::uint32_t index) {
    const auto position=std::uint64_t(index)*protocol::FILE_CHUNK_SIZE;
    const auto amount=static_cast<std::size_t>(std::min<std::uint64_t>(
        protocol::FILE_CHUNK_SIZE,offer_.size-position));
    fileproto::Chunk c; c.id=offer_.id; c.index=index; c.bytes.resize(amount);
    source_.clear(); source_.seekg(static_cast<std::streamoff>(position));
    source_.read(reinterpret_cast<char*>(c.bytes.data()),static_cast<std::streamsize>(amount));
    if (source_.gcount()!=static_cast<std::streamsize>(amount)) {
        fail("input file changed during transfer"); return {};
    }
    protocol::Bytes bytes;
    if (!fileproto::encode(c,bytes)) { fail("could not encode FILE_CHUNK"); return {}; }
    return packet(protocol::Type::FILE_CHUNK,sequence_.next(),bytes);
}

void FileSender::processAck(const fileproto::Ack& a) {
    if (a.id!=offer_.id || a.base>next_index_ || a.base>offer_.chunks) return;
    receiver_window_=a.window;
    bool progress=false;
    auto acknowledge=[&](std::uint32_t index) {
        if (index>=next_index_ || acked_[index]) return;
        acked_[index]=1; pending_.erase(index);
        const auto pos=std::uint64_t(index)*protocol::FILE_CHUNK_SIZE;
        acknowledged_bytes_+=std::min<std::uint64_t>(protocol::FILE_CHUNK_SIZE,offer_.size-pos);
        ++ack_progress_; progress=true;
    };
    for (std::uint32_t i=0;i<a.base;++i) acknowledge(i);
    for (std::size_t byte=0;byte<a.bitmap.size();++byte) {
        for (int bit=0;bit<8;++bit) {
            if (!(a.bitmap[byte] & (1u<<bit))) continue;
            const auto index=std::uint64_t(a.base)+byte*8+bit;
            if (index<offer_.chunks) acknowledge(static_cast<std::uint32_t>(index));
        }
    }
    if (progress && ack_progress_>=congestion_window_ && congestion_window_<protocol::MAX_WINDOW) {
        ++congestion_window_; ack_progress_=0; // additive increase
    }
}

void FileSender::onPacket(const protocol::Packet& p) {
    if (state_==SenderState::COMPLETED || state_==SenderState::FAILED) return;
    if (p.type==protocol::Type::FILE_ACCEPT_UDP && state_==SenderState::WAIT_UDP_READY) {
        fileproto::Accept a;
        if (!fileproto::decode(p.payload,a) || a.id!=offer_.id) return;
        if (!a.ready) { fail("receiver not ready: "+a.reason); return; }
        receiver_window_=a.window;
        state_=SenderState::SENDING;
    } else if (p.type==protocol::Type::FILE_ACK &&
               (state_==SenderState::SENDING || state_==SenderState::WAIT_COMPLETE)) {
        fileproto::Ack ack;
        if (fileproto::decode(p.payload,ack)) processAck(ack);
    } else if (p.type==protocol::Type::FILE_COMPLETE &&
               (state_==SenderState::SENDING || state_==SenderState::WAIT_COMPLETE)) {
        fileproto::Complete complete;
        if (!fileproto::decode(p.payload,complete) || complete.id!=offer_.id) return;
        if (complete.verified && complete.error_code==0 &&
            complete.size==offer_.size && complete.sha256==offer_.sha256)
            state_=SenderState::COMPLETED;
        else fail("receiver rejected final integrity check");
    } else if (p.type==protocol::Type::FILE_ERROR_UDP ||
               p.type==protocol::Type::FILE_CANCEL_UDP) {
        fileproto::Failure f;
        if (fileproto::decode(p.payload,f) && f.id==offer_.id)
            fail("remote transfer error: "+f.detail);
    }
}

std::vector<protocol::Packet> FileSender::poll(TimePoint now) {
    std::vector<protocol::Packet> packets;
    if (state_==SenderState::WAIT_UDP_READY) {
        if (!started_) {
            fileproto::Start s{offer_.id,offer_.size,offer_.chunk_size,offer_.chunks,offer_.sha256};
            protocol::Bytes bytes; if (!fileproto::encode(s,bytes)) { fail("bad FILE_START"); return packets; }
            start_pending_=createPending(packet(protocol::Type::FILE_START,sequence_.next(),bytes),now);
            started_=true; packets.push_back(start_pending_.packet);
        } else if (now>=start_pending_.deadline) {
            if (start_pending_.retries>=protocol::MAX_RETRANSMISSIONS) { fail("FILE_START timeout"); return packets; }
            ++start_pending_.retries;
            start_pending_.rto_ms=std::min(start_pending_.rto_ms*2,protocol::MAX_RTO_MS);
            start_pending_.deadline=now+std::chrono::milliseconds(start_pending_.rto_ms);
            packets.push_back(start_pending_.packet); // same sequence on retransmission
        }
        return packets;
    }
    if (state_!=SenderState::SENDING && state_!=SenderState::WAIT_COMPLETE) return packets;

    for (auto& [index, outstanding] : pending_) {
        if (now<outstanding.deadline) continue;
        if (outstanding.retries>=protocol::MAX_RETRANSMISSIONS) {
            fail("FILE_CHUNK timeout at index "+std::to_string(index)); return {};
        }
        ++outstanding.retries;
        congestion_window_=std::max<std::uint32_t>(1,congestion_window_/2); // multiplicative decrease
        ack_progress_=0;
        outstanding.rto_ms=std::min(outstanding.rto_ms*2,protocol::MAX_RTO_MS);
        outstanding.deadline=now+std::chrono::milliseconds(outstanding.rto_ms);
        packets.push_back(outstanding.packet);
    }
    if (state_==SenderState::SENDING) {
        const auto capacity=std::min(congestion_window_,receiver_window_);
        while (next_index_<offer_.chunks && pending_.size()<capacity) {
            auto p=makeChunk(next_index_);
            if (state_==SenderState::FAILED) return {};
            pending_.emplace(next_index_,createPending(p,now));
            packets.push_back(std::move(p)); ++next_index_;
        }
        if (next_index_==offer_.chunks && !ended_) {
            fileproto::End end{offer_.id,offer_.chunks,offer_.size,offer_.sha256};
            protocol::Bytes b; if (!fileproto::encode(end,b)) { fail("bad FILE_END"); return {}; }
            end_pending_=createPending(packet(protocol::Type::FILE_END,sequence_.next(),b),now);
            packets.push_back(end_pending_.packet);
            ended_=true; state_=SenderState::WAIT_COMPLETE;
        }
    }
    if (state_==SenderState::WAIT_COMPLETE && now>=end_pending_.deadline) {
        if (end_pending_.retries>=protocol::MAX_RETRANSMISSIONS) {
            fail("FILE_COMPLETE timeout"); return {};
        }
        ++end_pending_.retries;
        end_pending_.rto_ms=std::min(end_pending_.rto_ms*2,protocol::MAX_RTO_MS);
        end_pending_.deadline=now+std::chrono::milliseconds(end_pending_.rto_ms);
        packets.push_back(end_pending_.packet);
    }
    return packets;
}

void FileSender::fail(const std::string& error) { error_=error; state_=SenderState::FAILED; }
void FileSender::cancel() { fail("cancelled by local user"); }
} // namespace filetransfer
