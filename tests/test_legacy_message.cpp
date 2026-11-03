#include "../protocol/message.h"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void testRoundTrip() {
    Message source{MessageType::CHAT_PRIVATE,
                   {{"sender", "alice"}, {"recipient", "bob"}, {"content", "hello"}}};
    const auto encoded = serializeMessage(source);
    Message decoded{};
    require(deserializeMessage(encoded, decoded), "valid legacy JSON message rejected");
    require(decoded.type == MessageType::CHAT_PRIVATE, "legacy message type changed");
    require(decoded.payload == source.payload, "legacy JSON payload changed");
}
void testMalformedMessages() {
    Message output{};
    require(!deserializeMessage("", output), "empty JSON accepted");
    require(!deserializeMessage("{", output), "malformed JSON accepted");
    require(!deserializeMessage("[]", output), "JSON array accepted");
    require(!deserializeMessage(R"({"payload":{}})", output), "missing type accepted");
    require(!deserializeMessage(R"({"type":"MSG"})", output), "missing payload accepted");
    require(!deserializeMessage(R"({"type":7,"payload":{}})", output), "non-string type accepted");
    require(!deserializeMessage(R"({"type":"UNKNOWN","payload":{}})", output), "unknown type accepted");
    require(!deserializeMessage(R"({"type":"LOGIN","payload":[]})", output), "non-object payload accepted");
}
}
int main() {
    try {
        testRoundTrip();
        testMalformedMessages();
        std::cout << "Legacy JSON message codec tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Legacy JSON message tests failed: " << e.what() << '\n';
        return 1;
    }
}