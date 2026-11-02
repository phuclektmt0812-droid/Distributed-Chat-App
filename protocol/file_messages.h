#pragma once

#include "packet.h"
#include <array>
#include <string>

namespace fileproto {
using TransferId = std::array<std::uint8_t, 16>;
using Digest = std::array<std::uint8_t, 32>;

struct Offer {
    TransferId id{};
    std::string recipient;
    std::string filename;
    std::uint64_t size = 0;
    std::uint16_t chunk_size = protocol::FILE_CHUNK_SIZE;
    std::uint32_t chunks = 0;
    Digest sha256{};
};
struct Decision { TransferId id{}; bool accepted = false; std::uint16_t reason_code = 0; std::string reason; };
struct Start { TransferId id{}; std::uint64_t size = 0; std::uint16_t chunk_size = protocol::FILE_CHUNK_SIZE; std::uint32_t chunks = 0; Digest sha256{}; };
struct Accept { TransferId id{}; bool ready = true; std::uint16_t window = 32; std::uint16_t reason_code = 0; std::string reason; };
struct Chunk { TransferId id{}; std::uint32_t index = 0; protocol::Bytes bytes; };
struct Ack { TransferId id{}; std::uint32_t base = 0; protocol::Bytes bitmap; std::uint16_t window = 32; };
struct End { TransferId id{}; std::uint32_t chunks = 0; std::uint64_t size = 0; Digest sha256{}; };
struct Complete { TransferId id{}; bool verified = false; std::uint16_t error_code = 0; std::uint64_t size = 0; Digest sha256{}; };
struct Failure { TransferId id{}; std::uint16_t code = 0; std::string detail; };
struct Result { TransferId id{}; std::uint8_t status = 0; std::uint16_t error_code = 0; std::uint64_t bytes = 0; std::string detail; };

TransferId randomId();
std::string idHex(const TransferId& id);
Digest sha256File(const std::string& path);
std::uint32_t chunkCount(std::uint64_t size); // throws std::invalid_argument if above 100 MiB

bool encode(const Offer&, protocol::Bytes&);    bool decode(const protocol::Bytes&, Offer&);
bool encode(const Decision&, protocol::Bytes&); bool decode(const protocol::Bytes&, Decision&);
bool encode(const Start&, protocol::Bytes&);    bool decode(const protocol::Bytes&, Start&);
bool encode(const Accept&, protocol::Bytes&);   bool decode(const protocol::Bytes&, Accept&);
bool encode(const Chunk&, protocol::Bytes&);    bool decode(const protocol::Bytes&, Chunk&);
bool encode(const Ack&, protocol::Bytes&);      bool decode(const protocol::Bytes&, Ack&);
bool encode(const End&, protocol::Bytes&);      bool decode(const protocol::Bytes&, End&);
bool encode(const Complete&, protocol::Bytes&); bool decode(const protocol::Bytes&, Complete&);
bool encode(const Failure&, protocol::Bytes&);  bool decode(const protocol::Bytes&, Failure&);
bool encode(const Result&, protocol::Bytes&);   bool decode(const protocol::Bytes&, Result&);

protocol::Packet makePacket(protocol::Type type, std::uint32_t sequence, const protocol::Bytes& payload);
} // namespace fileproto
