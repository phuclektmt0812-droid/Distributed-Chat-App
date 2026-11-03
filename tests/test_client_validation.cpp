#include "test_helpers.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        test::TestServer server;
        DcapClient disconnected("127.0.0.1");
        test::require(!disconnected.login("before-connect"),
                      "login before connect must fail");
        test::require(!disconnected.sendGlobal("hello"),
                      "global send before auth must fail");
        test::require(!disconnected.sendPrivate("bob", "hello"),
                      "private send before auth must fail");
        test::require(disconnected.sendFile("bob", "missing.bin").empty(),
                      "file send before auth must fail");
        test::require(!disconnected.decideFile("unknown", true),
                      "unknown file decision must fail");

        DcapClient client("127.0.0.1");
        test::require(client.connect(), "client connect failed: " + client.lastError());
        test::require(!client.login(""), "empty username accepted");
        test::require(!client.login(std::string(protocol::MAX_USERNAME_BYTES + 1, 'x')),
                      "overlong username accepted");
        test::require(client.login("validation-user"), "valid login request rejected");
        test::waitForEvent(client, "LOGIN_OK");

        test::require(!client.sendGlobal(""), "empty global message accepted");
        test::require(!client.sendGlobal(std::string(protocol::MAX_CHAT_BYTES + 1, 'x')),
                      "oversized global message accepted");
        test::require(!client.sendPrivate("", "hello"), "empty private recipient accepted");
        test::require(!client.sendPrivate("bob", ""), "empty private message accepted");
        test::require(!client.sendPrivate(std::string(33, 'x'), "hello"),
                      "overlong private recipient accepted");
        test::require(!client.sendPrivate("bob", std::string(protocol::MAX_CHAT_BYTES + 1, 'x')),
                      "oversized private message accepted");

        const auto root = std::filesystem::temp_directory_path() /
            ("dcap-client-validation-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        test::require(client.sendFile("bob", root / "missing.bin").empty(),
                      "missing file accepted by sendFile");
        test::require(client.sendFile("", root / "missing.bin").empty(),
                      "empty file recipient accepted");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);

        client.disconnect();
        client.disconnect();
        test::require(!client.connected() && !client.authenticated(),
                      "disconnect must clear client state");
        test::require(!client.sendGlobal("after-disconnect"),
                      "send after disconnect must fail");

        DcapClient invalidAddress("", protocol::TCP_PORT);
        test::require(!invalidAddress.connect(), "empty server address accepted");

        std::cout << "DcapClient input validation tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "DcapClient validation tests failed: " << e.what() << '\n';
        return 1;
    }
}