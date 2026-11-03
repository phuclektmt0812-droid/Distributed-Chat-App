#include "../clients/file_sender.h"
#include "../clients/file_receiver.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    require(static_cast<bool>(output), "Could not create source test file");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "Could not write source test file");
}
std::vector<std::uint8_t> readBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Could not open received file");
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
void testSenderReceiverRoundTrip() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("dcap-file-test-" + std::to_string(stamp));
    const auto sourcePath = root / "source.bin";
    const auto receiveDirectory = root / "received";
    std::filesystem::create_directories(root);

    std::vector<std::uint8_t> original(3500);
    for (std::size_t i = 0; i < original.size(); ++i)
        original[i] = static_cast<std::uint8_t>((i * 37) & 0xff);
    writeBytes(sourcePath, original);

    try {
        protocol::SequenceCounter senderSequence;
        protocol::SequenceCounter receiverSequence;
        const std::string displayFilename =
            "L\\xC3\\xAA Ho\\xC3\\xA0ng Ph\\xC3\\xBAc Lab6Report.pdf";
        filetransfer::FileSender sender(
            sourcePath.string(), "receiver", senderSequence, displayFilename);
        require(sender.offer().filename == displayFilename,
                "Sender did not preserve the original UTF-8 filename");
        const auto offerPacket = sender.makeOffer(1);
        require(offerPacket.type == protocol::Type::FILE_OFFER,
                "Sender did not create FILE_OFFER");

        fileproto::Decision decision;
        decision.id = sender.offer().id;
        decision.accepted = true;
        protocol::Packet decisionPacket;
        decisionPacket.type = protocol::Type::FILE_DECISION;
        require(fileproto::encode(decision, decisionPacket.payload),
                "Could not encode FILE_DECISION");
        require(sender.handleDecision(decisionPacket),
                "Sender rejected a valid FILE_DECISION");

        filetransfer::FileReceiver receiver(
            sender.offer(), receiveDirectory, receiverSequence);
        const auto now = filetransfer::TimePoint{} + std::chrono::seconds(10);
        const auto startPackets = sender.poll(now);
        require(startPackets.size() == 1 &&
                startPackets.front().type == protocol::Type::FILE_START,
                "Sender did not emit FILE_START");

        const auto readyPackets = receiver.onPacket(startPackets.front());
        require(readyPackets.size() == 1 &&
                readyPackets.front().type == protocol::Type::FILE_ACCEPT_UDP,
                "Receiver did not accept a valid FILE_START");
        sender.onPacket(readyPackets.front());

        const auto dataPackets = sender.poll(now + std::chrono::milliseconds(1));
        require(!dataPackets.empty(), "Sender emitted no file data");

        bool sawEnd = false;
        bool sawChunk = false;
        bool sawComplete = false;
        for (const auto& packet : dataPackets) {
            if (packet.type == protocol::Type::FILE_CHUNK) sawChunk = true;
            else if (packet.type == protocol::Type::FILE_END) sawEnd = true;
            else continue;

            const auto responses = receiver.onPacket(packet);
            for (const auto& response : responses) {
                if (response.type == protocol::Type::FILE_ACK) {
                    sender.onPacket(response);
                } else if (response.type == protocol::Type::FILE_COMPLETE) {
                    sawComplete = true;
                    sender.onPacket(response);
                }
            }
        }

        require(sawChunk, "Sender did not emit FILE_CHUNK packets");
        require(sawEnd, "Sender did not emit FILE_END");
        require(sawComplete, "Receiver did not report verified completion");
        require(sender.state() == filetransfer::SenderState::COMPLETED,
                "Sender did not enter COMPLETED state");
        require(receiver.state() == filetransfer::ReceiverState::COMPLETED,
                "Receiver did not enter COMPLETED state");
        require(readBytes(receiver.completedPath()) == original,
                "Received file bytes do not match the original file");
        require(receiver.completedPath().filename().u8string() == displayFilename,
                "Receiver did not preserve the UTF-8 filename");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
void testInvalidOfferMetadata() {
    fileproto::Offer invalid;
    invalid.recipient = "receiver";
    invalid.filename = "../bad.bin";
    invalid.size = 1;
    invalid.chunk_size = protocol::FILE_CHUNK_SIZE;
    invalid.chunks = 1;
    protocol::Bytes encoded;
    require(!fileproto::encode(invalid, encoded),
            "FILE_OFFER with a path-traversal filename was accepted");
}
} // namespace

int main() {
    try {
        testInvalidOfferMetadata();
        testSenderReceiverRoundTrip();
        std::cout << "DCAP file-transfer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DCAP file-transfer tests failed: " << error.what() << '\n';
        return 1;
    }
}
