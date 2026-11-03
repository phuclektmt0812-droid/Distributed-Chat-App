#include "../server/user_manager.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::shared_ptr<ClientSession> makeSession(const std::string& name, std::uint8_t seed) {
    auto s = std::make_shared<ClientSession>();
    s->username = name;
    for (std::size_t i = 0; i < s->udpToken.size(); ++i)
        s->udpToken[i] = static_cast<std::uint8_t>(seed + i);
    return s;
}
void testBasicLifecycle() {
    UserManager users;
    const auto initial = users.revision();
    auto alice = makeSession("alice", 1);
    auto bob = makeSession("bob", 2);
    require(users.add(alice), "first add must succeed");
    require(!users.add(alice), "duplicate username must fail");
    require(users.revision() == initial + 1, "revision changes only on successful add");
    require(users.add(bob), "second distinct user must succeed");
    require(users.find("alice") == alice, "find must return exact session");
    require(!users.find("missing"), "find must return null for missing user");
    const auto names = users.usernames();
    require(names == std::vector<std::string>({"alice", "bob"}), "usernames must be sorted");
    require(users.snapshot().size() == 2, "snapshot must contain all sessions");
    users.remove("missing");
    require(users.revision() == initial + 2, "removing missing user must not change revision");
    users.remove("alice");
    require(!users.find("alice"), "removed user must no longer be found");
    require(users.revision() == initial + 3, "successful remove increments revision");
    users.remove("alice");
    require(users.revision() == initial + 3, "repeated remove must be idempotent");
}
void testUdpBindingAndExpiry() {
    UserManager users;
    auto user = makeSession("udp-user", 17);
    require(users.add(user), "could not add UDP test user");
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(0x7f000001);
    endpoint.sin_port = htons(43210);
    std::array<std::uint8_t, 32> invalid{};
    require(!users.bindUdp(invalid, endpoint), "unknown UDP token must not bind");
    require(!users.findByUdpEndpoint(endpoint), "unregistered endpoint must not resolve");
    require(users.bindUdp(user->udpToken, endpoint), "valid token must bind endpoint");
    require(user->udpRegistered, "session must be marked UDP registered");
    require(users.findByUdpEndpoint(endpoint) == user, "bound endpoint must resolve session");
    user->lastHeartbeatMs = 1000;
    require(users.expiredUdp(999).empty(), "clock rollback must not expire a future heartbeat");
    require(users.expiredUdp(16000).empty(), "exact 15-second boundary must not expire");
    const auto expired = users.expiredUdp(16001);
    require(expired.size() == 1 && expired.front() == user, "stale UDP session must expire");
    require(!user->udpRegistered, "expired UDP session must be unregistered");
    require(!users.findByUdpEndpoint(endpoint), "expired endpoint must stop resolving");
    require(users.expiredUdp(50000).empty(), "already-expired session must not expire twice");
}
void testConcurrentMutation() {
    UserManager users;
    constexpr int threadCount = 8;
    constexpr int perThread = 40;
    std::atomic<bool> succeeded{true};
    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < perThread; ++i) {
                auto s = makeSession("u-" + std::to_string(t) + "-" + std::to_string(i),
                                     static_cast<std::uint8_t>(t + i));
                if (!users.add(s) || users.find(s->username) != s) succeeded = false;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    require(succeeded.load(), "concurrent add/find failed");
    require(users.snapshot().size() == threadCount * perThread,
            "concurrent adds lost sessions");
    require(users.usernames().size() == threadCount * perThread,
            "concurrent usernames count mismatch");
    require(users.revision() == threadCount * perThread,
            "concurrent revision count mismatch");
}
}
int main() {
    try {
        testBasicLifecycle();
        testUdpBindingAndExpiry();
        testConcurrentMutation();
        std::cout << "UserManager tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "UserManager tests failed: " << e.what() << '\n';
        return 1;
    }
}