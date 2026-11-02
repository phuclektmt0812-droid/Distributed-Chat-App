#include "secure_payload.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace protocol {
namespace {
using Ctx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
int nibble(char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    throw std::invalid_argument("invalid key hex character");
}
} // namespace

EncryptionKey readKeyFromEnvironment(const char* name) {
    const char* text=std::getenv(name);
    if (!text) throw std::runtime_error(std::string("missing environment key: ")+name);
    const std::string hex(text);
    if (hex.size()!=64) throw std::invalid_argument("DCAP_KEY_HEX must be 64 hexadecimal characters");
    EncryptionKey key{};
    for (std::size_t i=0;i<key.size();++i)
        key[i]=static_cast<std::uint8_t>((nibble(hex[i*2])<<4)|nibble(hex[i*2+1]));
    return key;
}
std::string generateKeyHex() {
    EncryptionKey key{};
    if (RAND_bytes(key.data(),static_cast<int>(key.size()))!=1)
        throw std::runtime_error("OpenSSL secure random failed");
    std::ostringstream ss;
    for (auto x:key) ss<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(x);
    return ss.str();
}

bool encodeSecurePacket(const Packet& plain, const EncryptionKey& key, Bytes& wire) {
    if (plain.payload.size()+28>MAX_TCP_PAYLOAD) return false;
    Packet encrypted{plain.type,plain.sequence,Bytes(plain.payload.size()+28)};
    if (RAND_bytes(encrypted.payload.data(),12)!=1) return false;
    Bytes frame;
    if (!encodePacket(encrypted,frame)) return false;

    Ctx ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);
    if (!ctx || EVP_EncryptInit_ex(ctx.get(),EVP_aes_256_gcm(),nullptr,nullptr,nullptr)!=1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_SET_IVLEN,12,nullptr)!=1 ||
        EVP_EncryptInit_ex(ctx.get(),nullptr,nullptr,key.data(),encrypted.payload.data())!=1)
        return false;
    int count=0;
    if (EVP_EncryptUpdate(ctx.get(),nullptr,&count,frame.data(),HEADER_SIZE)!=1) return false;
    int n=0;
    if (!plain.payload.empty() && EVP_EncryptUpdate(ctx.get(),encrypted.payload.data()+12,&n,
               plain.payload.data(),static_cast<int>(plain.payload.size()))!=1) return false;
    int final=0;
    if (EVP_EncryptFinal_ex(ctx.get(),encrypted.payload.data()+12+n,&final)!=1 ||
        std::size_t(n+final)!=plain.payload.size()) return false;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_GET_TAG,16,
            encrypted.payload.data()+12+plain.payload.size())!=1) return false;
    return encodePacket(encrypted,wire);
}

bool decodeSecurePacket(const std::uint8_t* wire, std::size_t size,
                        const EncryptionKey& key, Packet& plain) {
    Packet encrypted;
    if (!decodePacket(wire,size,encrypted) || encrypted.payload.size()<28) return false;
    const auto n=encrypted.payload.size()-28;
    Ctx ctx(EVP_CIPHER_CTX_new(),EVP_CIPHER_CTX_free);
    if (!ctx || EVP_DecryptInit_ex(ctx.get(),EVP_aes_256_gcm(),nullptr,nullptr,nullptr)!=1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_SET_IVLEN,12,nullptr)!=1 ||
        EVP_DecryptInit_ex(ctx.get(),nullptr,nullptr,key.data(),encrypted.payload.data())!=1)
        return false;
    int ignored=0;
    if (EVP_DecryptUpdate(ctx.get(),nullptr,&ignored,wire,HEADER_SIZE)!=1) return false;
    Bytes text(n ? n : 1);
    int decrypted=0;
    if (n && EVP_DecryptUpdate(ctx.get(),text.data(),&decrypted,
                             encrypted.payload.data()+12,static_cast<int>(n))!=1) return false;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(),EVP_CTRL_GCM_SET_TAG,16,
            encrypted.payload.data()+12+n)!=1) return false;
    int final=0;
    if (EVP_DecryptFinal_ex(ctx.get(),text.data()+decrypted,&final)!=1 ||
        std::size_t(decrypted+final)!=n) return false;
    text.resize(n);
    plain={encrypted.type,encrypted.sequence,std::move(text)};
    return true;
}
bool decodeSecureDatagram(const std::uint8_t* wire, std::size_t size,
                          const EncryptionKey& key, Packet& plain) {
    Packet candidate;
    if (size>MAX_UDP_DATAGRAM || !decodeSecurePacket(wire,size,key,candidate) ||
        !isUdpType(candidate.type)) return false;
    plain=std::move(candidate);
    return true;
}
bool unwrapSecurePayload(const Packet& encrypted, const EncryptionKey& key, Packet& plain) {
    Bytes wire;
    return encodePacket(encrypted,wire) && decodeSecurePacket(wire.data(),wire.size(),key,plain);
}
} // namespace protocol
