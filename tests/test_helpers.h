#pragma once

#include "../clients/client.h"
#include "../server/server.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace test {

inline void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline AgentEvent waitForEvent(
    DcapClient& client,
    const std::string& expectedType,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& event : client.takeEvents()) {
            if (event.type == expectedType) {
                return event;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Timed out waiting for event: " + expectedType);
}

inline bool noEvent(
    DcapClient& client,
    const std::string& unexpectedType,
    std::chrono::milliseconds timeout
) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& event : client.takeEvents()) {
            if (event.type == unexpectedType) {
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

inline bool contains(
    const std::vector<std::string>& values,
    const std::string& expected
) {
    return std::find(values.begin(), values.end(), expected) != values.end();
}

class TestServer {
public:
    TestServer() {
        if (!initializeSocketRuntime()) {
            throw std::runtime_error("Could not initialize socket runtime");
        }
        runtime_initialized_ = true;
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        key_path_ = std::filesystem::temp_directory_path() /
            ("dcap-test-key-" + std::to_string(stamp) + ".key");

#ifdef _WIN32
        if (_putenv_s("DCAP_KEY_FILE", key_path_.string().c_str()) != 0) {
            cleanupSocketRuntime();
            runtime_initialized_ = false;
            throw std::runtime_error("Could not configure DCAP_KEY_FILE");
        }
#else
        if (setenv("DCAP_KEY_FILE", key_path_.string().c_str(), 1) != 0) {
            cleanupSocketRuntime();
            runtime_initialized_ = false;
            throw std::runtime_error("Could not configure DCAP_KEY_FILE");
        }
#endif

        if (!server_.start()) {
            server_.stop();
            std::error_code ignored;
            std::filesystem::remove(key_path_, ignored);
            cleanupSocketRuntime();
            runtime_initialized_ = false;
            throw std::runtime_error(
                "Could not start test server. Check whether TCP 54000 "
                "or UDP 54001 is already in use."
            );
        }
    }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    ~TestServer() {
        server_.stop();
        std::error_code ignored;
        std::filesystem::remove(key_path_, ignored);
        if (runtime_initialized_) {
            cleanupSocketRuntime();
        }
    }

    Server& server() {
        return server_;
    }

private:
    std::filesystem::path key_path_;
    Server server_;
    bool runtime_initialized_ = false;
};

inline void connectAndLogin(DcapClient& client, const std::string& username) {
    require(client.connect(), "Client connection failed: " + client.lastError());
    require(client.login(username), "Could not send AUTH for " + username);
    const auto event = waitForEvent(client, "LOGIN_OK");
    require(
        event.data.value("username", std::string()) == username,
        "AUTH_OK returned an unexpected username"
    );
}

} // namespace test