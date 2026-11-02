#include "../protocol/secure_payload.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <filesystem>
int main() {
    protocol::EncryptionKey key{}, wrongKey{};
    for(std::size_t i=0;i<key.size();++i) key[i]=static_cast<std::uint8_t>(i+1);
    wrongKey[0]=0xFF;
    protocol::Packet original;
    original.type=protocol::Type::MSG; original.sequence=42;
    original.payload={0x00,0x01,0x7F,0x80,0xFE,0xFF};
    protocol::Bytes wire;
    assert(protocol::encodeSecurePacket(original,key,wire));
    assert(wire.size()==protocol::HEADER_SIZE+original.payload.size()+28);
    protocol::Packet decoded;
    assert(protocol::decodeSecurePacket(wire.data(),wire.size(),key,decoded));
    assert(decoded.type==original.type && decoded.sequence==original.sequence && decoded.payload==original.payload);
    protocol::Packet rejected;
    assert(!protocol::decodeSecurePacket(wire.data(),wire.size(),wrongKey,rejected));
    auto tampered=wire; tampered.back()^=0x01;
    assert(!protocol::decodeSecurePacket(tampered.data(),tampered.size(),key,rejected));
    tampered=wire; tampered[6]^=0x01;
    assert(!protocol::decodeSecurePacket(tampered.data(),tampered.size(),key,rejected));
    std::cout<<"DCAP AES-256-GCM tests passed\n";
    return 0;
}