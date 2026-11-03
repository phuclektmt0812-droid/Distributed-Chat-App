#include "test_helpers.h"
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main() {
    try {
        test::TestServer server;
        constexpr int clientCount = 20;
        std::vector<std::unique_ptr<DcapClient>> clients;
        clients.reserve(clientCount);
        for (int i = 0; i < clientCount; ++i) {
            auto client = std::make_unique<DcapClient>("127.0.0.1");
            test::connectAndLogin(*client, "load-user-" + std::to_string(i));
            clients.push_back(std::move(client));
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (server.server().users().snapshot().size() != clientCount &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        test::require(server.server().users().snapshot().size() == clientCount,
                      "server did not retain all 20 concurrent sessions");

        test::require(clients.front()->sendGlobal("concurrency-broadcast"),
                      "first client could not broadcast");
        for (int i = 1; i < clientCount; ++i) {
            const auto event = test::waitForEvent(*clients[i], "CHAT_ALL");
            test::require(event.data.value("content", std::string()) == "concurrency-broadcast",
                          "concurrent recipient received wrong broadcast");
        }

        for (int i = 1; i < clientCount; i += 2) clients[i]->disconnect();
        const auto removalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (server.server().users().snapshot().size() != 10 &&
               std::chrono::steady_clock::now() < removalDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        test::require(server.server().users().snapshot().size() == 10,
                      "disconnect cleanup failed under concurrent load");

        clients.clear();
        const auto emptyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!server.server().users().snapshot().empty() &&
               std::chrono::steady_clock::now() < emptyDeadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        test::require(server.server().users().snapshot().empty(),
                      "server retained sessions after all clients disconnected");

        std::cout << "20-client concurrency tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Concurrency tests failed: " << e.what() << '\n';
        return 1;
    }
}