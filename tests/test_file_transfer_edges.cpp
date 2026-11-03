#include "../clients/file_sender.h"
#include "../clients/file_receiver.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
            ("dcap-file-edge-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};
void writeBytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    require(static_cast<bool>(out), "could not create fixture");
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(out), "could not write fixture");
}
std::vector<std::uint8_t> readBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(static_cast<bool>(in), "could not open output fixture");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
fileproto::Offer makeOffer(const fs::path& root, const std::string& name,
                           const std::vector<std::uint8_t>& bytes, std::uint8_t seed) {
    const auto source = root / ("hash-" + name);
    writeBytes(source, bytes);
    fileproto::Offer offer;
    offer.id.fill(seed);
    offer.recipient = "receiver";
    offer.filename = name;
    offer.size = bytes.size();
    offer.chunk_size = protocol::FILE_CHUNK_SIZE;
    offer.chunks = fileproto::chunkCount(offer.size);
    offer.sha256 = fileproto::sha256File(source.string());
    return offer;
}
protocol::Packet startPacket(const fileproto::Offer& offer) {
    fileproto::Start start{offer.id, offer.size, offer.chunk_size, offer.chunks, offer.sha256};
    protocol::Packet p; p.type = protocol::Type::FILE_START;
    require(fileproto::encode(start, p.payload), "could not encode start");
    return p;
}
protocol::Packet chunkPacket(const fileproto::Offer& offer, std::uint32_t index,
                             const std::vector<std::uint8_t>& bytes) {
    fileproto::Chunk chunk{offer.id, index, bytes};
    protocol::Packet p; p.type = protocol::Type::FILE_CHUNK;
    require(fileproto::encode(chunk, p.payload), "could not encode chunk");
    return p;
}
protocol::Packet endPacket(const fileproto::Offer& offer) {
    fileproto::End end{offer.id, offer.chunks, offer.size, offer.sha256};
    protocol::Packet p; p.type = protocol::Type::FILE_END;
    require(fileproto::encode(end, p.payload), "could not encode end");
    return p;
}
void testSenderValidationAndDecision() {
    TempDir temp;
    const auto file = temp.path / "sender.bin";
    writeBytes(file, {1, 2, 3});
    protocol::SequenceCounter sequence;
    bool threw = false;
    try { filetransfer::FileSender missing((temp.path / "missing").string(), "bob", sequence); }
    catch (const std::exception&) { threw = true; }
    require(threw, "missing source file must be rejected");
    threw = false;
    try { filetransfer::FileSender directory(temp.path.string(), "bob", sequence); }
    catch (const std::exception&) { threw = true; }
    require(threw, "directory source must be rejected");
    threw = false;
    try { filetransfer::FileSender invalid(file.string(), "", sequence); }
    catch (const std::exception&) { threw = true; }
    require(threw, "empty recipient must be rejected");

    filetransfer::FileSender sender(file.string(), "bob", sequence);
    require(sender.makeOffer(77).type == protocol::Type::FILE_OFFER,
            "sender must produce FILE_OFFER");
    fileproto::Decision wrong;
    wrong.id.fill(0xEE); wrong.accepted = true;
    protocol::Packet p; p.type = protocol::Type::FILE_DECISION;
    require(fileproto::encode(wrong, p.payload), "could not encode wrong decision");
    require(!sender.handleDecision(p), "decision for another transfer must be ignored");
    wrong.id = sender.offer().id; wrong.accepted = false; wrong.reason = "declined";
    require(fileproto::encode(wrong, p.payload), "could not encode rejection");
    require(sender.handleDecision(p), "valid rejection must be handled");
    require(sender.state() == filetransfer::SenderState::FAILED,
            "rejected sender must enter FAILED");
}
void testSenderStartRetransmissionAndChunkWindow() {
    TempDir temp;
    const auto file = temp.path / "window.bin";
    std::vector<std::uint8_t> data(protocol::FILE_CHUNK_SIZE * 2 + 7, 0x5A);
    writeBytes(file, data);
    protocol::SequenceCounter sequence;
    filetransfer::FileSender sender(file.string(), "bob", sequence);
    fileproto::Decision decision; decision.id = sender.offer().id; decision.accepted = true;
    protocol::Packet p; p.type = protocol::Type::FILE_DECISION;
    require(fileproto::encode(decision, p.payload) && sender.handleDecision(p),
            "valid sender acceptance failed");

    const auto t0 = filetransfer::TimePoint{} + std::chrono::seconds(1);
    auto start = sender.poll(t0);
    require(start.size() == 1 && start[0].type == protocol::Type::FILE_START,
            "sender must initiate UDP transfer");
    auto retry = sender.poll(t0 + std::chrono::milliseconds(protocol::INITIAL_RTO_MS));
    require(retry.size() == 1 && retry[0].sequence == start[0].sequence,
            "FILE_START retry must reuse original sequence");

    fileproto::Accept accept{sender.offer().id, true, 2, 0, ""};
    p = {}; p.type = protocol::Type::FILE_ACCEPT_UDP;
    require(fileproto::encode(accept, p.payload), "could not encode UDP accept");
    sender.onPacket(p);
    require(sender.state() == filetransfer::SenderState::SENDING,
            "valid UDP accept must start sending");

    const auto packets = sender.poll(t0 + std::chrono::seconds(2));
    std::vector<fileproto::Chunk> chunks;
    for (const auto& packet : packets) {
        if (packet.type == protocol::Type::FILE_CHUNK) {
            fileproto::Chunk c;
            require(fileproto::decode(packet.payload, c), "sender emitted malformed chunk");
            chunks.push_back(c);
        }
    }
    require(chunks.size() == 2, "receiver window must limit outstanding chunks to two");
    fileproto::Ack ack;
    ack.id = sender.offer().id;
    ack.base = 0;
    ack.bitmap = {0x02};
    ack.window = 2;
    p = {}; p.type = protocol::Type::FILE_ACK;
    require(fileproto::encode(ack, p.payload), "could not encode selective ACK");
    sender.onPacket(p);
    const auto acknowledged = protocol::FILE_CHUNK_SIZE;
    require(sender.acknowledgedBytes() == acknowledged,
            "selective ACK byte count is incorrect");
    sender.onPacket(p);
    require(sender.acknowledgedBytes() == acknowledged,
            "duplicate ACK must not count bytes twice");
}
void testSenderTimeoutAndCancel() {
    TempDir temp;
    const auto file = temp.path / "timeout.bin";
    writeBytes(file, {1});
    protocol::SequenceCounter sequence;
    filetransfer::FileSender sender(file.string(), "bob", sequence);
    fileproto::Decision decision; decision.id = sender.offer().id; decision.accepted = true;
    protocol::Packet p; p.type = protocol::Type::FILE_DECISION;
    require(fileproto::encode(decision, p.payload) && sender.handleDecision(p),
            "could not accept timeout fixture");
    auto now = filetransfer::TimePoint{} + std::chrono::seconds(10);
    require(sender.poll(now).size() == 1, "initial FILE_START missing");
    for (std::uint32_t i = 0; i < protocol::MAX_RETRANSMISSIONS; ++i) {
        now += std::chrono::milliseconds(std::min(
            protocol::INITIAL_RTO_MS << std::min<std::uint32_t>(i, 3),
            protocol::MAX_RTO_MS));
        sender.poll(now);
    }
    now += std::chrono::milliseconds(protocol::MAX_RTO_MS);
    sender.poll(now);
    require(sender.state() == filetransfer::SenderState::FAILED,
            "exhausted FILE_START retries must fail transfer");

    filetransfer::FileSender cancelled(file.string(), "bob", sequence);
    cancelled.cancel();
    require(cancelled.state() == filetransfer::SenderState::FAILED,
            "local cancel must stop sender");
    require(!cancelled.lastError().empty(), "cancel should retain failure reason");
}
void testReceiverOutOfOrderDuplicateAndCollision() {
    TempDir temp;
    std::vector<std::uint8_t> data(protocol::FILE_CHUNK_SIZE + 3);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i);
    const auto offer = makeOffer(temp.path, "result.bin", data, 0x21);
    protocol::SequenceCounter sequence;
    const auto receiveDir = temp.path / "received";
    fs::create_directories(receiveDir);
    writeBytes(receiveDir / "result.bin", {9, 9});

    filetransfer::FileReceiver receiver(offer, receiveDir, sequence);
    auto replies = receiver.onPacket(startPacket(offer));
    require(receiver.state() == filetransfer::ReceiverState::RECEIVING,
            "valid FILE_START must open receiver");
    require(replies.size() == 1 && replies[0].type == protocol::Type::FILE_ACCEPT_UDP,
            "receiver must acknowledge FILE_START");

    const std::vector<std::uint8_t> second(data.begin() + protocol::FILE_CHUNK_SIZE, data.end());
    const std::vector<std::uint8_t> first(data.begin(), data.begin() + protocol::FILE_CHUNK_SIZE);
    replies = receiver.onPacket(chunkPacket(offer, 1, second));
    require(receiver.receivedBytes() == second.size(), "out-of-order chunk not recorded");
    receiver.onPacket(chunkPacket(offer, 1, second));
    require(receiver.receivedBytes() == second.size(), "duplicate chunk counted twice");
    replies = receiver.onPacket(endPacket(offer));
    require(receiver.state() == filetransfer::ReceiverState::RECEIVING,
            "early FILE_END must wait for missing chunks");
    replies = receiver.onPacket(chunkPacket(offer, 0, first));
    require(receiver.state() == filetransfer::ReceiverState::COMPLETED,
            "receiver must finalize after final missing chunk arrives");
    require(!replies.empty() && replies.back().type == protocol::Type::FILE_COMPLETE,
            "receiver must emit FILE_COMPLETE");
    require(readBytes(receiver.completedPath()) == data, "reassembled bytes mismatch");
    require(receiver.completedPath().filename() == "result (1).bin",
            "existing destination must be preserved with collision-safe name");
    require(readBytes(receiveDir / "result.bin") == std::vector<std::uint8_t>({9, 9}),
            "receiver overwrote pre-existing destination");
}
void testReceiverRejectsBadMetadataAndChunks() {
    TempDir temp;
    const std::vector<std::uint8_t> data{1, 2, 3};
    auto offer = makeOffer(temp.path, "bad.bin", data, 0x31);
    protocol::SequenceCounter sequence;
    auto wrongStart = startPacket(offer);
    fileproto::Start start;
    require(fileproto::decode(wrongStart.payload, start), "start decode failed");
    ++start.size;
    require(fileproto::encode(start, wrongStart.payload), "wrong start encode failed");
    filetransfer::FileReceiver metadataReceiver(offer, temp.path / "meta", sequence);
    auto reply = metadataReceiver.onPacket(wrongStart);
    require(metadataReceiver.state() == filetransfer::ReceiverState::FAILED,
            "mismatched start metadata must fail receiver");
    require(!reply.empty() && reply.back().type == protocol::Type::FILE_ERROR_UDP,
            "metadata failure must emit FILE_ERROR_UDP");

    filetransfer::FileReceiver chunkReceiver(offer, temp.path / "chunk", sequence);
    chunkReceiver.onPacket(startPacket(offer));
    reply = chunkReceiver.onPacket(chunkPacket(offer, 0, {1, 2}));
    require(chunkReceiver.state() == filetransfer::ReceiverState::FAILED,
            "short final chunk must fail receiver");
    require(!reply.empty() && reply.back().type == protocol::Type::FILE_ERROR_UDP,
            "invalid chunk must emit transfer error");

    filetransfer::FileReceiver indexReceiver(offer, temp.path / "index", sequence);
    indexReceiver.onPacket(startPacket(offer));
    reply = indexReceiver.onPacket(chunkPacket(offer, 1, {1}));
    require(indexReceiver.state() == filetransfer::ReceiverState::FAILED,
            "out-of-range chunk index must fail receiver");
}
void testReceiverHashFailureAndCancelCleanup() {
    TempDir temp;
    const std::vector<std::uint8_t> data{4, 5, 6, 7};
    auto offer = makeOffer(temp.path, "hash-fail.bin", data, 0x41);
    offer.sha256[0] ^= 0xFF;
    protocol::SequenceCounter sequence;
    const auto directory = temp.path / "hash-receive";
    filetransfer::FileReceiver receiver(offer, directory, sequence);
    receiver.onPacket(startPacket(offer));
    receiver.onPacket(chunkPacket(offer, 0, data));
    const auto reply = receiver.onPacket(endPacket(offer));
    require(receiver.state() == filetransfer::ReceiverState::FAILED,
            "wrong final digest must fail receiver");
    require(!reply.empty() && reply.back().type == protocol::Type::FILE_COMPLETE,
            "integrity failure must report FILE_COMPLETE with failure status");
    require(!fs::exists(directory / (fileproto::idHex(offer.id) + ".part")),
            "failed partial file must be removed");

    auto cancelOffer = makeOffer(temp.path, "cancel.bin", data, 0x42);
    const auto cancelDir = temp.path / "cancel-receive";
    filetransfer::FileReceiver cancelled(cancelOffer, cancelDir, sequence);
    cancelled.onPacket(startPacket(cancelOffer));
    const auto partial = cancelDir / (fileproto::idHex(cancelOffer.id) + ".part");
    require(fs::exists(partial), "partial file should exist during transfer");
    cancelled.cancel();
    require(cancelled.state() == filetransfer::ReceiverState::FAILED,
            "receiver cancel must enter FAILED");
    require(!fs::exists(partial), "receiver cancel must remove owned partial file");
}
void testEmptyFileAndPartialOwnership() {
    TempDir temp;
    const auto empty = makeOffer(temp.path, "empty.bin", {}, 0x51);
    protocol::SequenceCounter sequence;
    filetransfer::FileReceiver receiver(empty, temp.path / "empty-recv", sequence);
    receiver.onPacket(startPacket(empty));
    auto reply = receiver.onPacket(endPacket(empty));
    require(receiver.state() == filetransfer::ReceiverState::COMPLETED,
            "zero-byte file must complete");
    require(fs::file_size(receiver.completedPath()) == 0, "empty output has nonzero size");
    require(!reply.empty() && reply.back().type == protocol::Type::FILE_COMPLETE,
            "empty file must emit completion");

    auto offer = makeOffer(temp.path, "owned.bin", {8}, 0x52);
    const auto directory = temp.path / "owned-recv";
    fs::create_directories(directory);
    const auto preexisting = directory / (fileproto::idHex(offer.id) + ".part");
    writeBytes(preexisting, {7, 7});
    filetransfer::FileReceiver collision(offer, directory, sequence);
    reply = collision.onPacket(startPacket(offer));
    require(collision.state() == filetransfer::ReceiverState::FAILED,
            "existing .part path must reject second owner");
    require(readBytes(preexisting) == std::vector<std::uint8_t>({7, 7}),
            "receiver must not delete another transfer's partial file");
}
}
int main() {
    try {
        testSenderValidationAndDecision();
        testSenderStartRetransmissionAndChunkWindow();
        testSenderTimeoutAndCancel();
        testReceiverOutOfOrderDuplicateAndCollision();
        testReceiverRejectsBadMetadataAndChunks();
        testReceiverHashFailureAndCancelCleanup();
        testEmptyFileAndPartialOwnership();
        std::cout << "File transfer edge-case tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "File transfer edge-case tests failed: " << e.what() << '\n';
        return 1;
    }
}