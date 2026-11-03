#include "../protocol/message_payloads.h"
#include "../protocol/packet.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
messages::Auth& authDummy() {
    static messages::Auth value;
    return value;
}
template<class T>
void roundTrip(const T& input, T& output, const char* message) {
    protocol::Bytes bytes;
    require(messages::encode(input, bytes), message);
    require(messages::decode(bytes, output), message);
}
void testAuthAndSessionCodecs() {
    messages::Auth auth{"user-01"}, authOut;
    roundTrip(auth, authOut, "AUTH codec failed");
    require(authOut.username == auth.username, "AUTH data mismatch");
    messages::AuthOk ok{"user-01", {}, 123456}, okOut;
    roundTrip(ok, okOut, "AUTH_OK codec failed");
    require(okOut.username == ok.username && okOut.server_time_ms == ok.server_time_ms,
            "AUTH_OK data mismatch");
    messages::AuthFail fail{104, "invalid"}, failOut;
    roundTrip(fail, failOut, "AUTH_FAIL codec failed");
    require(failOut.code == fail.code && failOut.reason == fail.reason, "AUTH_FAIL mismatch");
    messages::SessionInfo info{{}, 54001, 5000}, infoOut;
    roundTrip(info, infoOut, "SESSION_INFO codec failed");
    require(infoOut.udp_port == info.udp_port && infoOut.heartbeat_ms == info.heartbeat_ms,
            "SESSION_INFO mismatch");
    messages::UdpRegister reg{}, regOut;
    roundTrip(reg, regOut, "UDP_REGISTER codec failed");
    require(regOut.token == reg.token, "UDP_REGISTER mismatch");
    messages::UdpRegisterOk regOk{true, 0}, regOkOut;
    roundTrip(regOk, regOkOut, "UDP_REGISTER_OK codec failed");
    require(regOkOut.registered && regOkOut.code == 0, "UDP_REGISTER_OK mismatch");
}
void testHeartbeatAndChatCodecs() {
    messages::Heartbeat hb{99, 123}, hbOut;
    roundTrip(hb, hbOut, "HEARTBEAT codec failed");
    require(hbOut.nonce == 99 && hbOut.client_time_ms == 123, "HEARTBEAT mismatch");
    messages::HeartbeatAck hba{99, 123, 456}, hbaOut;
    roundTrip(hba, hbaOut, "HEARTBEAT_ACK codec failed");
    require(hbaOut.server_time_ms == 456, "HEARTBEAT_ACK mismatch");
    messages::ChatRequest chat{7, "Xin chào 🌍"}, chatOut;
    roundTrip(chat, chatOut, "MSG codec failed");
    require(chatOut.client_id == 7 && chatOut.text == chat.text, "MSG mismatch");
    messages::ChatEvent event{8, "alice", 456, "hello"}, eventOut;
    roundTrip(event, eventOut, "CHAT event codec failed");
    require(eventOut.server_id == 8 && eventOut.sender == "alice" &&
            eventOut.server_time_ms == 456 && eventOut.text == "hello", "CHAT event mismatch");
    messages::PrivateRequest priv{9, "bob", "secret"}, privOut;
    roundTrip(priv, privOut, "PRIVATE_MSG codec failed");
    require(privOut.recipient == "bob" && privOut.text == "secret", "PRIVATE_MSG mismatch");
    messages::PrivateEvent pe{10, "alice", "bob", 789, "private"}, peOut;
    roundTrip(pe, peOut, "PRIVATE_MSG event codec failed");
    require(peOut.sender == "alice" && peOut.recipient == "bob" &&
            peOut.text == "private", "PRIVATE_MSG event mismatch");
}
void testControlCodecsAndInvalidInputs() {
    messages::UserList users{4, {"alice", "bob"}}, usersOut;
    roundTrip(users, usersOut, "USER_LIST codec failed");
    require(usersOut.revision == 4 && usersOut.users == users.users, "USER_LIST mismatch");
    auto unsorted = users; unsorted.users = {"bob", "alice"};
    protocol::Bytes bytes;
    require(!messages::encode(unsorted, bytes), "unsorted USER_LIST accepted");
    auto duplicate = users; duplicate.users = {"alice", "alice"};
    require(!messages::encode(duplicate, bytes), "duplicate USER_LIST accepted");
    messages::Ack ack{4, 22, 1, "ok"}, ackOut;
    roundTrip(ack, ackOut, "ACK codec failed");
    require(ackOut.request_sequence == 22 && ackOut.status == 1, "ACK mismatch");
    auto badAck = ack; badAck.status = 3;
    require(!messages::encode(badAck, bytes), "invalid ACK status accepted");
    messages::Error error{107, 4, 23, "too large"}, errorOut;
    roundTrip(error, errorOut, "ERROR codec failed");
    require(errorOut.code == 107 && errorOut.related_sequence == 23, "ERROR mismatch");
    messages::ServerNotice notice{2, "maintenance"}, noticeOut;
    roundTrip(notice, noticeOut, "SERVER_NOTICE codec failed");
    require(noticeOut.severity == 2 && noticeOut.message == "maintenance", "notice mismatch");

    require(!messages::encode(messages::Auth{""}, bytes), "empty username accepted");
    require(!messages::encode(messages::Auth{std::string(33, 'x')}, bytes),
            "overlong username accepted");
    require(!messages::encode(messages::ChatRequest{1, ""}, bytes), "empty chat accepted");
    require(!messages::encode(messages::ChatRequest{1, std::string(4097, 'x')}, bytes),
            "oversized chat accepted");
    require(!messages::decode({}, authDummy()), "empty payload unexpectedly decoded");
}
}
int main() {
    try {
        testAuthAndSessionCodecs();
        testHeartbeatAndChatCodecs();
        testControlCodecsAndInvalidInputs();
        std::cout << "Message payload codec tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Message payload codec tests failed: " << e.what() << '\n';
        return 1;
    }
}