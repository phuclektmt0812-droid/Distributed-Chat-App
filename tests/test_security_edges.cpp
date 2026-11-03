#include "../protocol/secure_payload.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<class F>
bool throws(F&& f) {
    try { f(); } catch (...) { return true; }
    return false;
}
void testSecurePacketEdges() {
    protocol::EncryptionKey key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i * 3 + 1);
    protocol::Packet source{protocol::Type::MSG, 0xABCDEF01, {}};
    protocol::Bytes wire;
    require(protocol::encodeSecurePacket(source, key, wire), "empty secure packet encode failed");
    protocol::Packet decoded;
    require(protocol::decodeSecurePacket(wire.data(), wire.size(), key, decoded),
            "empty secure packet decode failed");
    require(decoded.type == source.type && decoded.sequence == source.sequence &&
            decoded.payload.empty(), "empty secure packet changed");

    source.payload = {0, 1, 2, 127, 128, 255};
    require(protocol::encodeSecurePacket(source, key, wire), "secure packet encode failed");
    require(!protocol::decodeSecurePacket(wire.data(), wire.size() - 1, key, decoded),
            "truncated secure packet accepted");
    require(!wire.empty(), "encoded secure packet is unexpectedly empty");
    auto tampered = wire;
    tampered.at(tampered.size() - 1) ^= 1;
    require(!protocol::decodeSecurePacket(tampered.data(), tampered.size(), key, decoded),
            "modified authentication tag accepted");
    tampered = wire; tampered[protocol::HEADER_SIZE + 1] ^= 1;
    require(!protocol::decodeSecurePacket(tampered.data(), tampered.size(), key, decoded),
            "modified nonce accepted");
    tampered = wire; tampered[6] ^= 1;
    require(!protocol::decodeSecurePacket(tampered.data(), tampered.size(), key, decoded),
            "modified authenticated header accepted");
    auto wrong = key; wrong[0] ^= 0xFF;
    require(!protocol::decodeSecurePacket(wire.data(), wire.size(), wrong, decoded),
            "wrong encryption key accepted");
    source.payload.assign(protocol::MAX_TCP_PAYLOAD - 27, 0);
    require(!protocol::encodeSecurePacket(source, key, wire),
            "secure payload exceeding encrypted frame limit accepted");

    source = {protocol::Type::MSG, 1, {1}};
    require(protocol::encodeSecurePacket(source, key, wire), "TCP secure packet encode failed");
    require(!protocol::decodeSecureDatagram(wire.data(), wire.size(), key, decoded),
            "TCP packet accepted as secure UDP datagram");
    source.type = protocol::Type::HEARTBEAT;
    require(protocol::encodeSecurePacket(source, key, wire), "UDP secure packet encode failed");
    require(protocol::decodeSecureDatagram(wire.data(), wire.size(), key, decoded),
            "valid secure UDP datagram rejected");
}
void testKeyFileValidation() {
    const auto root = fs::temp_directory_path() /
        ("dcap-security-edge-" + protocol::generateKeyHex());
    fs::create_directories(root);
    const auto missing = root / "missing.key";
    require(throws([&] { (void)protocol::readKeyFromFile(missing.string()); }),
            "missing key file must throw");

    const auto createdPath = root / "created.key";
    const auto created = protocol::loadOrCreateKeyFile(createdPath.string());
    require(fs::exists(createdPath), "loadOrCreate did not create key file");
    require(protocol::readKeyFromFile(createdPath.string()) == created,
            "created key did not round-trip");
    require(protocol::loadOrCreateKeyFile(createdPath.string()) == created,
            "existing key should be reused");
    require(protocol::generateKeyHex().size() == 64, "generated key hex must be 64 chars");

    const auto malformed = root / "malformed.key";
    { std::ofstream out(malformed); out << "not-a-key\n"; }
    const auto invalidHex = root / "invalid-hex.key";
    { std::ofstream out(invalidHex); out << std::string(63, 'a') << "g\n"; }
    const auto extra = root / "extra.key";
    { std::ofstream out(extra); out << std::string(64, 'a') << "\nextra\n"; }
#ifndef _WIN32
    ::chmod(malformed.c_str(), 0600);
    ::chmod(invalidHex.c_str(), 0600);
    ::chmod(extra.c_str(), 0600);
#endif
    require(throws([&] { (void)protocol::readKeyFromFile(malformed.string()); }),
            "malformed key length must throw");
    require(throws([&] { (void)protocol::readKeyFromFile(invalidHex.string()); }),
            "non-hex key must throw");
    require(throws([&] { (void)protocol::readKeyFromFile(extra.string()); }),
            "unexpected extra key data must throw");
#ifndef _WIN32
    const auto insecure = root / "insecure.key";
    { std::ofstream out(insecure); out << std::string(64, 'b') << "\n"; }
    ::chmod(insecure.c_str(), 0644);
    require(throws([&] { (void)protocol::readKeyFromFile(insecure.string()); }),
            "group/world-readable key file must be rejected");
#endif
    std::error_code ec; fs::remove_all(root, ec);
}
void testEnvironmentPath() {
#ifdef _WIN32
    _putenv_s("DCAP_TEST_KEY_PATH", "temporary-test-key");
#else
    setenv("DCAP_TEST_KEY_PATH", "temporary-test-key", 1);
#endif
    require(protocol::keyFilePathFromEnvironment("DCAP_TEST_KEY_PATH") == "temporary-test-key",
            "custom key-file environment variable ignored");
#ifdef _WIN32
    _putenv_s("DCAP_TEST_KEY_PATH", "");
#else
    unsetenv("DCAP_TEST_KEY_PATH");
#endif
    require(protocol::keyFilePathFromEnvironment("DCAP_TEST_KEY_PATH") == "dcap.key",
            "default key path incorrect");
}
}
int main() {
    try {
        testSecurePacketEdges();
        testKeyFileValidation();
        testEnvironmentPath();
        std::cout << "Security edge-case tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Security edge-case tests failed: " << e.what() << '\n';
        return 1;
    }
}