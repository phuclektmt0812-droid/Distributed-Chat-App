#include "client.h"

#include "../protocol/packet.h"
#include "../protocol/protocol.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstring>
#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/time.h>
#endif

using nlohmann::json;

namespace {
std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
std::string idFromPayload(const protocol::Bytes& payload) {
    if (payload.size() < 16) return {};
    fileproto::TransferId id{};
    std::copy_n(payload.begin(), 16, id.begin());
    return fileproto::idHex(id);
}
}

DcapClient::DcapClient(std::string address, std::uint16_t tcpPort, std::uint16_t udpPort,
                       std::filesystem::path receivedFilesDirectory)
    : server_address_(std::move(address)), server_port_(tcpPort), udp_port_(udpPort),
      received_files_directory_(receivedFilesDirectory.empty()
          ? std::filesystem::current_path() / "received_files"
          : std::move(receivedFilesDirectory)) {}

DcapClient::~DcapClient() { disconnect(); }

bool DcapClient::connect() {
    if (connected_) return true;
    try { encryption_key_=protocol::readKeyFromFile(protocol::keyFilePathFromEnvironment()); }
    catch(const std::exception& error) { std::lock_guard<std::mutex> lock(state_mutex_); last_error_=error.what(); return false; }
    if (tcp_thread_.joinable() || udp_thread_.joinable() || maintenance_thread_.joinable() ||
        tcp_socket_ != INVALID_SOCKET_HANDLE || udp_socket_ != INVALID_SOCKET_HANDLE) {
        disconnect();
    }
    if (server_address_.empty() || server_port_ == 0 || !initializeSocketRuntime()) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "Invalid server address or socket initialization failed";
        return false;
    }

    socket_runtime_initialized_ = true;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string port = std::to_string(server_port_);
    if (getaddrinfo(server_address_.c_str(), port.c_str(), &hints, &addresses) != 0 ||
        !addresses) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "Could not resolve chat server address";
        if (socket_runtime_initialized_.exchange(false)) cleanupSocketRuntime();
        return false;
    }

    SocketHandle tcp = INVALID_SOCKET_HANDLE;
    sockaddr_in udpDestination{};
    for (auto* current = addresses; current; current = current->ai_next) {
        tcp = ::socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (tcp != INVALID_SOCKET_HANDLE &&
            ::connect(tcp, current->ai_addr, static_cast<SocketLength>(current->ai_addrlen)) == 0) {
            std::memcpy(&udpDestination, current->ai_addr, sizeof(sockaddr_in));
            break;
        }
        if (tcp != INVALID_SOCKET_HANDLE) closeSocket(tcp);
        tcp = INVALID_SOCKET_HANDLE;
    }
    freeaddrinfo(addresses);
    if (tcp == INVALID_SOCKET_HANDLE) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "Could not connect to TCP server";
        if (socket_runtime_initialized_.exchange(false)) cleanupSocketRuntime();
        return false;
    }

    // Keep dead peers detectable and prevent a blocked TCP send from stalling the UI.
    if (!setSocketTimeout(tcp, 0, 10000) || !setSocketKeepAlive(tcp)) {
        closeSocket(tcp);
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "Could not configure TCP socket resilience options";
        if (socket_runtime_initialized_.exchange(false)) cleanupSocketRuntime();
        return false;
    }

    SocketHandle udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp == INVALID_SOCKET_HANDLE) {
        closeSocket(tcp);
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "Could not create UDP socket";
        if (socket_runtime_initialized_.exchange(false)) cleanupSocketRuntime();
        return false;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (::bind(udp, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        closeSocket(tcp); closeSocket(udp);
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "Could not bind UDP socket";
        if (socket_runtime_initialized_.exchange(false)) cleanupSocketRuntime();
        return false;
    }
    udpDestination.sin_port = htons(udp_port_);
#ifdef _WIN32
    DWORD timeoutMs = 1000;
    setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    timeval timeout{1,0};
    setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

    tcp_socket_ = tcp;
    udp_socket_ = udp;
    server_udp_ = udpDestination;
    connected_ = true;
    running_ = true;
    try {
        tcp_thread_ = std::thread(&DcapClient::tcpLoop, this);
        udp_thread_ = std::thread(&DcapClient::udpLoop, this);
        maintenance_thread_ = std::thread(&DcapClient::maintenanceLoop, this);
    } catch (...) {
        disconnect();
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = "Could not start client network threads";
        return false;
    }
    return true;
}

bool DcapClient::login(const std::string& name) {
    if (!connected_ || name.empty() || name.size() > protocol::MAX_USERNAME_BYTES ||
        !protocol::validUtf8(name)) return false;
    protocol::Packet packet;
    packet.type = protocol::Type::AUTH;
    if (!messages::encode(messages::Auth{name}, packet.payload)) return false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        username_ = name;
    }
    return sendTcp(std::move(packet));
}

bool DcapClient::sendTcp(protocol::Packet packet) {
    if (!connected_ || tcp_socket_ == INVALID_SOCKET_HANDLE) return false;
    if (packet.sequence == 0) packet.sequence = tcp_sequence_.next();
    std::lock_guard<std::mutex> lock(tcp_send_mutex_);
    return protocol::sendTcpPacketSecure(tcp_socket_, packet, encryption_key_);
}

bool DcapClient::sendUdp(protocol::Packet packet) {
    if (!connected_ || udp_socket_ == INVALID_SOCKET_HANDLE) return false;
    packet.sequence = udp_sequence_.next();
    std::lock_guard<std::mutex> lock(udp_send_mutex_);
    return protocol::sendUdpPacketSecure(udp_socket_, packet, server_udp_, encryption_key_);
}

bool DcapClient::sendGlobal(const std::string& text) {
    if (!authenticated_ || text.empty() || text.size() > protocol::MAX_CHAT_BYTES) return false;
    protocol::Packet packet;
    packet.type = protocol::Type::MSG;
    if (!messages::encode(messages::ChatRequest{tcp_sequence_.next(), text}, packet.payload)) return false;
    return sendTcp(std::move(packet));
}

bool DcapClient::sendPrivate(const std::string& recipient, const std::string& text) {
    if (!authenticated_ || recipient.empty() || text.empty() ||
        text.size() > protocol::MAX_CHAT_BYTES) return false;
    protocol::Packet packet;
    packet.type = protocol::Type::PRIVATE_MSG;
    if (!messages::encode(messages::PrivateRequest{tcp_sequence_.next(), recipient, text}, packet.payload)) return false;
    return sendTcp(std::move(packet));
}

std::string DcapClient::sendFile(const std::string& recipient, const std::filesystem::path& path) {
    if (!authenticated_ || recipient.empty()) return {};
    try {
        auto sender = std::make_unique<filetransfer::FileSender>(path.string(), recipient, udp_sequence_);
        const auto id = fileproto::idHex(sender->offer().id);
        auto offer = sender->makeOffer(0);
        offer.sequence = tcp_sequence_.next();
        const auto offerSequence = offer.sequence;
        {
            std::lock_guard<std::mutex> lock(transfers_mutex_);
            if (senders_.count(id)) return {};
            senders_.emplace(id, std::move(sender));
            pending_file_offers_[offerSequence] = id;
        }
        if (!sendTcp(std::move(offer))) {
            std::lock_guard<std::mutex> lock(transfers_mutex_);
            senders_.erase(id);
            pending_file_offers_.erase(offerSequence);
            return {};
        }
        emit("FILE_PROGRESS", {{"transferId", id}, {"filename", path.filename().string()},
                               {"progress", 0}, {"status", "Waiting for recipient"}});
        return id;
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = error.what();
        return {};
    }
}

bool DcapClient::decideFile(const std::string& id, bool accept) {
    fileproto::Offer offer;
    {
        std::lock_guard<std::mutex> lock(transfers_mutex_);
        auto found = incoming_offers_.find(id);
        if (found == incoming_offers_.end()) return false;
        offer = found->second;
        if (accept) {
            try {
                auto receiver = std::make_unique<filetransfer::FileReceiver>(
                    offer, received_files_directory_, udp_sequence_);
                receivers_[id] = std::move(receiver);
            } catch (const std::exception& error) {
                emit("ERROR", {{"error", error.what()}});
                return false;
            }
        }
    }
    fileproto::Decision decision;
    decision.id = offer.id;
    decision.accepted = accept;
    decision.reason = accept ? "" : "Rejected by recipient";
    protocol::Packet packet;
    packet.type = protocol::Type::FILE_DECISION;
    if (!fileproto::encode(decision, packet.payload) || !sendTcp(std::move(packet))) {
        std::lock_guard<std::mutex> lock(transfers_mutex_);
        receivers_.erase(id);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(transfers_mutex_);
        incoming_offers_.erase(id);
    }
    return true;
}

void DcapClient::tcpLoop() {
    while (running_) {
        protocol::Packet packet;
        if (!protocol::receiveTcpPacketSecure(tcp_socket_, packet, encryption_key_)) break;
        handleTcp(packet);
    }
    if (running_.exchange(false)) {
        connected_ = false;
        authenticated_ = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_error_ = "TCP connection closed by the server or network";
        }
        emit("DISCONNECTED", {{"reason", "TCP connection closed by the server or network"}});
    }
}

void DcapClient::udpLoop() {
    std::uint8_t buffer[protocol::MAX_UDP_DATAGRAM + 1];
    while (running_) {
        sockaddr_in source{};
        SocketLength length = sizeof(source);
#ifdef _WIN32
        const int received = recvfrom(udp_socket_, reinterpret_cast<char*>(buffer),
            static_cast<int>(sizeof(buffer)), 0, reinterpret_cast<sockaddr*>(&source), &length);
#else
        const auto received = recvfrom(udp_socket_, buffer, sizeof(buffer), 0,
            reinterpret_cast<sockaddr*>(&source), &length);
#endif
        if (received <= 0) {
            if (!running_) break;
            continue;
        }
        protocol::Packet packet;
        if (protocol::decodeSecureDatagram(buffer, static_cast<std::size_t>(received), encryption_key_, packet))
            handleUdp(packet);
    }
}

void DcapClient::maintenanceLoop() {
    auto heartbeatAt = std::chrono::steady_clock::now();
    while (running_) {
        const auto now = std::chrono::steady_clock::now();
        if (authenticated_ && udp_registered_ && now >= heartbeatAt) {
            messages::Heartbeat heartbeat{nowMs(), nowMs()};
            protocol::Packet packet;
            packet.type = protocol::Type::HEARTBEAT;
            if (messages::encode(heartbeat, packet.payload)) sendUdp(std::move(packet));
            heartbeatAt = now + std::chrono::milliseconds(protocol::HEARTBEAT_INTERVAL_MS);
        }

        {
            std::lock_guard<std::mutex> lock(transfers_mutex_);
            for (auto it = senders_.begin(); it != senders_.end();) {
                auto& sender = *it->second;
                for (auto& packet : sender.poll(now)) sendUdp(std::move(packet));
                const auto total = sender.totalBytes();
                const auto done = sender.acknowledgedBytes();
                const double progress = total ? (100.0 * done / total) :
                    (sender.state() == filetransfer::SenderState::COMPLETED ? 100.0 : 0.0);
                emit("FILE_PROGRESS", {{"transferId", it->first},
                    {"filename", sender.offer().filename}, {"progress", progress},
                    {"status", sender.state() == filetransfer::SenderState::WAIT_DECISION
                        ? "Waiting for recipient" : "Transferring"}});
                if (sender.state() == filetransfer::SenderState::COMPLETED) {
                    emit("FILE_COMPLETE", {{"transferId", it->first},
                        {"filename", sender.offer().filename}, {"progress", 100}});
                    it = senders_.erase(it);
                } else if (sender.state() == filetransfer::SenderState::FAILED) {
                    emit("ERROR", {{"error", sender.lastError()}, {"transferId", it->first}});
                    it = senders_.erase(it);
                } else ++it;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void DcapClient::handleTcp(const protocol::Packet& packet) {
    switch (packet.type) {
    case protocol::Type::AUTH_OK: {
        messages::AuthOk data;
        if (!messages::decode(packet.payload, data)) break;
        authenticated_ = true;
        emit("LOGIN_OK", {{"username", data.username}});
        break;
    }
    case protocol::Type::AUTH_FAIL: {
        messages::AuthFail data;
        if (!messages::decode(packet.payload, data)) break;
        authenticated_ = false;
        { std::lock_guard<std::mutex> lock(state_mutex_); username_.clear(); }
        emit("LOGIN_FAIL", {{"error", data.reason}, {"code", data.code}});
        break;
    }
    case protocol::Type::SESSION_INFO: {
        messages::SessionInfo info;
        if (!messages::decode(packet.payload, info)) break;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            udp_token_ = info.token;
        }
        messages::UdpRegister registration{info.token};
        protocol::Packet request;
        request.type = protocol::Type::UDP_REGISTER;
        if (messages::encode(registration, request.payload)) sendUdp(std::move(request));
        break;
    }
    case protocol::Type::USER_LIST: {
        messages::UserList data;
        if (!messages::decode(packet.payload, data)) break;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            users_ = data.users;
        }
        emit("USER_LIST", {{"users", data.users}, {"revision", data.revision}});
        break;
    }
    case protocol::Type::MSG: {
        messages::ChatEvent data;
        if (!messages::decode(packet.payload, data)) break;
        emit("CHAT_ALL", {{"sender", data.sender}, {"content", data.text},
            {"timestamp", timestamp(data.server_time_ms)}, {"server_id", data.server_id}});
        break;
    }
    case protocol::Type::PRIVATE_MSG: {
        messages::PrivateEvent data;
        if (!messages::decode(packet.payload, data)) break;
        emit("CHAT_PRIVATE", {{"sender", data.sender}, {"recipient", data.recipient},
            {"content", data.text}, {"private", true},
            {"timestamp", timestamp(data.server_time_ms)}, {"server_id", data.server_id}});
        break;
    }
    case protocol::Type::FILE_OFFER: {
        fileproto::Offer offer;
        if (!fileproto::decode(packet.payload, offer)) break;
        const auto id = fileproto::idHex(offer.id);
        {
            std::lock_guard<std::mutex> lock(transfers_mutex_);
            incoming_offers_[id] = offer;
        }
        emit("FILE_OFFER", {{"transfer_id", id}, {"transferId", id},
            {"sender", ""}, {"filename", offer.filename}, {"size", offer.size},
            {"recipient", offer.recipient}});
        break;
    }
    case protocol::Type::FILE_DECISION: {
        const auto id = idFromPayload(packet.payload);
        std::lock_guard<std::mutex> lock(transfers_mutex_);
        auto it = senders_.find(id);
        if (it != senders_.end()) it->second->handleDecision(packet);
        for (auto pending = pending_file_offers_.begin(); pending != pending_file_offers_.end();) {
            if (pending->second == id) pending = pending_file_offers_.erase(pending);
            else ++pending;
        }
        break;
    }
    case protocol::Type::FILE_RESULT: {
        fileproto::Result result;
        if (!fileproto::decode(packet.payload, result)) break;
        const auto id = fileproto::idHex(result.id);
        bool senderFound = false;
        {
            std::lock_guard<std::mutex> lock(transfers_mutex_);
            auto sender = senders_.find(id);
            if (sender != senders_.end()) {
                senderFound = true;
                if (result.status == 1) sender->second->completeFromServer();
                else sender->second->failFromServer(result.detail);
                senders_.erase(sender);
            }
            auto receiver = receivers_.find(id);
            if (result.status != 1 && receiver != receivers_.end()) {
                receiver->second->cancel();
                receivers_.erase(receiver);
            }
            for (auto pending = pending_file_offers_.begin(); pending != pending_file_offers_.end();) {
                if (pending->second == id) pending = pending_file_offers_.erase(pending);
                else ++pending;
            }
        }
        if (result.status != 1 || senderFound) {
            emit(result.status == 1 ? "FILE_COMPLETE" : "FILE_RESULT",
                {{"transferId", id}, {"transfer_id", id}, {"status", result.status},
                 {"progress", result.status == 1 ? 100 : 0}, {"detail", result.detail}});
        }
        break;
    }
    case protocol::Type::ERROR: {
        messages::Error error;
        if (messages::decode(packet.payload, error)) {
            {
                std::lock_guard<std::mutex> lock(transfers_mutex_);
                auto pending = pending_file_offers_.find(error.related_sequence);
                if (pending != pending_file_offers_.end()) {
                    auto sender = senders_.find(pending->second);
                    if (sender != senders_.end()) sender->second->failFromServer(error.detail);
                    pending_file_offers_.erase(pending);
                }
            }
            emit("ERROR", {{"error", error.detail}, {"code", error.code}});
        }
        break;
    }
    case protocol::Type::SERVER_NOTICE: {
        messages::ServerNotice notice;
        if (messages::decode(packet.payload, notice))
            emit("SERVER_NOTICE", {{"message", notice.message}, {"severity", notice.severity}});
        break;
    }
    case protocol::Type::PING: {
        protocol::Packet pong;
        pong.type = protocol::Type::PONG;
        pong.payload = packet.payload;
        sendTcp(std::move(pong));
        break;
    }
    default: break;
    }
}

void DcapClient::handleUdp(const protocol::Packet& packet) {
    if (packet.type == protocol::Type::UDP_REGISTER_OK) {
        messages::UdpRegisterOk result;
        if (messages::decode(packet.payload, result)) {
            udp_registered_ = result.registered;
            if (!result.registered) emit("ERROR", {{"error", "UDP registration rejected"}});
        }
        return;
    }
    if (packet.type == protocol::Type::HEARTBEAT_ACK) return;
    const auto id = idFromPayload(packet.payload);
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(transfers_mutex_);
    auto sender = senders_.find(id);
    if (sender != senders_.end()) {
        sender->second->onPacket(packet);
        return;
    }
    auto receiver = receivers_.find(id);
    if (receiver != receivers_.end()) {
        for (auto& reply : receiver->second->onPacket(packet)) sendUdp(std::move(reply));
        if (receiver->second->state() == filetransfer::ReceiverState::COMPLETED) {
            emit("FILE_COMPLETE", {{"transferId", id},
                {"filename", receiver->second->completedPath().filename().string()},
                {"progress", 100}, {"status", "Received"}, {"received", true}});
            receivers_.erase(receiver);
        } else if (receiver->second->state() == filetransfer::ReceiverState::FAILED) {
            emit("ERROR", {{"error", receiver->second->lastError()}, {"transferId", id}});
            receivers_.erase(receiver);
        } else {
            const auto total = receiver->second->totalBytes();
            const auto received = receiver->second->receivedBytes();
            const double progress = total ? (100.0 * received / total) : 0.0;
            emit("FILE_PROGRESS", {{"transferId", id},
                {"filename", receiver->second->filename()}, {"progress", progress},
                {"status", "Receiving"}});
        }
    }
}

void DcapClient::emit(std::string type, json data) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    events_.push_back({std::move(type), std::move(data)});
    while (events_.size() > 1000) events_.pop_front();
}

std::vector<AgentEvent> DcapClient::takeEvents() {
    std::vector<AgentEvent> out;
    std::lock_guard<std::mutex> lock(events_mutex_);
    while (!events_.empty()) {
        out.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    return out;
}

std::string DcapClient::username() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return username_;
}
std::vector<std::string> DcapClient::users() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return users_;
}
std::string DcapClient::lastError() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}
std::string DcapClient::timestamp(std::uint64_t ms) {
    const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

void DcapClient::logout() {
    if (authenticated_) {
        protocol::Packet packet;
        packet.type = protocol::Type::LOGOUT;
        sendTcp(std::move(packet));
    }
    disconnect();
}

void DcapClient::disconnect() {
    const bool wasRunning = running_.exchange(false);
    authenticated_ = false;
    connected_ = false;
    if (tcp_socket_ != INVALID_SOCKET_HANDLE) shutdownSocket(tcp_socket_);
    if (udp_socket_ != INVALID_SOCKET_HANDLE) shutdownSocket(udp_socket_);
    if (tcp_thread_.joinable() && tcp_thread_.get_id() != std::this_thread::get_id()) tcp_thread_.join();
    if (udp_thread_.joinable() && udp_thread_.get_id() != std::this_thread::get_id()) udp_thread_.join();
    if (maintenance_thread_.joinable() && maintenance_thread_.get_id() != std::this_thread::get_id()) maintenance_thread_.join();
    if (tcp_socket_ != INVALID_SOCKET_HANDLE) { closeSocket(tcp_socket_); tcp_socket_ = INVALID_SOCKET_HANDLE; }
    if (udp_socket_ != INVALID_SOCKET_HANDLE) { closeSocket(udp_socket_); udp_socket_ = INVALID_SOCKET_HANDLE; }
    udp_registered_ = false;
    { std::lock_guard<std::mutex> lock(state_mutex_); username_.clear(); users_.clear(); }
    {
        std::lock_guard<std::mutex> lock(transfers_mutex_);
        senders_.clear(); receivers_.clear(); incoming_offers_.clear(); pending_file_offers_.clear();
    }
    if (wasRunning) emit("DISCONNECTED");
    if (socket_runtime_initialized_.exchange(false)) cleanupSocketRuntime();
}
