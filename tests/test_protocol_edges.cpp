#include "../protocol/packet.h"
#include <algorithm>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void testTypeClassification() {
    require(protocol::isValidType(1) && protocol::isValidType(17), "TCP type range rejected");
    require(protocol::isValidType(32) && protocol::isValidType(37), "UDP control range rejected");
    require(protocol::isValidType(40) && protocol::isValidType(47), "UDP file range rejected");
    require(!protocol::isValidType(0) && !protocol::isValidType(18) &&
            !protocol::isValidType(31) && !protocol::isValidType(38) &&
            !protocol::isValidType(48) && !protocol::isValidType(255),
            "unknown packet type accepted");
    require(protocol::isTcpType(protocol::Type::AUTH), "AUTH must be TCP");
    require(protocol::isUdpType(protocol::Type::HEARTBEAT), "HEARTBEAT must be UDP");
    require(!protocol::isUdpType(protocol::Type::MSG), "MSG must not be UDP");
}
void testByteReaderWriterAndUtf8() {
    protocol::ByteWriter writer;
    writer.u8(0xFE); writer.u16(0xABCD); writer.u32(0x12345678);
    writer.u64(0x0102030405060708ULL);
    require(writer.str("Tiếng Việt 🌐"), "valid UTF-8 string rejected");
    protocol::ByteReader reader(writer.data());
    std::uint8_t a = 0; std::uint16_t b = 0; std::uint32_t c = 0; std::uint64_t d = 0;
    std::string s;
    require(reader.u8(a) && reader.u16(b) && reader.u32(c) && reader.u64(d) && reader.str(s),
            "ByteReader failed on valid data");
    require(a == 0xFE && b == 0xABCD && c == 0x12345678 &&
            d == 0x0102030405060708ULL && s == "Tiếng Việt 🌐" && reader.done(),
            "ByteWriter/ByteReader round trip mismatch");
    const protocol::Bytes truncatedBytes{0x01};
    protocol::ByteReader truncated(truncatedBytes);
    require(!truncated.u16(b), "truncated integer accepted");
    const protocol::Bytes invalidStringBytes{0x00, 0x02, 0xC0, 0xAF};
    protocol::ByteReader invalidString(invalidStringBytes);
    require(!invalidString.str(s), "overlong UTF-8 string accepted");
    require(protocol::validUtf8("ASCII and 日本語"), "valid UTF-8 rejected");
    require(!protocol::validUtf8(std::string("\x80", 1)), "stray continuation byte accepted");
    require(!protocol::validUtf8(std::string("\xC0\xAF", 2)), "overlong encoding accepted");
    require(!protocol::validUtf8(std::string("\xED\xA0\x80", 3)), "UTF-8 surrogate accepted");
    require(!protocol::validUtf8(std::string("\xF4\x90\x80\x80", 4)), "code point above Unicode range accepted");
    require(!protocol::validUtf8(std::string("\xE2\x82", 2)), "truncated UTF-8 accepted");
}
void testFilenameValidation() {
    require(protocol::validFilename("report.txt"), "normal filename rejected");
    require(protocol::validFilename("báo-cáo.txt"), "UTF-8 filename rejected");
    const std::vector<std::string> invalid = {
        "", ".", "..", "../secret", "folder/file", "folder\\file", "C:secret",
        std::string("bad\nname"), std::string("bad\x7F", 4),
        std::string(256, 'x'), std::string("\xFF", 1)
    };
    for (const auto& name : invalid)
        require(!protocol::validFilename(name), "unsafe filename accepted");
}
void testPacketMalformedAndLimits() {
    protocol::Packet packet{protocol::Type::MSG, 7, {1, 2, 3}};
    protocol::Bytes wire;
    require(protocol::encodePacket(packet, wire), "valid packet encode failed");
    protocol::Packet decoded;
    require(!protocol::decodePacket(nullptr, wire.size(), decoded), "null packet pointer accepted");
    require(!protocol::decodePacket(wire.data(), protocol::HEADER_SIZE - 1, decoded),
            "short header accepted");
    auto bad = wire; bad[0] ^= 1;
    require(!protocol::decodePacket(bad, decoded), "bad magic accepted");
    bad = wire; bad[4] = protocol::VERSION + 1;
    require(!protocol::decodePacket(bad, decoded), "unsupported version accepted");
    bad = wire; bad[5] = 18;
    require(!protocol::decodePacket(bad, decoded), "unknown type accepted");
    bad = wire; bad[13] = 4;
    require(!protocol::decodePacket(bad, decoded), "declared length mismatch accepted");
    protocol::Packet invalid{static_cast<protocol::Type>(18), 0, {}};
    require(!protocol::encodePacket(invalid, wire), "invalid type encoded");
    invalid = {protocol::Type::MSG, 0,
               protocol::Bytes(protocol::MAX_TCP_PAYLOAD + 1, 0)};
    require(!protocol::encodePacket(invalid, wire), "oversized TCP payload encoded");
    packet = {protocol::Type::HEARTBEAT, 9, {1}};
    require(protocol::encodePacket(packet, wire), "valid UDP packet encode failed");
    require(protocol::decodeDatagram(wire, decoded), "valid UDP datagram rejected");
    packet.type = protocol::Type::MSG;
    require(protocol::encodePacket(packet, wire), "TCP packet encode failed");
    require(!protocol::decodeDatagram(wire, decoded), "TCP packet accepted as UDP");
}
void testParserErrorRecoveryAndSequenceConcurrency() {
    protocol::TcpPacketParser parser;
    protocol::Packet packet{protocol::Type::MSG, 1, {1}};
    protocol::Bytes wire;
    require(protocol::encodePacket(packet, wire), "packet encode failed");
    std::vector<protocol::Packet> out;
    auto invalid = wire; invalid[5] = static_cast<std::uint8_t>(protocol::Type::HEARTBEAT);
    require(!parser.feed(invalid.data(), invalid.size(), out),
            "TCP parser accepted UDP type on TCP stream");
    require(parser.feed(wire.data(), wire.size(), out),
            "parser did not recover after reset on malformed input");
    require(out.size() == 1, "parser failed to emit packet after reset");

    protocol::SequenceCounter counter;
    std::vector<std::uint32_t> values;
    std::mutex mutex;
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&] {
            std::vector<std::uint32_t> local;
            for (int i = 0; i < 1000; ++i) local.push_back(counter.next());
            std::lock_guard<std::mutex> lock(mutex);
            values.insert(values.end(), local.begin(), local.end());
        });
    }
    for (auto& worker : workers) worker.join();
    std::sort(values.begin(), values.end());
    require(values.size() == 8000 && values.front() == 1 && values.back() == 8000,
            "sequence counter lost or duplicated values under concurrency");
}
}
int main() {
    try {
        testTypeClassification();
        testByteReaderWriterAndUtf8();
        testFilenameValidation();
        testPacketMalformedAndLimits();
        testParserErrorRecoveryAndSequenceConcurrency();
        std::cout << "Protocol boundary tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Protocol boundary tests failed: " << e.what() << '\n';
        return 1;
    }
}