#include "secure_payload.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
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

namespace {
EncryptionKey parseKeyHex(const std::string& hex) {
    if (hex.size()!=64)
        throw std::invalid_argument("Key file must contain exactly 64 hexadecimal characters");
    EncryptionKey key{};
    for (std::size_t i=0;i<key.size();++i)
        key[i]=static_cast<std::uint8_t>((nibble(hex[i*2])<<4)|nibble(hex[i*2+1]));
    return key;
}
} // namespace

std::string keyFilePathFromEnvironment(const char* name) {
    const char* configured=std::getenv(name);
    return configured && *configured ? std::string(configured) : std::string("dcap.key");
}

EncryptionKey readKeyFromFile(const std::string& path) {
    std::ifstream input(path,std::ios::binary);
    if(!input) throw std::runtime_error("Cannot open key file '"+path+"'. Start the Server first or copy its key file securely.");
    std::string hex;
    if(!std::getline(input,hex) || input.bad())
        throw std::invalid_argument("Key file '"+path+"' is empty or unreadable");
    if(!hex.empty() && hex.back()=='\\r') hex.pop_back();
    std::string extra;
    if(std::getline(input,extra) && !extra.empty())
        throw std::invalid_argument("Key file '"+path+"' contains unexpected extra data");
#ifndef _WIN32
    struct stat info{};
    if(::stat(path.c_str(),&info)==0 && (info.st_mode & (S_IRWXG|S_IRWXO))!=0)
        throw std::runtime_error("Key file '"+path+"' is accessible by group/others; run chmod 600 on it");
#endif
    return parseKeyHex(hex);
}

EncryptionKey loadOrCreateKeyFile(const std::string& path) {
    const std::string contents=generateKeyHex()+"\n";
#ifdef _WIN32
    const int fd=::_open(path.c_str(),_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
#else
    const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,S_IRUSR|S_IWUSR);
#endif
    if(fd<0) {
        if(errno==EEXIST) return readKeyFromFile(path);
        throw std::runtime_error("Cannot create key file '"+path+"': "+std::string(std::strerror(errno)));
    }
    std::size_t offset=0;
    while(offset<contents.size()) {
#ifdef _WIN32
        const int written=::_write(fd,contents.data()+offset,static_cast<unsigned int>(contents.size()-offset));
#else
        const ssize_t written=::write(fd,contents.data()+offset,contents.size()-offset);
#endif
        if(written<0 && errno==EINTR) continue;
        if(written<=0) {
#ifdef _WIN32
            ::_close(fd);
#else
            ::close(fd);
#endif
            std::remove(path.c_str());
            throw std::runtime_error("Could not write key file '"+path+"'");
        }
        offset+=static_cast<std::size_t>(written);
    }
#ifdef _WIN32
    if(::_close(fd)!=0) throw std::runtime_error("Could not close key file '"+path+"'");
#else
    if(::close(fd)!=0) throw std::runtime_error("Could not close key file '"+path+"'");
#endif
    return parseKeyHex(contents.substr(0,64));
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
