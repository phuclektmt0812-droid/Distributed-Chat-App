#pragma once
#include "packet.h"
#include "../platform/socket_compat.h"
#include <cstdint>
#include <string>

namespace protocol {
constexpr std::uint16_t TCP_PORT=54000;
constexpr std::uint16_t UDP_PORT=54001;
constexpr std::size_t MAX_USERNAME_BYTES=32;
constexpr std::size_t MAX_CHAT_BYTES=4096;
constexpr std::uint32_t HEARTBEAT_INTERVAL_MS=5000;

constexpr std::uint16_t ERR_MALFORMED_PACKET=100;
constexpr std::uint16_t ERR_UNSUPPORTED_VERSION=101;
constexpr std::uint16_t ERR_UNKNOWN_MESSAGE_TYPE=102;
constexpr std::uint16_t ERR_NOT_AUTHENTICATED=103;
constexpr std::uint16_t ERR_INVALID_USERNAME=104;
constexpr std::uint16_t ERR_USERNAME_IN_USE=105;
constexpr std::uint16_t ERR_USER_NOT_FOUND=106;
constexpr std::uint16_t ERR_MESSAGE_TOO_LARGE=107;
constexpr std::uint16_t ERR_RATE_LIMITED=108;
constexpr std::uint16_t ERR_INVALID_SESSION=109;
constexpr std::uint16_t ERR_WRONG_TRANSPORT=110;
constexpr std::uint16_t ERR_TRANSFER_NOT_FOUND=200;
constexpr std::uint16_t ERR_TRANSFER_REJECTED=201;
constexpr std::uint16_t ERR_FILE_TOO_LARGE=202;
constexpr std::uint16_t ERR_INVALID_FILENAME=203;
constexpr std::uint16_t ERR_INVALID_CHUNK=204;
constexpr std::uint16_t ERR_TRANSFER_TIMEOUT=205;
constexpr std::uint16_t ERR_INTEGRITY_FAILED=206;
constexpr std::uint16_t ERR_DISK_SPACE=207;
constexpr std::uint16_t ERR_TRANSFER_CANCELLED=208;
constexpr std::uint16_t ERR_TRANSFER_BUSY=209;

void putU16(Bytes& bytes,std::uint16_t value);
void putU32(Bytes& bytes,std::uint32_t value);
void putU64(Bytes& bytes,std::uint64_t value);
bool putString(Bytes& bytes,const std::string& value);
std::uint16_t readU16(const std::uint8_t* bytes);
std::uint32_t readU32(const std::uint8_t* bytes);
std::uint64_t readU64(const std::uint8_t* bytes);
bool getString(const Bytes& bytes,std::size_t& offset,std::string& output,
               std::size_t max_length=65535);

bool sendTcpPacket(SocketHandle socket,const Packet& packet);
bool receiveTcpPacket(SocketHandle socket,Packet& packet);
bool sendUdpPacket(SocketHandle socket,const Packet& packet,const sockaddr_in& destination);
bool decodePacket(const std::uint8_t* bytes,std::size_t size,Packet& output,
                  std::size_t max_datagram_size);

using PacketCodec = TcpPacketParser;
} // namespace protocol
