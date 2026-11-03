#include "user_manager.h"
#include <algorithm>
#include <cstring>

bool UserManager::add(const std::shared_ptr<ClientSession>& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(users_.count(s->username)) return false;
    users_[s->username]=s; ++revision_; return true;
}
void UserManager::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if(users_.erase(name)) ++revision_;
}
std::shared_ptr<ClientSession> UserManager::find(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it=users_.find(name); return it==users_.end()?nullptr:it->second;
}
std::vector<std::shared_ptr<ClientSession>> UserManager::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<ClientSession>> v;
    for(auto& p:users_) v.push_back(p.second);
    return v;
}
std::vector<std::string> UserManager::usernames() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> v;
    for(auto& p:users_) v.push_back(p.first);
    std::sort(v.begin(),v.end()); return v;
}
bool UserManager::bindUdp(const std::array<std::uint8_t,32>& token,
                          const sockaddr_in& endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto& p:users_) {
        auto& s=p.second;
        if(s->udpToken==token) {
            s->udpEndpoint=endpoint; s->udpRegistered=true;
            s->lastHeartbeatMs=0; return true;
        }
    }
    return false;
}
std::shared_ptr<ClientSession> UserManager::findByUdpEndpoint(const sockaddr_in& ep) {
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto& p:users_) {
        auto s=p.second;
        if(s->udpRegistered && s->udpEndpoint.sin_addr.s_addr==ep.sin_addr.s_addr &&
           s->udpEndpoint.sin_port==ep.sin_port) return s;
    }
    return nullptr;
}
std::vector<std::shared_ptr<ClientSession>> UserManager::expiredUdp(std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<ClientSession>> out;
    for(auto& p:users_) {
        auto s=p.second;
        if(s->udpRegistered && s->lastHeartbeatMs &&
           now>=s->lastHeartbeatMs && now-s->lastHeartbeatMs>15000) {
            s->udpRegistered=false; out.push_back(s);
        }
    }
    return out;
}
std::uint32_t UserManager::revision() const {
    std::lock_guard<std::mutex> lock(mutex_); return revision_;
}
