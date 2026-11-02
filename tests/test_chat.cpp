#include "test_helpers.h"

#include <iostream>

int main() {
    try {
        test::TestServer server;

        DcapClient alice("127.0.0.1");
        DcapClient bob("127.0.0.1");
        DcapClient charlie("127.0.0.1");

        test::connectAndLogin(alice, "alice");
        test::connectAndLogin(bob, "bob");
        test::connectAndLogin(charlie, "charlie");

        const auto usersDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while ((!test::contains(alice.users(), "bob") ||
                !test::contains(alice.users(), "charlie")) &&
               std::chrono::steady_clock::now() < usersDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        test::require(
            test::contains(alice.users(), "alice") &&
            test::contains(alice.users(), "bob") &&
            test::contains(alice.users(), "charlie"),
            "Online user list does not contain all authenticated clients"
        );

        test::require(
            alice.sendGlobal("hello from alice"),
            "Could not send global message"
        );

        const auto bobGlobal = test::waitForEvent(bob, "CHAT_ALL");
        const auto charlieGlobal = test::waitForEvent(charlie, "CHAT_ALL");
        test::require(
            bobGlobal.data.value("content", std::string()) == "hello from alice" &&
            bobGlobal.data.value("sender", std::string()) == "alice",
            "Bob received incorrect global message data"
        );
        test::require(
            charlieGlobal.data.value("content", std::string()) == "hello from alice" &&
            charlieGlobal.data.value("sender", std::string()) == "alice",
            "Charlie received incorrect global message data"
        );

        test::require(
            alice.sendPrivate("bob", "message only for bob"),
            "Could not send private message"
        );

        const auto privateEvent = test::waitForEvent(bob, "CHAT_PRIVATE");
        test::require(
            privateEvent.data.value("content", std::string()) == "message only for bob" &&
            privateEvent.data.value("sender", std::string()) == "alice" &&
            privateEvent.data.value("recipient", std::string()) == "bob",
            "Private message fields did not match"
        );
        test::require(
            test::noEvent(charlie, "CHAT_PRIVATE", std::chrono::milliseconds(350)),
            "Private message was incorrectly delivered to Charlie"
        );

        bob.disconnect();
        const auto removalDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (test::contains(alice.users(), "bob") &&
               std::chrono::steady_clock::now() < removalDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        test::require(
            !test::contains(alice.users(), "bob"),
            "Disconnected user remained in the online user list"
        );

        std::cout << "DCAP chat-routing tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DCAP chat tests failed: " << error.what() << '\n';
        return 1;
    }
}
