#include "user_manager.h"
#include "../protocol/constants.h"
#include "../protocol/protocol.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
constexpr int PBKDF2_ITERATIONS = 600000;

std::string toHex(const std::uint8_t* bytes, std::size_t length) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);
    for (std::size_t i = 0; i < length; ++i) {
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 0x0f]);
    }
    return result;
}

bool fromHex(const std::string& text, std::uint8_t* output, std::size_t length) {
    if (text.size() != length * 2) return false;
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < length; ++i) {
        const int high = nibble(text[i * 2]);
        const int low = nibble(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

bool validUsername(const std::string& username) {
    if (username.empty() || username.size() > protocol::MAX_USERNAME_BYTES ||
        !protocol::validUtf8(username)) return false;
    for (unsigned char c : username) {
        if (c < 32 || c == 127) return false;
    }
    return true;
}

bool validPassword(const std::string& password) {
    return password.size() >= 8 && password.size() <= 128 &&
           protocol::validUtf8(password);
}

bool derivePasswordHash(const std::string& password,
                        const std::array<std::uint8_t,16>& salt,
                        std::array<std::uint8_t,32>& output) {
    return PKCS5_PBKDF2_HMAC(
        password.data(), static_cast<int>(password.size()),
        salt.data(), static_cast<int>(salt.size()),
        PBKDF2_ITERATIONS, EVP_sha256(),
        static_cast<int>(output.size()), output.data()) == 1;
}
} // namespace

UserManager::UserManager() {
    const char* configuredPath = std::getenv("DCAP_ACCOUNT_FILE");
    account_store_path_ = configuredPath && *configuredPath
        ? std::filesystem::u8path(configuredPath)
        : std::filesystem::current_path() / "accounts.db";
    loadAccounts();
}

UserManager::~UserManager() = default;

bool UserManager::initializeAccountStore() {
    std::lock_guard<std::mutex> lock(mutex_);
    accounts_.reset();
    users_.clear();
    revision_ = 0;
    storage_ready_ = true;
    const char* configuredPath = std::getenv("DCAP_ACCOUNT_FILE");
    account_store_path_ = configuredPath && *configuredPath
        ? std::filesystem::u8path(configuredPath)
        : std::filesystem::current_path() / "accounts.db";
    loadAccounts();
    return storage_ready_;
}

UserManager::AccountNode* UserManager::findAccount(const std::string& username) {
    auto* current = accounts_.get();
    while (current) {
        if (current->username == username) return current;
        current = current->next.get();
    }
    return nullptr;
}

void UserManager::loadAccounts() {
    std::error_code error;
    if (!std::filesystem::exists(account_store_path_, error)) {
        if (error) storage_ready_ = false;
        return;
    }

    std::ifstream input(account_store_path_, std::ios::binary);
    if (!input) {
        storage_ready_ = false;
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream row(line);
        std::string usernameHex, saltHex, hashHex, extra;
        if (!(row >> usernameHex >> saltHex >> hashHex) || (row >> extra)) {
            storage_ready_ = false;
            continue;
        }

        std::vector<std::uint8_t> usernameBytes(usernameHex.size() / 2);
        std::array<std::uint8_t,16> salt{};
        std::array<std::uint8_t,32> passwordHash{};
        if (usernameHex.size() % 2 != 0 ||
            !fromHex(usernameHex, usernameBytes.data(), usernameBytes.size()) ||
            !fromHex(saltHex, salt.data(), salt.size()) ||
            !fromHex(hashHex, passwordHash.data(), passwordHash.size())) {
            storage_ready_ = false;
            continue;
        }
        const std::string username(usernameBytes.begin(), usernameBytes.end());
        if (!validUsername(username) || findAccount(username)) {
            storage_ready_ = false;
            continue;
        }

        auto account = std::make_unique<AccountNode>();
        account->username = username;
        account->salt = salt;
        account->passwordHash = passwordHash;
        account->next = std::move(accounts_);
        accounts_ = std::move(account);
    }
    if (!input.eof()) storage_ready_ = false;
}

bool UserManager::appendAccountRecord(const AccountNode& account) {
    try {
        const auto parent = account_store_path_.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        std::ofstream output(account_store_path_, std::ios::binary | std::ios::app);
        if (!output) return false;

        const auto* usernameBytes =
            reinterpret_cast<const std::uint8_t*>(account.username.data());
        output << toHex(usernameBytes, account.username.size()) << ' '
               << toHex(account.salt.data(), account.salt.size()) << ' '
               << toHex(account.passwordHash.data(), account.passwordHash.size())
               << '\n';
        output.flush();
        if (!output) return false;
        output.close();
#ifndef _WIN32
        if (::chmod(account_store_path_.c_str(), S_IRUSR | S_IWUSR) != 0) return false;
#endif
        return true;
    } catch (...) {
        return false;
    }
}

SignUpStatus UserManager::signUp(const std::string& username,
                                 const std::string& password) {
    if (!validUsername(username)) return SignUpStatus::INVALID_USERNAME;
    if (!validPassword(password)) return SignUpStatus::WEAK_PASSWORD;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!storage_ready_) return SignUpStatus::STORAGE_ERROR;
    if (findAccount(username)) return SignUpStatus::ACCOUNT_EXISTS;

    auto account = std::make_unique<AccountNode>();
    account->username = username;
    if (RAND_bytes(account->salt.data(),
                   static_cast<int>(account->salt.size())) != 1 ||
        !derivePasswordHash(password, account->salt, account->passwordHash)) {
        return SignUpStatus::STORAGE_ERROR;
    }
    if (!appendAccountRecord(*account)) return SignUpStatus::STORAGE_ERROR;

    account->next = std::move(accounts_);
    accounts_ = std::move(account);
    return SignUpStatus::SUCCESS;
}

AccountLoginStatus UserManager::login(
    const std::shared_ptr<ClientSession>& session, const std::string& password) {
    if (!session) return AccountLoginStatus::ACCOUNT_NOT_FOUND;
    std::lock_guard<std::mutex> lock(mutex_);
    auto* account = findAccount(session->username);
    if (!account) return AccountLoginStatus::ACCOUNT_NOT_FOUND;

    std::array<std::uint8_t,32> candidate{};
    if (!derivePasswordHash(password, account->salt, candidate))
        return AccountLoginStatus::INVALID_PASSWORD;
    const bool passwordMatches =
        CRYPTO_memcmp(candidate.data(), account->passwordHash.data(),
                      account->passwordHash.size()) == 0;
    OPENSSL_cleanse(candidate.data(), candidate.size());
    if (!passwordMatches) return AccountLoginStatus::INVALID_PASSWORD;

    if (auto active = account->activeSession.lock()) {
        if (active != session) return AccountLoginStatus::ALREADY_ONLINE;
    }
    if (users_.find(session->username) != users_.end())
        return AccountLoginStatus::ALREADY_ONLINE;

    account->activeSession = session;
    users_[session->username] = session;
    ++revision_;
    return AccountLoginStatus::SUCCESS;
}

void UserManager::remove(const std::shared_ptr<ClientSession>& session) {
    if (!session) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = users_.find(session->username);
    if (found == users_.end() || found->second != session) return;

    if (auto* account = findAccount(session->username)) {
        if (account->activeSession.lock() == session)
            account->activeSession.reset();
    }
    users_.erase(found);
    ++revision_;
}

std::shared_ptr<ClientSession> UserManager::find(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = users_.find(name);
    return it == users_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<ClientSession>> UserManager::snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<ClientSession>> result;
    result.reserve(users_.size());
    for (auto& entry : users_) result.push_back(entry.second);
    return result;
}

std::vector<std::string> UserManager::usernames() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(users_.size());
    for (auto& entry : users_) result.push_back(entry.first);
    std::sort(result.begin(), result.end());
    return result;
}

bool UserManager::bindUdp(const std::array<std::uint8_t,32>& token,
                          const sockaddr_in& endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : users_) {
        auto& session = entry.second;
        if (session->udpToken == token) {
            session->udpEndpoint = endpoint;
            session->udpRegistered = true;
            session->lastHeartbeatMs = 0;
            return true;
        }
    }
    return false;
}

std::shared_ptr<ClientSession> UserManager::findByUdpEndpoint(const sockaddr_in& endpoint) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : users_) {
        const auto& session = entry.second;
        if (session->udpRegistered &&
            session->udpEndpoint.sin_addr.s_addr == endpoint.sin_addr.s_addr &&
            session->udpEndpoint.sin_port == endpoint.sin_port) return session;
    }
    return nullptr;
}

std::vector<std::shared_ptr<ClientSession>> UserManager::expiredUdp(std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<ClientSession>> expired;
    for (auto& entry : users_) {
        const auto& session = entry.second;
        if (session->udpRegistered && session->lastHeartbeatMs &&
            now >= session->lastHeartbeatMs &&
            now - session->lastHeartbeatMs > 15000) {
            session->udpRegistered = false;
            expired.push_back(session);
        }
    }
    return expired;
}

std::uint32_t UserManager::revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

std::size_t UserManager::accountCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (auto* current = accounts_.get(); current; current = current->next.get())
        ++count;
    return count;
}
