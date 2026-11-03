#include "message_payloads.h"
#include <algorithm>
#include <utility>

namespace messages {
namespace {
using W=protocol::ByteWriter; using R=protocol::ByteReader;
bool validName(const std::string& s) {
    if (s.empty() || s.size()>32 || !protocol::validUtf8(s)) return false;
    for (unsigned char c:s) if (c<32 || c==127) return false;
    return true;
}
bool validPassword(const std::string& s) { return !s.empty() && s.size()<=128 && protocol::validUtf8(s); }
bool validText(const std::string& s) { return !s.empty() && s.size()<=4096 && protocol::validUtf8(s); }
bool readName(R& r,std::string& s) { return r.str(s,32) && validName(s); }
bool readText(R& r,std::string& s) { return r.str(s,4096) && validText(s); }
} // namespace

bool encode(const Auth& p,protocol::Bytes& b) {
    if (!validName(p.username) || !validPassword(p.password)) return false;
    W w; if (!w.str(p.username) || !w.str(p.password)) return false;
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,Auth& o) {
    R r(b); Auth p;
    if (!readName(r,p.username) || !r.str(p.password,128) ||
        !validPassword(p.password) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const SignUp& p,protocol::Bytes& b) {
    if (!validName(p.username) || !validPassword(p.password)) return false;
    W w; if (!w.str(p.username) || !w.str(p.password)) return false;
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,SignUp& o) {
    R r(b); SignUp p;
    if (!readName(r,p.username) || !r.str(p.password,128) ||
        !validPassword(p.password) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const AuthOk& p,protocol::Bytes& b) {
    if (!validName(p.username)) return false;
    W w; if (!w.str(p.username)) return false;
    w.raw(p.session_id.data(),p.session_id.size()); w.u64(p.server_time_ms);
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,AuthOk& o) {
    R r(b); AuthOk p;
    if (!readName(r,p.username) || !r.raw(p.session_id.data(),p.session_id.size()) ||
        !r.u64(p.server_time_ms) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const AuthFail& p,protocol::Bytes& b) {
    W w; w.u16(p.code); if (!w.str(p.reason)) return false;
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,AuthFail& o) {
    R r(b); AuthFail p;
    if (!r.u16(p.code) || !r.str(p.reason) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const SessionInfo& p,protocol::Bytes& b) {
    W w; w.raw(p.token.data(),p.token.size()); w.u16(p.udp_port); w.u32(p.heartbeat_ms);
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,SessionInfo& o) {
    R r(b); SessionInfo p;
    if (!r.raw(p.token.data(),p.token.size()) || !r.u16(p.udp_port) ||
        !r.u32(p.heartbeat_ms) || !r.done()) return false;
    o=p; return true;
}
bool encode(const UdpRegister& p,protocol::Bytes& b) {
    b.assign(p.token.begin(),p.token.end()); return true;
}
bool decode(const protocol::Bytes& b,UdpRegister& o) {
    if (b.size()!=32) return false;
    std::copy_n(b.begin(),32,o.token.begin()); return true;
}
bool encode(const UdpRegisterOk& p,protocol::Bytes& b) {
    W w; w.u8(p.registered?1:0); w.u16(p.code); b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,UdpRegisterOk& o) {
    R r(b); UdpRegisterOk p; std::uint8_t bit=0;
    if (!r.u8(bit) || bit>1 || !r.u16(p.code) || !r.done()) return false;
    p.registered=bit!=0; o=p; return true;
}
bool encode(const Heartbeat& p,protocol::Bytes& b) {
    W w; w.u64(p.nonce); w.u64(p.client_time_ms); b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,Heartbeat& o) {
    R r(b); Heartbeat p;
    if (!r.u64(p.nonce) || !r.u64(p.client_time_ms) || !r.done()) return false;
    o=p; return true;
}
bool encode(const HeartbeatAck& p,protocol::Bytes& b) {
    W w; w.u64(p.nonce); w.u64(p.client_time_ms); w.u64(p.server_time_ms);
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,HeartbeatAck& o) {
    R r(b); HeartbeatAck p;
    if (!r.u64(p.nonce) || !r.u64(p.client_time_ms) ||
        !r.u64(p.server_time_ms) || !r.done()) return false;
    o=p; return true;
}
bool encode(const ChatRequest& p,protocol::Bytes& b) {
    if (!validText(p.text)) return false;
    W w; w.u32(p.client_id); if (!w.str(p.text)) return false;
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,ChatRequest& o) {
    R r(b); ChatRequest p;
    if (!r.u32(p.client_id) || !readText(r,p.text) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const ChatEvent& p,protocol::Bytes& b) {
    if (!validName(p.sender) || !validText(p.text)) return false;
    W w; w.u32(p.server_id); if (!w.str(p.sender)) return false;
    w.u64(p.server_time_ms); if (!w.str(p.text)) return false;
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,ChatEvent& o) {
    R r(b); ChatEvent p;
    if (!r.u32(p.server_id) || !readName(r,p.sender) ||
        !r.u64(p.server_time_ms) || !readText(r,p.text) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const PrivateRequest& p,protocol::Bytes& b) {
    if (!validName(p.recipient) || !validText(p.text)) return false;
    W w; w.u32(p.client_id); if (!w.str(p.recipient) || !w.str(p.text)) return false;
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,PrivateRequest& o) {
    R r(b); PrivateRequest p;
    if (!r.u32(p.client_id) || !readName(r,p.recipient) || !readText(r,p.text) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const PrivateEvent& p,protocol::Bytes& b) {
    if (!validName(p.sender) || !validName(p.recipient) || !validText(p.text)) return false;
    W w; w.u32(p.server_id);
    if (!w.str(p.sender) || !w.str(p.recipient)) return false;
    w.u64(p.server_time_ms); if (!w.str(p.text)) return false;
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,PrivateEvent& o) {
    R r(b); PrivateEvent p;
    if (!r.u32(p.server_id) || !readName(r,p.sender) || !readName(r,p.recipient) ||
        !r.u64(p.server_time_ms) || !readText(r,p.text) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const UserList& p,protocol::Bytes& b) {
    if (p.users.size()>65535 || !std::is_sorted(p.users.begin(),p.users.end()) ||
        std::adjacent_find(p.users.begin(),p.users.end())!=p.users.end()) return false;
    W w; w.u32(p.revision); w.u16(static_cast<std::uint16_t>(p.users.size()));
    for (const auto& s:p.users) if (!validName(s) || !w.str(s)) return false;
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,UserList& o) {
    R r(b); UserList p; std::uint16_t count=0;
    if (!r.u32(p.revision) || !r.u16(count)) return false;
    for (std::uint16_t i=0;i<count;++i) {
        std::string user; if (!readName(r,user)) return false;
        p.users.push_back(std::move(user));
    }
    if (!r.done() || !std::is_sorted(p.users.begin(),p.users.end()) ||
        std::adjacent_find(p.users.begin(),p.users.end())!=p.users.end()) return false;
    o=std::move(p); return true;
}
bool encode(const Ack& p,protocol::Bytes& b) {
    if (p.status>2) return false;
    W w; w.u8(p.acknowledged_type); w.u32(p.request_sequence); w.u8(p.status);
    if (!w.str(p.detail)) return false;
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,Ack& o) {
    R r(b); Ack p;
    if (!r.u8(p.acknowledged_type) || !r.u32(p.request_sequence) ||
        !r.u8(p.status) || p.status>2 || !r.str(p.detail) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const Error& p,protocol::Bytes& b) {
    W w; w.u16(p.code); w.u8(p.related_type); w.u32(p.related_sequence);
    if (!w.str(p.detail)) return false;
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,Error& o) {
    R r(b); Error p;
    if (!r.u16(p.code) || !r.u8(p.related_type) || !r.u32(p.related_sequence) ||
        !r.str(p.detail) || !r.done()) return false;
    o=std::move(p); return true;
}
bool encode(const ServerNotice& p,protocol::Bytes& b) {
    if (p.severity>2) return false;
    W w; w.u8(p.severity); if (!w.str(p.message)) return false;
    b=w.take(); return true;
}
bool decode(const protocol::Bytes& b,ServerNotice& o) {
    R r(b); ServerNotice p;
    if (!r.u8(p.severity) || p.severity>2 || !r.str(p.message) || !r.done()) return false;
    o=std::move(p); return true;
}
} // namespace messages
