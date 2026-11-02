#include "socket_compat.h"
#include <algorithm>
#include <climits>

#ifdef _WIN32
bool initializeSocketRuntime() {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}
void cleanupSocketRuntime() { WSACleanup(); }
void closeSocket(SocketHandle s) { if (s != INVALID_SOCKET_HANDLE) closesocket(s); }
void shutdownSocket(SocketHandle s) { if (s != INVALID_SOCKET_HANDLE) shutdown(s, SD_BOTH); }
int lastSocketError() { return WSAGetLastError(); }
bool isInterruptedError(int e) { return e == WSAEINTR; }
bool setReuseAddress(SocketHandle s) {
    BOOL value = TRUE;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                      reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}
bool setSocketTimeout(SocketHandle s, int receiveTimeoutMs, int sendTimeoutMs) {
    bool ok = true;
    if (receiveTimeoutMs > 0) {
        DWORD value = static_cast<DWORD>(receiveTimeoutMs);
        ok = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0 && ok;
    }
    if (sendTimeoutMs > 0) {
        DWORD value = static_cast<DWORD>(sendTimeoutMs);
        ok = setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0 && ok;
    }
    return ok;
}
bool setSocketKeepAlive(SocketHandle s) {
    BOOL enabled = TRUE;
    return setsockopt(s, SOL_SOCKET, SO_KEEPALIVE,
                      reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
}
bool sendAll(SocketHandle s, const std::uint8_t* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        int chunk = static_cast<int>(std::min<std::size_t>(length - sent, INT_MAX));
        int n = send(s, reinterpret_cast<const char*>(data + sent), chunk, 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}
bool receiveExact(SocketHandle s, std::uint8_t* data, std::size_t length) {
    std::size_t received = 0;
    while (received < length) {
        int chunk = static_cast<int>(std::min<std::size_t>(length - received, INT_MAX));
        int n = recv(s, reinterpret_cast<char*>(data + received), chunk, 0);
        if (n <= 0) return false;
        received += static_cast<std::size_t>(n);
    }
    return true;
}
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
bool initializeSocketRuntime() {
    signal(SIGPIPE, SIG_IGN);
    return true;
}
void cleanupSocketRuntime() {}
void closeSocket(SocketHandle s) { if (s != INVALID_SOCKET_HANDLE) close(s); }
void shutdownSocket(SocketHandle s) { if (s != INVALID_SOCKET_HANDLE) shutdown(s, SHUT_RDWR); }
int lastSocketError() { return errno; }
bool isInterruptedError(int e) { return e == EINTR; }
bool setReuseAddress(SocketHandle s) {
    int value = 1;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) == 0;
}
bool setSocketTimeout(SocketHandle s, int receiveTimeoutMs, int sendTimeoutMs) {
    bool ok = true;
    if (receiveTimeoutMs > 0) {
        timeval value{receiveTimeoutMs / 1000, (receiveTimeoutMs % 1000) * 1000};
        ok = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0 && ok;
    }
    if (sendTimeoutMs > 0) {
        timeval value{sendTimeoutMs / 1000, (sendTimeoutMs % 1000) * 1000};
        ok = setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)) == 0 && ok;
    }
    return ok;
}
bool setSocketKeepAlive(SocketHandle s) {
    int enabled = 1;
    return setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) == 0;
}
bool sendAll(SocketHandle s, const std::uint8_t* data, std::size_t length) {
    std::size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(s, data + sent, length - sent, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}
bool receiveExact(SocketHandle s, std::uint8_t* data, std::size_t length) {
    std::size_t received = 0;
    while (received < length) {
        ssize_t n = recv(s, data + received, length - received, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        received += static_cast<std::size_t>(n);
    }
    return true;
}
#endif
