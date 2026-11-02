#include "test_helpers.h"

#include <iostream>

int main() {
    try {
        test::TestServer server;

        DcapClient validClient("127.0.0.1");
        test::connectAndLogin(validClient, "valid-user");

        DcapClient duplicateClient("127.0.0.1");
        test::require(
            duplicateClient.connect(),
            "Could not connect duplicate-login client: " + duplicateClient.lastError()
        );
        test::require(
            duplicateClient.login("valid-user"),
            "Could not send duplicate AUTH request"
        );

        const auto rejected = test::waitForEvent(duplicateClient, "LOGIN_FAIL");
        test::require(
            rejected.data.value("code", 0) == static_cast<std::uint16_t>(protocol::ErrorCode::USERNAME_IN_USE),
            "Duplicate username was rejected with an unexpected error code"
        );
        test::require(
            validClient.authenticated(),
            "Rejecting the duplicate login damaged the original session"
        );

        DcapClient invalidClient("127.0.0.1");
        test::require(invalidClient.connect(), "Could not connect invalid-login client");
        test::require(
            !invalidClient.login(""),
            "Client accepted an empty username locally"
        );
        test::require(
            !invalidClient.login(std::string(protocol::MAX_USERNAME_BYTES + 1, 'x')),
            "Client accepted an overlong username locally"
        );

        std::cout << "DCAP authentication tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DCAP authentication tests failed: " << error.what() << '\n';
        return 1;
    }
}
