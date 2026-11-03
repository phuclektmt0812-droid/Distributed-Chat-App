#pragma once

#include "packet.h"
#include <array>
#include <string>
#include <vector>

namespace messages {
struct Auth { std::string username; std::string password; };
struct SignUp { std::string username; std::string password; };
struct AuthOk { std::string username; std::array<std::uint8_t,16> session_id{}; std::uint64_t server_time_ms=0; };
struct AuthFail { std::uint16_t code=0; std::string reason; };
struct SessionInfo { std::array<std::uint8_t,32> token{}; std::uint16_t udp_port=54001; std::uint32_t heartbeat_ms=5000; };
struct UdpRegister { std::array<std::uint8_t,32> token{}; };
struct UdpRegisterOk { bool registered=false; std::uint16_t code=0; };
struct Heartbeat { std::uint64_t nonce=0; std::uint64_t client_time_ms=0; };
struct HeartbeatAck { std::uint64_t nonce=0; std::uint64_t client_time_ms=0; std::uint64_t server_time_ms=0; };
struct ChatRequest { std::uint32_t client_id=0; std::string text; };
struct ChatEvent { std::uint32_t server_id=0; std::string sender; std::uint64_t server_time_ms=0; std::string text; };
struct PrivateRequest { std::uint32_t client_id=0; std::string recipient; std::string text; };
struct PrivateEvent { std::uint32_t server_id=0; std::string sender; std::string recipient; std::uint64_t server_time_ms=0; std::string text; };
struct UserList { std::uint32_t revision=0; std::vector<std::string> users; };
struct Ack { std::uint8_t acknowledged_type=0; std::uint32_t request_sequence=0; std::uint8_t status=0; std::string detail; };
struct Error { std::uint16_t code=0; std::uint8_t related_type=0; std::uint32_t related_sequence=0; std::string detail; };
struct ServerNotice { std::uint8_t severity=0; std::string message; };

bool encode(const Auth&,protocol::Bytes&);           bool decode(const protocol::Bytes&,Auth&);
bool encode(const SignUp&,protocol::Bytes&);         bool decode(const protocol::Bytes&,SignUp&);
bool encode(const AuthOk&,protocol::Bytes&);         bool decode(const protocol::Bytes&,AuthOk&);
bool encode(const AuthFail&,protocol::Bytes&);       bool decode(const protocol::Bytes&,AuthFail&);
bool encode(const SessionInfo&,protocol::Bytes&);    bool decode(const protocol::Bytes&,SessionInfo&);
bool encode(const UdpRegister&,protocol::Bytes&);    bool decode(const protocol::Bytes&,UdpRegister&);
bool encode(const UdpRegisterOk&,protocol::Bytes&);  bool decode(const protocol::Bytes&,UdpRegisterOk&);
bool encode(const Heartbeat&,protocol::Bytes&);      bool decode(const protocol::Bytes&,Heartbeat&);
bool encode(const HeartbeatAck&,protocol::Bytes&);   bool decode(const protocol::Bytes&,HeartbeatAck&);
bool encode(const ChatRequest&,protocol::Bytes&);    bool decode(const protocol::Bytes&,ChatRequest&);
bool encode(const ChatEvent&,protocol::Bytes&);      bool decode(const protocol::Bytes&,ChatEvent&);
bool encode(const PrivateRequest&,protocol::Bytes&); bool decode(const protocol::Bytes&,PrivateRequest&);
bool encode(const PrivateEvent&,protocol::Bytes&);   bool decode(const protocol::Bytes&,PrivateEvent&);
bool encode(const UserList&,protocol::Bytes&);       bool decode(const protocol::Bytes&,UserList&);
bool encode(const Ack&,protocol::Bytes&);            bool decode(const protocol::Bytes&,Ack&);
bool encode(const Error&,protocol::Bytes&);          bool decode(const protocol::Bytes&,Error&);
bool encode(const ServerNotice&,protocol::Bytes&);   bool decode(const protocol::Bytes&,ServerNotice&);
} // namespace messages
