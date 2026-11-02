#include "test_helpers.h"

#include "../clients/connection.h"

#include <iostream>

int main() {
    try {
        test::TestServer server;
        Connection client("127.0.0.1", server.port());

        test::require(client.connectToServer("127.0.0.1", server.port()),
            "TCP connection failed");

        Message ping{};
        ping.type = MessageType::PING;
        ping.payload = nlohmann::json::object();
        test::require(client.sendFramedMessage(ping),
            "Could not send a framed test message");

        Message response{};
        test::require(client.receiveFramedMessage(response),
            "Server did not respond to the test message");
        test::require(response.type == MessageType::ERROR,
            "Server did not reject an unauthenticated message");

        client.disconnect();
        test::require(!client.isConnected(),
            "Client did not disconnect");

        Connection reconnectingClient("127.0.0.1", server.port());
        test::require(
            reconnectingClient.connectToServer("127.0.0.1", server.port()),
            "Server did not accept a new connection after client disconnect"
        );
        reconnectingClient.disconnect();

        std::cout << "Connection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Connection tests failed: " << error.what() << '\n';
        return 1;
    }
}