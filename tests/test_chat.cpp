#include "test_helpers.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace {

bool hasUser(const ClientEvent& event, const std::string& username) {
    if (!event.data.contains("users") || !event.data["users"].is_array()) {
        return false;
    }

    for (const auto& user : event.data["users"]) {
        if (user.is_string() && user.get<std::string>() == username) {
            return true;
        }
    }
    return false;
}

ClientEvent waitForUserListWithout(
    ChatClient& client,
    const std::string& username
) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        auto event = client.getNextEvent();
        if (event.has_value() && event->type == "USER_LIST" &&
            !hasUser(*event, username) &&
            hasUser(*event, "alice") &&
            hasUser(*event, "charlie")) {
            return *event;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("User was not removed from the online list");
}

bool receivesChatMessage(
    ChatClient& client,
    std::chrono::milliseconds timeout
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto event = client.getNextEvent();
        if (event.has_value() && event->type == "CHAT_DELIVER") {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool hasExpectedOnlineUsers(const std::vector<std::string>& users) {
    return std::find(users.begin(), users.end(), "alice") != users.end() &&
        std::find(users.begin(), users.end(), "bob") != users.end() &&
        std::find(users.begin(), users.end(), "charlie") != users.end();
}

} // namespace

int main() {
    try {
        test::TestServer server;
        ChatClient alice("127.0.0.1", server.port());
        ChatClient bob("127.0.0.1", server.port());
        ChatClient charlie("127.0.0.1", server.port());

        test::login(alice, "alice");
        test::login(bob, "bob");
        test::login(charlie, "charlie");

        test::require(alice.sendGlobalMessage("hello everyone"),
            "Alice could not send a global message");
        const ClientEvent globalForBob = test::waitForEvent(bob, "CHAT_DELIVER");
        const ClientEvent globalForCharlie = test::waitForEvent(charlie, "CHAT_DELIVER");
        for (const ClientEvent* event : {&globalForBob, &globalForCharlie}) {
            test::require(event->data.value("content", std::string()) ==
                    "hello everyone",
                "Global message content did not match");
            test::require(event->data.value("sender", std::string()) == "alice",
                "Global message sender did not match");
            test::require(event->data.value("private", false) == false,
                "Global message was marked private");
        }

        test::require(alice.sendPrivateMessage("bob", "just for bob"),
            "Alice could not send a private message");
        const ClientEvent privateForBob = test::waitForEvent(bob, "CHAT_DELIVER");
        test::require(privateForBob.data.value("content", std::string()) ==
                "just for bob",
            "Private message content did not match");
        test::require(privateForBob.data.value("private", false),
            "Private message was not marked private");
        test::require(!receivesChatMessage(charlie, std::chrono::milliseconds(350)),
            "Private message was delivered to Charlie");

        const auto onlineDeadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(3);
        while (!hasExpectedOnlineUsers(alice.getLatestUserList()) &&
            std::chrono::steady_clock::now() < onlineDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        test::require(hasExpectedOnlineUsers(alice.getLatestUserList()),
            "Online user list did not contain all logged-in users");

        bob.disconnect();
        test::require(!hasUser(waitForUserListWithout(alice, "bob"), "bob"),
            "Disconnected user remained in the online list");

        std::cout << "Chat tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Chat tests failed: " << error.what() << '\n';
        return 1;
    }
}