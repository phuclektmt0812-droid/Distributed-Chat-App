#pragma once

#include "../protocol/file_messages.h"
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace filetransfer {

using TimePoint = std::chrono::steady_clock::time_point;
enum class SenderState { WAIT_DECISION, WAIT_UDP_READY, SENDING, WAIT_COMPLETE, COMPLETED, FAILED };

// Runs on the Client Agent's file-worker thread. Caller sends returned Packets
// to SERVER UDP :54001 and routes the server-forwarded reply back to onPacket().
// Packet sequencing uses the same UDP SequenceCounter as HEARTBEAT/UDP_REGISTER.
class FileSender {
public:
    FileSender(const std::string& input_path, const std::string& recipient,
               protocol::SequenceCounter& udp_sequence);

    protocol::Packet makeOffer(std::uint32_t tcp_sequence) const;
    bool handleDecision(const protocol::Packet& tcp_packet);
    void onPacket(const protocol::Packet& udp_packet);
    std::vector<protocol::Packet> poll(TimePoint now);
    void cancel();
    void failFromServer(const std::string& reason) { fail(reason); }

    SenderState state() const { return state_; }
    std::uint64_t acknowledgedBytes() const { return acknowledged_bytes_; }
    std::uint64_t totalBytes() const { return offer_.size; }
    std::uint32_t congestionWindow() const { return congestion_window_; }
    const fileproto::Offer& offer() const { return offer_; }
    const std::string& lastError() const { return error_; }

private:
    struct Pending {
        protocol::Packet packet;
        TimePoint deadline{};
        std::uint32_t rto_ms = protocol::INITIAL_RTO_MS;
        std::uint32_t retries = 0;
    };
    void fail(const std::string& error);
    Pending createPending(const protocol::Packet& p, TimePoint now) const;
    protocol::Packet makeChunk(std::uint32_t index);
    void processAck(const fileproto::Ack& ack);

    protocol::SequenceCounter& sequence_;
    std::ifstream source_;
    fileproto::Offer offer_;
    SenderState state_ = SenderState::WAIT_DECISION;
    std::string error_;
    std::uint32_t next_index_ = 0;
    std::uint32_t congestion_window_ = protocol::INITIAL_WINDOW;
    std::uint32_t receiver_window_ = protocol::MAX_WINDOW;
    std::uint32_t ack_progress_ = 0;
    std::uint64_t acknowledged_bytes_ = 0;
    std::vector<std::uint8_t> acked_;
    std::map<std::uint32_t, Pending> pending_;
    bool started_ = false;
    bool ended_ = false;
    Pending start_pending_{};
    Pending end_pending_{};
};
} // namespace filetransfer
