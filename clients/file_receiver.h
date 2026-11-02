#pragma once

#include "../protocol/file_messages.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace filetransfer {
enum class ReceiverState { WAIT_START, RECEIVING, COMPLETED, FAILED };

// Construct only after the user ACCEPTED the exact TCP FILE_OFFER.
// All packets are received through Member 1's UDP server relay, not direct P2P.
class FileReceiver {
public:
    FileReceiver(const fileproto::Offer& approved_offer,
                 const std::filesystem::path& receive_directory,
                 protocol::SequenceCounter& udp_sequence);
    ~FileReceiver();

    FileReceiver(const FileReceiver&) = delete;
    FileReceiver& operator=(const FileReceiver&) = delete;

    std::vector<protocol::Packet> onPacket(const protocol::Packet& udp_packet);
    void cancel();

    ReceiverState state() const { return state_; }
    std::uint64_t receivedBytes() const { return received_bytes_; }
    std::uint64_t totalBytes() const { return offer_.size; }
    std::filesystem::path completedPath() const { return completed_path_; }
    const std::string& lastError() const { return error_; }

private:
    protocol::Packet acceptPacket();
    protocol::Packet ackPacket();
    protocol::Packet completePacket(bool verified, std::uint16_t error_code);
    protocol::Packet failurePacket(std::uint16_t code, const std::string& detail);
    bool matches(const fileproto::Start&) const;
    bool matches(const fileproto::End&) const;
    bool finalize();
    void fail(const std::string& reason);
    void removePartial();

    fileproto::Offer offer_;
    std::filesystem::path directory_;
    std::filesystem::path temporary_path_;
    std::filesystem::path completed_path_;
    protocol::SequenceCounter& sequence_;
    ReceiverState state_ = ReceiverState::WAIT_START;
    std::string error_;
    std::fstream output_;
    std::vector<std::uint8_t> received_;
    std::uint64_t received_bytes_ = 0;
    std::uint32_t received_count_ = 0;
    std::uint32_t ack_base_ = 0;
    bool end_seen_ = false;
    bool owns_partial_ = false;
};
} // namespace filetransfer
