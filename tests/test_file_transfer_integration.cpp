#include "test_helpers.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
void writeFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    test::require(static_cast<bool>(out), "could not create integration source file");
    if (!bytes.empty())
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    test::require(static_cast<bool>(out), "could not write integration source file");
}
std::vector<std::uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    test::require(static_cast<bool>(in), "could not open integrated received file");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}
int main() {
    const auto originalDirectory = fs::current_path();
    const auto root = fs::temp_directory_path() /
        ("dcap-e2e-file-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root);
        fs::current_path(root);
        const auto source = root / "source.bin";
        std::vector<std::uint8_t> expected(25000);
        for (std::size_t i = 0; i < expected.size(); ++i)
            expected[i] = static_cast<std::uint8_t>((i * 29) & 0xFF);
        writeFile(source, expected);

        {
            test::TestServer server;
            DcapClient sender("127.0.0.1");
            DcapClient receiver("127.0.0.1");
            test::connectAndLogin(sender, "file-sender");
            test::connectAndLogin(receiver, "file-receiver");

            const auto udpReady = [&server]() {
                const auto senderSession = server.server().users().find("file-sender");
                const auto receiverSession = server.server().users().find("file-receiver");
                return senderSession && receiverSession &&
                    senderSession->udpRegistered && receiverSession->udpRegistered;
            };
            const auto registrationDeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!udpReady() && std::chrono::steady_clock::now() < registrationDeadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            test::require(udpReady(),
                          "both endpoints must register UDP before transfer");

            const auto transferId = sender.sendFile("file-receiver", source);
            test::require(!transferId.empty(), "sender could not create FILE_OFFER");
            const auto offer = test::waitForEvent(receiver, "FILE_OFFER");
            test::require(offer.data.value("transferId", std::string()) == transferId,
                          "receiver saw a different transfer ID");
            test::require(offer.data.value("filename", std::string()) == "source.bin",
                          "receiver saw wrong filename");
            test::require(receiver.decideFile(transferId, true),
                          "receiver could not accept transfer");

            const auto senderDone = test::waitForEvent(sender, "FILE_COMPLETE",
                                                        std::chrono::seconds(30));
            test::require(senderDone.data.value("transferId", std::string()) == transferId,
                          "sender completion event has wrong transfer ID");
            const auto receiverDone = test::waitForEvent(receiver, "FILE_COMPLETE",
                                                          std::chrono::seconds(30));
            test::require(receiverDone.data.value("transferId", std::string()) == transferId,
                          "receiver completion event has wrong transfer ID");

            const auto received = root / "received_files" / "source.bin";
            test::require(fs::exists(received), "end-to-end output file was not created");
            test::require(readFile(received) == expected,
                          "end-to-end relayed file bytes differ from source");
            sender.disconnect();
            receiver.disconnect();
        }
        fs::current_path(originalDirectory);
        std::error_code ec; fs::remove_all(root, ec);
        std::cout << "End-to-end UDP-relayed file transfer test passed\n";
        return 0;
    } catch (const std::exception& e) {
        fs::current_path(originalDirectory);
        std::error_code ec; fs::remove_all(root, ec);
        std::cerr << "File transfer integration test failed: " << e.what() << '\n';
        return 1;
    }
}