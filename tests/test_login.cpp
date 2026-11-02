#include "test_helpers.h"

#include <iostream>

int main() {
    try {
        test::TestServer server;
        ChatClient validClient("127.0.0.1", server.port());
        test::login(validClient, "valid-user");

        ChatClient invalidClient("127.0.0.1", server.port());
        test::require(invalidClient.connect(),
            "Could not connect client for invalid-login test");
        test::require(invalidClient.login("", ""),
            "Could not send invalid login request");
        test::waitForEvent(invalidClient, "LOGIN_FAIL");

        ChatClient duplicateClient("127.0.0.1", server.port());
        test::require(duplicateClient.connect(),
            "Could not connect client for duplicate-login test");
        test::require(duplicateClient.login("valid-user", "another-password"),
            "Could not send duplicate login request");
        test::waitForEvent(duplicateClient, "LOGIN_FAIL");

        std::cout << "Login tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Login tests failed: " << error.what() << '\n';
        return 1;
    }
}