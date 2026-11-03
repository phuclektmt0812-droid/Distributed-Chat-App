#include "../protocol/file_messages.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<class T>
void roundTrip(const T& input, T& output, const char* message) {
    protocol::Bytes bytes;
    require(fileproto::encode(input, bytes), message);
    require(fileproto::decode(bytes, output), message);
}
fileproto::Offer validOffer() {
    fileproto::Offer p;
    p.id.fill(0x11); p.recipient = "receiver"; p.filename = "archive.bin";
    p.size = protocol::FILE_CHUNK_SIZE + 1; p.chunk_size = protocol::FILE_CHUNK_SIZE;
    p.chunks = 2; p.sha256.fill(0xA5);
    return p;
}
void testAllRoundTrips() {
    auto offer = validOffer(); fileproto::Offer offerOut;
    roundTrip(offer, offerOut, "FILE_OFFER round trip failed");
    require(offerOut.id == offer.id && offerOut.filename == offer.filename &&
            offerOut.size == offer.size && offerOut.sha256 == offer.sha256,
            "FILE_OFFER changed");
    fileproto::Decision decision{offer.id, true, 0, "accepted"}, decisionOut;
    roundTrip(decision, decisionOut, "FILE_DECISION round trip failed");
    require(decisionOut.accepted && decisionOut.reason == "accepted", "FILE_DECISION changed");
    fileproto::Start start{offer.id, offer.size, offer.chunk_size, offer.chunks, offer.sha256}, startOut;
    roundTrip(start, startOut, "FILE_START round trip failed");
    require(startOut.chunks == offer.chunks && startOut.size == offer.size, "FILE_START changed");
    fileproto::Accept accept{offer.id, true, 8, 0, ""}, acceptOut;
    roundTrip(accept, acceptOut, "FILE_ACCEPT_UDP round trip failed");
    require(acceptOut.ready && acceptOut.window == 8, "FILE_ACCEPT_UDP changed");
    fileproto::Chunk chunk{offer.id, 1, {1, 2, 3}}, chunkOut;
    roundTrip(chunk, chunkOut, "FILE_CHUNK round trip failed");
    require(chunkOut.index == 1 && chunkOut.bytes == chunk.bytes, "FILE_CHUNK changed");
    fileproto::Ack ack{offer.id, 1, {0x05, 0x02}, 16}, ackOut;
    roundTrip(ack, ackOut, "FILE_ACK round trip failed");
    require(ackOut.base == 1 && ackOut.bitmap == ack.bitmap && ackOut.window == 16,
            "FILE_ACK changed");
    fileproto::End end{offer.id, offer.chunks, offer.size, offer.sha256}, endOut;
    roundTrip(end, endOut, "FILE_END round trip failed");
    require(endOut.size == offer.size && endOut.sha256 == offer.sha256, "FILE_END changed");
    fileproto::Complete complete{offer.id, true, 0, offer.size, offer.sha256}, completeOut;
    roundTrip(complete, completeOut, "FILE_COMPLETE round trip failed");
    require(completeOut.verified && completeOut.size == offer.size, "FILE_COMPLETE changed");
    fileproto::Failure failure{offer.id, 208, "cancelled"}, failureOut;
    roundTrip(failure, failureOut, "FILE_ERROR round trip failed");
    require(failureOut.code == 208 && failureOut.detail == "cancelled", "FILE_ERROR changed");
    fileproto::Result result{offer.id, 2, 208, 0, "cancelled"}, resultOut;
    roundTrip(result, resultOut, "FILE_RESULT round trip failed");
    require(resultOut.status == 2 && resultOut.error_code == 208, "FILE_RESULT changed");
}
void testOfferAndSizeValidation() {
    protocol::Bytes bytes;
    auto offer = validOffer();
    auto bad = offer; bad.filename = "../secret";
    require(!fileproto::encode(bad, bytes), "path traversal filename accepted");
    bad = offer; bad.filename = "folder/name.bin";
    require(!fileproto::encode(bad, bytes), "path separator filename accepted");
    bad = offer; bad.recipient = std::string(33, 'x');
    require(!fileproto::encode(bad, bytes), "overlong recipient accepted");
    bad = offer; bad.recipient = "bad\nname";
    require(!fileproto::encode(bad, bytes), "control character recipient accepted");
    bad = offer; bad.chunk_size = 10;
    require(!fileproto::encode(bad, bytes), "unsupported chunk size accepted");
    bad = offer; ++bad.chunks;
    require(!fileproto::encode(bad, bytes), "inconsistent chunk count accepted");
    bad = offer; bad.size = protocol::MAX_FILE_SIZE + 1;
    require(!fileproto::encode(bad, bytes), "oversized file offer accepted");
    require(fileproto::chunkCount(0) == 0, "empty file should have zero chunks");
    require(fileproto::chunkCount(protocol::FILE_CHUNK_SIZE) == 1,
            "exact one-chunk size has wrong count");
    require(fileproto::chunkCount(protocol::FILE_CHUNK_SIZE + 1) == 2,
            "partial final chunk has wrong count");
    bool threw = false;
    try { (void)fileproto::chunkCount(protocol::MAX_FILE_SIZE + 1); }
    catch (const std::invalid_argument&) { threw = true; }
    require(threw, "chunkCount must throw above maximum file size");
}
void testControlFieldValidation() {
    protocol::Bytes bytes;
    auto offer = validOffer();
    fileproto::Accept accept{offer.id, true, 33, 0, ""};
    require(!fileproto::encode(accept, bytes), "window above MAX_WINDOW accepted");
    fileproto::Ack ack{offer.id, 0, {1,2,3,4,5}, 1};
    require(!fileproto::encode(ack, bytes), "ACK bitmap over four bytes accepted");
    ack.bitmap.clear(); ack.window = 33;
    require(!fileproto::encode(ack, bytes), "ACK window above MAX_WINDOW accepted");
    fileproto::Chunk chunk{offer.id, 0, {}};
    require(!fileproto::encode(chunk, bytes), "empty chunk accepted");
    chunk.bytes.assign(protocol::FILE_CHUNK_SIZE + 1, 1);
    require(!fileproto::encode(chunk, bytes), "oversized chunk accepted");
    fileproto::Result result{offer.id, 3, 0, 0, ""};
    require(!fileproto::encode(result, bytes), "unknown result status accepted");

    fileproto::Decision decision{offer.id, true, 0, ""};
    require(fileproto::encode(decision, bytes), "valid decision encode failed");
    bytes[16] = 2;
    fileproto::Decision decoded;
    require(!fileproto::decode(bytes, decoded), "non-boolean decision flag accepted");
    fileproto::Complete complete{offer.id, true, 0, 0, {}};
    require(fileproto::encode(complete, bytes), "valid complete encode failed");
    bytes[16] = 2;
    fileproto::Complete decodedComplete;
    require(!fileproto::decode(bytes, decodedComplete), "non-boolean complete flag accepted");
}
}
int main() {
    try {
        testAllRoundTrips();
        testOfferAndSizeValidation();
        testControlFieldValidation();
        std::cout << "File message codec tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "File message codec tests failed: " << e.what() << '\n';
        return 1;
    }
}