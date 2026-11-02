#include "../protocol/packet.h"
#include "../protocol/message_payloads.h"
#include "../protocol/file_messages.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void testPacketRoundTrip() {
    protocol::Packet original;
    original.type = protocol::Type::MSG;
    original.sequence = 0x10203040;
    original.payload = {0x00, 0x01, 0x7f, 0x80, 0xff};

    protocol::Bytes wire;
    require(protocol::encodePacket(original, wire), "Could not encode DCAP packet");

    protocol::Packet decoded;
    require(protocol::decodePacket(wire, decoded), "Could not decode DCAP packet");
    require(decoded.type == original.type, "Packet type changed during round trip");
    require(decoded.sequence == original.sequence, "Packet sequence changed during round trip");
    require(decoded.payload == original.payload, "Packet payload changed during round trip");

    auto badMagic = wire;
    badMagic[0] ^= 0xff;
    require(!protocol::decodePacket(badMagic, decoded), "Invalid magic was accepted");

    auto truncated = wire;
    truncated.pop_back();
    require(!protocol::decodePacket(truncated, decoded), "Truncated packet was accepted");

    auto trailing = wire;
    trailing.push_back(0);
    require(!protocol::decodePacket(trailing, decoded), "Packet with trailing bytes was accepted");

    require(!protocol::decodeDatagram(wire, decoded),
        "TCP message type was accepted as a UDP datagram");
}

void testTcpStreamParser() {
    protocol::Packet first{protocol::Type::MSG, 1, {1, 2, 3}};
    protocol::Packet second{protocol::Type::PRIVATE_MSG, 2, {4, 5}};
    protocol::Bytes firstWire;
    protocol::Bytes secondWire;
    require(protocol::encodePacket(first, firstWire), "Could not encode first stream packet");
    require(protocol::encodePacket(second, secondWire), "Could not encode second stream packet");

    protocol::TcpPacketParser parser;
    std::vector<protocol::Packet> output;
    require(parser.feed(firstWire.data(), 5, output) && output.empty(),
        "Parser did not retain an incomplete TCP header");
    require(parser.feed(firstWire.data() + 5, firstWire.size() - 5, output),
        "Parser rejected the remaining TCP packet bytes");
    require(output.size() == 1 && output[0].sequence == 1,
        "Parser did not reconstruct a packet split across recv calls");

    protocol::Bytes combined = firstWire;
    combined.insert(combined.end(), secondWire.begin(), secondWire.end());
    parser.reset();
    output.clear();
    require(parser.feed(combined.data(), combined.size(), output),
        "Parser rejected coalesced TCP packets");
    require(output.size() == 2 && output[0].sequence == 1 && output[1].sequence == 2,
        "Parser did not extract multiple packets from one TCP read");
}

void testPayloadCodecs() {
    messages::Auth auth{"test-user"};
    protocol::Bytes bytes;
    require(messages::encode(auth, bytes), "Could not encode AUTH payload");
    messages::Auth decodedAuth;
    require(messages::decode(bytes, decodedAuth), "Could not decode AUTH payload");
    require(decodedAuth.username == auth.username, "AUTH username changed");

    messages::ChatRequest chat{77, "Hello DCAP"};
    require(messages::encode(chat, bytes), "Could not encode MSG payload");
    messages::ChatRequest decodedChat;
    require(messages::decode(bytes, decodedChat), "Could not decode MSG payload");
    require(decodedChat.client_id == chat.client_id && decodedChat.text == chat.text,
        "MSG payload changed during round trip");

    messages::ChatRequest invalidChat{1, ""};
    require(!messages::encode(invalidChat, bytes), "Empty chat text was accepted");
}

void testFileMessageCodec() {
    fileproto::Offer offer;
    offer.id[0] = 0x42;
    offer.recipient = "receiver";
    offer.filename = "sample.bin";
    offer.size = 3;
    offer.chunk_size = protocol::FILE_CHUNK_SIZE;
    offer.chunks = 1;
    offer.sha256[0] = 0xab;

    protocol::Bytes bytes;
    require(fileproto::encode(offer, bytes), "Could not encode FILE_OFFER");
    fileproto::Offer decoded;
    require(fileproto::decode(bytes, decoded), "Could not decode FILE_OFFER");
    require(decoded.id == offer.id && decoded.recipient == offer.recipient &&
            decoded.filename == offer.filename && decoded.size == offer.size &&
            decoded.chunks == offer.chunks && decoded.sha256 == offer.sha256,
        "FILE_OFFER metadata changed during round trip");

    auto invalid = offer;
    invalid.filename = "../outside.bin";
    require(!fileproto::encode(invalid, bytes), "Unsafe filename was accepted");

    fileproto::Chunk chunk;
    chunk.id = offer.id;
    chunk.index = 0;
    chunk.bytes = {0x10, 0x20, 0x30};
    require(fileproto::encode(chunk, bytes), "Could not encode FILE_CHUNK");
    fileproto::Chunk decodedChunk;
    require(fileproto::decode(bytes, decodedChunk), "Could not decode FILE_CHUNK");
    require(decodedChunk.id == chunk.id && decodedChunk.index == chunk.index &&
            decodedChunk.bytes == chunk.bytes,
        "FILE_CHUNK changed during round trip");
}
} // namespace

int main() {
    try {
        testPacketRoundTrip();
        testTcpStreamParser();
        testPayloadCodecs();
        testFileMessageCodec();
        std::cout << "DCAP protocol codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DCAP protocol tests failed: " << error.what() << '\n';
        return 1;
    }
}
