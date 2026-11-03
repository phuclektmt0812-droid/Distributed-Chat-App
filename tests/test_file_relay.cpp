#include "test_helpers.h"
#include "../server/file_relay.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
namespace {
fs::path makeSource(const fs::path& root, const std::string& name) {
    const auto path = root / name;
    std::ofstream out(path, std::ios::binary);
    test::require(static_cast<bool>(out), "could not create relay fixture");
    const std::string bytes(2 * 1024 * 1024, 'R');
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    test::require(static_cast<bool>(out), "could not write relay fixture");
    return path;
}
void waitForOffers(DcapClient& receiver, std::size_t count) {
    std::size_t seen = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (seen < count && std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : receiver.takeEvents())
            if (event.type == "FILE_OFFER") ++seen;
        if (seen < count) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    test::require(seen == count, "receiver did not receive expected number of FILE_OFFER events");
}
void testReceiverAutomaticallyAcceptsOffer() {
    const auto root = fs::temp_directory_path() /
        ("dcap-relay-auto-accept-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto source = makeSource(root, "automatic.bin");
    {
        test::TestServer server;
        DcapClient sender("127.0.0.1"), receiver("127.0.0.1");
        test::connectAndLogin(sender, "relay-sender-auto");
        test::connectAndLogin(receiver, "relay-receiver-auto");
        const auto id = sender.sendFile("relay-receiver-auto", source);
        test::require(!id.empty(), "could not submit automatic transfer");
        const auto offer = test::waitForEvent(receiver, "FILE_OFFER");
        test::require(offer.data.value("transferId", std::string()) == id,
                      "wrong offer delivered to recipient");
        test::require(offer.data.value("autoAccepted", false),
                      "recipient did not auto-accept the offer");

        const auto senderDone = test::waitForEvent(sender, "FILE_COMPLETE", std::chrono::seconds(30));
        const auto receiverDone = test::waitForEvent(receiver, "FILE_COMPLETE", std::chrono::seconds(30));
        test::require(senderDone.data.value("transferId", std::string()) == id &&
                      receiverDone.data.value("transferId", std::string()) == id,
                      "automatically accepted transfer did not complete on both clients");
    }
    std::error_code ec; fs::remove_all(root, ec);
}
void testTransferExpires() {
    const auto root = fs::temp_directory_path() /
        ("dcap-relay-expire-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto source = makeSource(root, "expire.bin");
    {
        test::TestServer server;
        DcapClient sender("127.0.0.1"), receiver("127.0.0.1");
        test::connectAndLogin(sender, "expire-sender");
        test::connectAndLogin(receiver, "expire-receiver");
        const auto id = sender.sendFile("expire-receiver", source);
        test::require(!id.empty(), "could not submit expiring transfer");
        test::waitForEvent(receiver, "FILE_OFFER");
        const auto future = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()) + 121000;
        server.server().fileRelay().expire(future);
        const auto result = test::waitForEvent(sender, "FILE_RESULT");
        test::require(result.data.value("transferId", std::string()) == id &&
                      result.data.value("status", -1) == 0,
                      "expired transfer did not report failure");
    }
    std::error_code ec; fs::remove_all(root, ec);
}
void testDisconnectCancelsPendingTransfer() {
    const auto root = fs::temp_directory_path() /
        ("dcap-relay-disconnect-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto source = makeSource(root, "disconnect.bin");
    {
        test::TestServer server;
        DcapClient sender("127.0.0.1"), receiver("127.0.0.1");
        test::connectAndLogin(sender, "disconnect-sender");
        test::connectAndLogin(receiver, "disconnect-receiver");
        const auto id = sender.sendFile("disconnect-receiver", source);
        test::require(!id.empty(), "could not submit disconnect transfer");
        test::waitForEvent(receiver, "FILE_OFFER");
        receiver.disconnect();
        const auto result = test::waitForEvent(sender, "FILE_RESULT");
        test::require(result.data.value("transferId", std::string()) == id &&
                      result.data.value("status", -1) != 1,
                      "peer disconnect did not cancel pending transfer");
    }
    std::error_code ec; fs::remove_all(root, ec);
}
void testPerSenderTransferLimit() {
    const auto root = fs::temp_directory_path() /
        ("dcap-relay-limit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto source = makeSource(root, "limit.bin");
    {
        test::TestServer server;
        DcapClient sender("127.0.0.1"), receiver("127.0.0.1");
        test::connectAndLogin(sender, "limit-sender");
        test::connectAndLogin(receiver, "limit-receiver");
        for (int i = 0; i < 4; ++i) {
            const auto id = sender.sendFile("limit-receiver", source);
            test::require(!id.empty(), "one of the first four transfer offers failed locally");
        }
        waitForOffers(receiver, 4);
        const auto fifth = sender.sendFile("limit-receiver", source);
        test::require(!fifth.empty(), "fifth offer should reach server for rejection");
        const auto error = test::waitForEvent(sender, "ERROR");
        test::require(error.data.value("code", 0) == protocol::ERR_TRANSFER_BUSY,
                      "server did not enforce per-sender transfer capacity");
    }
    std::error_code ec; fs::remove_all(root, ec);
}
}
int main() {
    try {
        testReceiverAutomaticallyAcceptsOffer();
        testTransferExpires();
        testDisconnectCancelsPendingTransfer();
        testPerSenderTransferLimit();
        std::cout << "File relay lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "File relay tests failed: " << e.what() << '\n';
        return 1;
    }
}