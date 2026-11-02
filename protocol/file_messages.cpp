#include "file_messages.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace fileproto {
namespace {
using W = protocol::ByteWriter;
using R = protocol::ByteReader;
void id(W& w, const TransferId& x) { w.raw(x.data(), x.size()); }
bool id(R& r, TransferId& x) { return r.raw(x.data(), x.size()); }
void digest(W& w, const Digest& d) { w.raw(d.data(), d.size()); }
bool digest(R& r, Digest& d) { return r.raw(d.data(), d.size()); }
bool validSize(std::uint64_t size, std::uint16_t chunk, std::uint32_t n) {
    return size <= protocol::MAX_FILE_SIZE && chunk == protocol::FILE_CHUNK_SIZE &&
           n == size / chunk + (size % chunk != 0);
}
bool validOffer(const Offer& o) {
    if (!protocol::validFilename(o.filename) || o.recipient.empty() ||
        o.recipient.size()>32 || !protocol::validUtf8(o.recipient) ||
        !validSize(o.size,o.chunk_size,o.chunks)) return false;
    for (unsigned char c:o.recipient) if (c<32 || c==127) return false;
    return true;
}
} // namespace

std::uint32_t chunkCount(std::uint64_t size) {
    if (size > protocol::MAX_FILE_SIZE) throw std::invalid_argument("maximum file size is 100 MiB");
    return static_cast<std::uint32_t>(size / protocol::FILE_CHUNK_SIZE +
                                      (size % protocol::FILE_CHUNK_SIZE != 0));
}
TransferId randomId() {
    TransferId out{};
    if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1)
        throw std::runtime_error("OpenSSL secure random failed");
    return out;
}
std::string idHex(const TransferId& id) {
    std::ostringstream s;
    for (auto b : id) s << std::hex << std::setw(2) << std::setfill('0') << unsigned(b);
    return s.str();
}
Digest sha256File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file for SHA-256: " + path);
    using Ptr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    Ptr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("SHA-256 initialization failed");
    std::array<char, 65536> buffer{};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto n = in.gcount();
        if (n > 0 && EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(n)) != 1)
            throw std::runtime_error("SHA-256 update failed");
    }
    if (in.bad()) throw std::runtime_error("file read failed during SHA-256");
    Digest d{}; unsigned n = 0;
    if (EVP_DigestFinal_ex(ctx.get(), d.data(), &n) != 1 || n != d.size())
        throw std::runtime_error("SHA-256 finalization failed");
    return d;
}

protocol::Packet makePacket(protocol::Type type, std::uint32_t seq, const protocol::Bytes& bytes) {
    return {type, seq, bytes};
}

bool encode(const Offer& o, protocol::Bytes& out) {
    if (!validOffer(o)) return false;
    W w; id(w, o.id);
    if (!w.str(o.recipient) || !w.str(o.filename)) return false;
    w.u64(o.size); w.u16(o.chunk_size); w.u32(o.chunks); digest(w, o.sha256);
    out = w.take(); return true;
}
bool decode(const protocol::Bytes& b, Offer& o) {
    R r(b); Offer p;
    if (!id(r,p.id) || !r.str(p.recipient,32) || !r.str(p.filename,255) ||
        !r.u64(p.size) || !r.u16(p.chunk_size) || !r.u32(p.chunks) ||
        !digest(r,p.sha256) || !r.done() || !validOffer(p)) return false;
    o = std::move(p); return true;
}
bool encode(const Decision& p, protocol::Bytes& out) {
    W w; id(w,p.id); w.u8(p.accepted ? 1 : 0); w.u16(p.reason_code);
    if (!w.str(p.reason)) return false;
    out=w.take(); return true;
}
bool decode(const protocol::Bytes& b, Decision& o) {
    R r(b); Decision p; std::uint8_t accept=0;
    if (!id(r,p.id) || !r.u8(accept) || accept>1 || !r.u16(p.reason_code) ||
        !r.str(p.reason) || !r.done()) return false;
    p.accepted=accept!=0; o=std::move(p); return true;
}
bool encode(const Start& p, protocol::Bytes& out) {
    if (!validSize(p.size,p.chunk_size,p.chunks)) return false;
    W w; id(w,p.id); w.u64(p.size); w.u16(p.chunk_size); w.u32(p.chunks); digest(w,p.sha256);
    out=w.take(); return true;
}
bool decode(const protocol::Bytes& b, Start& o) {
    R r(b); Start p;
    if (!id(r,p.id) || !r.u64(p.size) || !r.u16(p.chunk_size) ||
        !r.u32(p.chunks) || !digest(r,p.sha256) || !r.done() ||
        !validSize(p.size,p.chunk_size,p.chunks)) return false;
    o=p; return true;
}
bool encode(const Accept& p, protocol::Bytes& out) {
    if (p.window>protocol::MAX_WINDOW) return false;
    W w; id(w,p.id); w.u8(p.ready?1:0); w.u16(p.window); w.u16(p.reason_code);
    if (!w.str(p.reason)) return false;
    out=w.take(); return true;
}
bool decode(const protocol::Bytes& b, Accept& o) {
    R r(b); Accept p; std::uint8_t ready=0;
    if (!id(r,p.id) || !r.u8(ready) || ready>1 || !r.u16(p.window) ||
        p.window>protocol::MAX_WINDOW || !r.u16(p.reason_code) ||
        !r.str(p.reason) || !r.done()) return false;
    p.ready=ready!=0; o=std::move(p); return true;
}
bool encode(const Chunk& p, protocol::Bytes& out) {
    if (p.bytes.empty() || p.bytes.size()>protocol::FILE_CHUNK_SIZE) return false;
    W w; id(w,p.id); w.u32(p.index); w.u16(static_cast<std::uint16_t>(p.bytes.size())); w.raw(p.bytes);
    out=w.take(); return true;
}
bool decode(const protocol::Bytes& b, Chunk& o) {
    R r(b); Chunk p; std::uint16_t n=0;
    if (!id(r,p.id) || !r.u32(p.index) || !r.u16(n) || !n ||
        n>protocol::FILE_CHUNK_SIZE || r.left()!=n) return false;
    p.bytes.resize(n);
    if (!r.raw(p.bytes.data(),n) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const Ack& p, protocol::Bytes& out) {
    if (p.bitmap.size()>4 || p.window>protocol::MAX_WINDOW) return false;
    W w; id(w,p.id); w.u32(p.base); w.u16(static_cast<std::uint16_t>(p.bitmap.size()));
    w.raw(p.bitmap); w.u16(p.window); out=w.take(); return true;
}
bool decode(const protocol::Bytes& b, Ack& o) {
    R r(b); Ack p; std::uint16_t n=0;
    if (!id(r,p.id) || !r.u32(p.base) || !r.u16(n) || n>4 ||
        r.left()!=static_cast<std::size_t>(n+2)) return false;
    p.bitmap.resize(n);
    if (!r.raw(p.bitmap.data(),n) || !r.u16(p.window) ||
        p.window>protocol::MAX_WINDOW || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const End& p, protocol::Bytes& out) {
    if (!validSize(p.size,protocol::FILE_CHUNK_SIZE,p.chunks)) return false;
    W w; id(w,p.id); w.u32(p.chunks); w.u64(p.size); digest(w,p.sha256);
    out=w.take(); return true;
}
bool decode(const protocol::Bytes& b, End& o) {
    R r(b); End p;
    if (!id(r,p.id) || !r.u32(p.chunks) || !r.u64(p.size) ||
        !digest(r,p.sha256) || !r.done() ||
        !validSize(p.size,protocol::FILE_CHUNK_SIZE,p.chunks)) return false;
    o=p; return true;
}
bool encode(const Complete& p, protocol::Bytes& out) {
    W w; id(w,p.id); w.u8(p.verified?1:0); w.u16(p.error_code); w.u64(p.size); digest(w,p.sha256);
    out=w.take(); return true;
}
bool decode(const protocol::Bytes& b, Complete& o) {
    R r(b); Complete p; std::uint8_t status=0;
    if (!id(r,p.id) || !r.u8(status) || status>1 || !r.u16(p.error_code) ||
        !r.u64(p.size) || !digest(r,p.sha256) || !r.done()) return false;
    p.verified=status!=0; o=p; return true;
}
bool encode(const Failure& p, protocol::Bytes& out) {
    W w; id(w,p.id); w.u16(p.code); if (!w.str(p.detail)) return false;
    out=w.take(); return true;
}
bool decode(const protocol::Bytes& b, Failure& o) {
    R r(b); Failure p;
    if (!id(r,p.id) || !r.u16(p.code) || !r.str(p.detail) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const Result& p, protocol::Bytes& out) {
    if (p.status>2) return false;
    W w; id(w,p.id); w.u8(p.status); w.u16(p.error_code); w.u64(p.bytes);
    if (!w.str(p.detail)) return false;
    out=w.take(); return true;
}
bool decode(const protocol::Bytes& b, Result& o) {
    R r(b); Result p;
    if (!id(r,p.id) || !r.u8(p.status) || p.status>2 || !r.u16(p.error_code) ||
        !r.u64(p.bytes) || !r.str(p.detail) || !r.done()) return false;
    o=std::move(p); return true;
}
} // namespace fileproto
