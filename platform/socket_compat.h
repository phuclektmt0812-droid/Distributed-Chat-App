#pragma once
#include <cstdint>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

// Windows headers define ERROR and FILE_END as macros. They collide with
// protocol::Type enumerators using the same names, so remove them after the
// Windows headers have been included.
#ifdef ERROR
#undef ERROR
#endif
#ifdef FILE_END
#undef FILE_END
#endif
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

bool initializeSocketRuntime();
void cleanupSocketRuntime();
void closeSocket(SocketHandle s);
void shutdownSocket(SocketHandle s);
int lastSocketError();
bool isInterruptedError(int errorCode);
bool setReuseAddress(SocketHandle s);
bool setSocketTimeout(SocketHandle s, int receiveTimeoutMs, int sendTimeoutMs);
bool setSocketKeepAlive(SocketHandle s);
bool sendAll(SocketHandle s, const std::uint8_t* data, std::size_t length);
bool receiveExact(SocketHandle s, std::uint8_t* data, std::size_t length);
