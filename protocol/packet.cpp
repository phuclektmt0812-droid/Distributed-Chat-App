#include "packet.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace protocol {
namespace {
std::uint32_t load32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}
} // namespace

bool isValidType(std::uint8_t t) {
    return (t >= 1 && t <= 20) || (t >= 32 && t <= 37) || (t >= 40 && t <= 47);
}
bool isUdpType(Type t) {
    return (std::uint8_t(t) >= 32 && std::uint8_t(t) <= 37) ||
           (std::uint8_t(t) >= 40 && std::uint8_t(t) <= 47);
}
bool isTcpType(Type t) { return std::uint8_t(t) >= 1 && std::uint8_t(t) <= 20; }

void ByteWriter::u8(std::uint8_t v) { bytes_.push_back(v); }
void ByteWriter::u16(std::uint16_t v) {
    u8(std::uint8_t(v >> 8)); u8(std::uint8_t(v));
}
void ByteWriter::u32(std::uint32_t v) {
    for (int i = 3; i >= 0; --i) u8(std::uint8_t(v >> (i * 8)));
}
void ByteWriter::u64(std::uint64_t v) {
    for (int i = 7; i >= 0; --i) u8(std::uint8_t(v >> (i * 8)));
}
void ByteWriter::raw(const std::uint8_t* p, std::size_t n) {
    if (n) bytes_.insert(bytes_.end(), p, p + n);
}
bool ByteWriter::str(const std::string& s) {
    if (s.size() > 65535 || !validUtf8(s)) return false;
    u16(static_cast<std::uint16_t>(s.size()));
    raw(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    return true;
}
bool ByteReader::u8(std::uint8_t& v) {
    if (left() < 1) return false;
    v = bytes_[offset_++]; return true;
}
bool ByteReader::u16(std::uint16_t& v) {
    if (left() < 2) return false;
    v = (std::uint16_t(bytes_[offset_]) << 8) | bytes_[offset_ + 1];
    offset_ += 2; return true;
}
bool ByteReader::u32(std::uint32_t& v) {
    if (left() < 4) return false;
    v = load32(bytes_.data() + offset_);
    offset_ += 4; return true;
}
bool ByteReader::u64(std::uint64_t& v) {
    if (left() < 8) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | bytes_[offset_++];
    return true;
}
bool ByteReader::raw(std::uint8_t* out, std::size_t n) {
    if (left() < n || (n && !out)) return false;
    if (n) std::memcpy(out, bytes_.data() + offset_, n);
    offset_ += n; return true;
}
bool ByteReader::str(std::string& s, std::size_t max_bytes) {
    std::uint16_t n = 0;
    if (!u16(n) || n > max_bytes || left() < n) return false;
    std::string text(reinterpret_cast<const char*>(bytes_.data() + offset_), n);
    if (!validUtf8(text)) return false;
    offset_ += n;
    s = std::move(text); return true;
}

bool encodePacket(const Packet& p, Bytes& output) {
    if (!isValidType(std::uint8_t(p.type)) || p.payload.size() > MAX_TCP_PAYLOAD) return false;
    ByteWriter w;
    w.u32(MAGIC); w.u8(VERSION); w.u8(std::uint8_t(p.type));
    w.u32(p.sequence); w.u32(static_cast<std::uint32_t>(p.payload.size()));
    w.raw(p.payload);
    output = w.take(); return true;
}
bool decodePacket(const std::uint8_t* data, std::size_t length, Packet& output) {
    if (!data || length < HEADER_SIZE || length > HEADER_SIZE + MAX_TCP_PAYLOAD ||
        load32(data) != MAGIC || data[4] != VERSION || !isValidType(data[5])) return false;
    const auto declared = load32(data + 10);
    if (declared > MAX_TCP_PAYLOAD || length != HEADER_SIZE + std::size_t(declared)) return false;
    Packet parsed;
    parsed.type = static_cast<Type>(data[5]); parsed.sequence = load32(data + 6);
    parsed.payload.assign(data + HEADER_SIZE, data + length);
    output = std::move(parsed); return true;
}
bool decodeDatagram(const std::uint8_t* data, std::size_t length, Packet& output) {
    Packet parsed;
    if (length > MAX_UDP_DATAGRAM || !decodePacket(data, length, parsed) ||
        !isUdpType(parsed.type)) return false;
    output = std::move(parsed); return true;
}
bool TcpPacketParser::feed(const std::uint8_t* data, std::size_t length,
                           std::vector<Packet>& out) {
    if (length && !data) return false;
    if (length > 2 * (HEADER_SIZE + MAX_TCP_PAYLOAD)) return false;
    if (length) buffer_.insert(buffer_.end(), data, data + length);
    std::size_t offset = 0;
    while (buffer_.size() - offset >= HEADER_SIZE) {
        const auto* p = buffer_.data() + offset;
        const auto n = load32(p + 10);
        if (load32(p) != MAGIC || p[4] != VERSION || !isTcpType(static_cast<Type>(p[5])) ||
            n > MAX_TCP_PAYLOAD) { reset(); return false; }
        if (buffer_.size() - offset - HEADER_SIZE < n) break;
        Packet parsed;
        if (!decodePacket(p, HEADER_SIZE + n, parsed)) { reset(); return false; }
        out.push_back(std::move(parsed)); offset += HEADER_SIZE + n;
    }
    if (offset) buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
    if (buffer_.size() > HEADER_SIZE + MAX_TCP_PAYLOAD) { reset(); return false; }
    return true;
}

bool validUtf8(const std::string& s) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = bytes[i];
        if (c <= 0x7f) { ++i; continue; }
        std::uint32_t cp = 0;
        int extra = 0;
        std::uint32_t min_cp = 0;
        if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; min_cp = 0x80; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; min_cp = 0x800; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; min_cp = 0x10000; }
        else return false;
        if (i + extra >= s.size()) return false;
        for (int j = 1; j <= extra; ++j) {
            if ((bytes[i + j] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (bytes[i + j] & 0x3f);
        }
        if (cp < min_cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        i += extra + 1;
    }
    return true;
}
bool validFilename(const std::string& s) {
    if (s.empty() || s.size() > 255 || s == "." || s == ".." || !validUtf8(s)) return false;
    for (unsigned char ch : s)
        if (ch < 32 || ch == 127 || ch == '/' || ch == '\\' || ch == ':' || ch == 0) return false;
    return true;
}
} // namespace protocol
