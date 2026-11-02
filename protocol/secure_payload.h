#pragma once

#include "packet.h"
#include <array>
#include <string>

namespace protocol {
using EncryptionKey = std::array<std::uint8_t, 32>;

// Optional team-wide security profile, requiring explicit agreement before use.
// Maintains DCAP v1's 14-byte header and type IDs. Payload on wire becomes
// nonce[12] | AES-256-GCM(ciphertext)[N] | tag[16], header authenticated as AAD.
// All involved endpoints, INCLUDING the UDP relay Server, must use the same
// secure decoder so it can validate transfer IDs and session permissions.
EncryptionKey readKeyFromFile(const std::string& path);
EncryptionKey loadOrCreateKeyFile(const std::string& path);
std::string keyFilePathFromEnvironment(const char* name = "DCAP_KEY_FILE");
std::string generateKeyHex();
bool encodeSecurePacket(const Packet& plain, const EncryptionKey& key, Bytes& wire);
bool decodeSecurePacket(const std::uint8_t* wire, std::size_t size,
                        const EncryptionKey& key, Packet& plain);
// Convenience for packets already extracted by TcpPacketParser.
bool unwrapSecurePayload(const Packet& encrypted, const EncryptionKey& key, Packet& plain);
bool decodeSecureDatagram(const std::uint8_t* wire, std::size_t size,
                          const EncryptionKey& key, Packet& plain);
} // namespace protocol
