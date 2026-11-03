#include "test_helpers.h"

#include <iostream>

int main() {
    try {
        test::TestServer server;

        DcapClient first("127.0.0.1");
        test::require(first.connect(), "First TCP connection failed: " + first.lastError());
        test::require(first.login("connection-user"), "Could not send AUTH");
        test::waitForEvent(first, "LOGIN_OK");

        first.disconnect();
        const auto removalDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (server.server().users().find("connection-user") &&
               std::chrono::steady_clock::now() < removalDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        test::require(
            !server.server().users().find("connection-user"),
            "Server did not remove the disconnected session"
        );

        DcapClient second("127.0.0.1");
        test::connectAndLogin(second, "connection-user");
        test::require(second.authenticated(), "Reconnected client is not authenticated");
        second.disconnect();

        DcapClient third("127.0.0.1");
        test::connectAndLogin(third, "logout-user");
        third.logout();
        const auto logoutDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (server.server().users().find("logout-user") &&
               std::chrono::steady_clock::now() < logoutDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        test::require(!server.server().users().find("logout-user"),
                      "LOGOUT did not remove authenticated session");

        server.server().stop();
        test::require(!server.server().running(), "server stop did not clear running state");
        test::require(server.server().start(), "server could not restart after stop");
        test::require(server.server().start(), "starting an already-running server should be idempotent");
        server.server().stop();

        std::cout << "DCAP connection, logout and server lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DCAP connection tests failed: " << error.what() << '\n';
        return 1;
    }
}