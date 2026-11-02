#pragma once

#include "constants.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace protocol {
using Bytes = std::vector<std::uint8_t>;

struct Packet {
    Type type = Type::AUTH;
    std::uint32_t sequence = 0;
    Bytes payload;
};

bool isValidType(std::uint8_t value);
bool isUdpType(Type type);
bool isTcpType(Type type);

// Canonical Member 1 v1 packet: DCAP[4] | version[1] | type[1] |
// sequence[4] | payload_length[4] | payload bytes. No extra TCP prefix.
bool encodePacket(const Packet& packet, Bytes& output);
bool decodePacket(const std::uint8_t* data, std::size_t length, Packet& output);
bool decodeDatagram(const std::uint8_t* data, std::size_t length, Packet& output);
inline bool decodeDatagram(const Bytes& data, Packet& output) {
    return decodeDatagram(data.data(), data.size(), output);
}
inline bool decodePacket(const Bytes& data, Packet& output) {
    return decodePacket(data.data(), data.size(), output);
}

// Holds incomplete TCP stream fragments; create one parser per TCP socket.
class TcpPacketParser {
public:
    bool feed(const std::uint8_t* data, std::size_t length, std::vector<Packet>& packets);
    void reset() { buffer_.clear(); }
private:
    Bytes buffer_;
};

// Share one allocator among all threads emitting packets for the same socket.
class SequenceCounter {
public:
    std::uint32_t next() { return next_.fetch_add(1, std::memory_order_relaxed); }
private:
    std::atomic<std::uint32_t> next_{1};
};

class ByteWriter {
public:
    void u8(std::uint8_t v);
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void raw(const std::uint8_t* data, std::size_t n);
    void raw(const Bytes& b) { raw(b.data(), b.size()); }
    bool str(const std::string& utf8); // uint16 byte length + UTF-8
    const Bytes& data() const { return bytes_; }
    Bytes take() { return std::move(bytes_); }
private:
    Bytes bytes_;
};

class ByteReader {
public:
    explicit ByteReader(const Bytes& bytes) : bytes_(bytes) {}
    bool u8(std::uint8_t& v);
    bool u16(std::uint16_t& v);
    bool u32(std::uint32_t& v);
    bool u64(std::uint64_t& v);
    bool raw(std::uint8_t* out, std::size_t n);
    bool str(std::string& utf8, std::size_t max_bytes = 65535);
    bool done() const { return offset_ == bytes_.size(); }
    std::size_t left() const { return bytes_.size() - offset_; }
private:
    const Bytes& bytes_;
    std::size_t offset_ = 0;
};

bool validUtf8(const std::string& text);
bool validFilename(const std::string& basename);
} // namespace protocol
