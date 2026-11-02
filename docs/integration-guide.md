# INTEGRATION GUIDE — Distributed and Streaming-Based Client-Server Collaboration Application

- Shared wire contract: `protocol-spec.md`
- File-transfer contract: `file-transfer-spec.md`
- Target environment: Windows, macOS, and Linux; local loopback (`localhost`/`127.0.0.1`) or LAN IPv4 addressing
- Language/toolchain: C++17; a supported compiler for the host OS (MinGW-w64 GCC or MSVC on Windows, Clang on macOS, GCC or Clang on Linux)
- Network API: raw sockets through a small cross-platform compatibility layer (Winsock2 on Windows; BSD/POSIX sockets on macOS and Linux)

This document defines how the project modules fit together and how the team builds, runs, and integrates them. The protocol details belong in `protocol-spec.md`; file-transfer behavior belongs in `file-transfer-spec.md`.

---

## 1. Project architecture

The application has three executable/runtime roles:

1. **Server** — accepts TCP clients, authenticates usernames, manages sessions and online users, routes chat, handles UDP endpoint registration/heartbeat/discovery, and relays UDP file-transfer datagrams.
2. **Client Agent** — owns TCP and UDP sockets, protocol serialization, chat state, file-transfer state, and communicates with the UI.
3. **Browser UI** — displays chat, users, and transfer progress. JavaScript communicates only with the local Client Agent over HTTP.

```text
                         LAN / IPv4
+-------------------+       TCP :54000       +-----------------------+
| Browser UI        | <--------------------> | Client Agent A        |
| HTML/CSS/JS       |  HTTP :8080 (localhost)| TCP + UDP raw sockets |
+-------------------+                        +-----------+-----------+
                                                        |
                                      TCP chat/control  | UDP heartbeat,
                                      UDP file data     | file data
                                                        v
                                             +-----------------------+
                                             | Server                |
                                             | TCP :54000            |
                                             | UDP :54001            |
                                             | Sessions / routing    |
                                             +-----------+-----------+
                                                         ^
                                      TCP + UDP          |
                                             +-----------+-----------+
                                             | Client Agent B        |
                                             | TCP + UDP raw sockets |
                                             +-----------+-----------+
                                                        ^
                                             HTTP :8080  |
                                             +-----------+-----------+
                                             | Browser UI B          |
                                             +-----------------------+
```

### 1.1 Transport decisions

| Traffic | Transport | Port | Implementation |
|---|---|---:|---|
| Client ↔ Server login/chat/control | TCP | 54000 | Raw platform sockets through the compatibility layer |
| Client ↔ Server heartbeat/discovery | UDP | 54001 | Raw platform sockets through the compatibility layer |
| Client ↔ Server file data/ACK | UDP | 54001 | Reliable application protocol over datagrams |
| Browser ↔ local Client Agent | HTTP/1.1 TCP | 8080 | Minimal HTTP server using raw sockets |

The file data plane uses UDP because the assignment's Milestone 6 explicitly requires file chunking and reassembly over UDP. Login, chat, and transfer negotiation remain on TCP. This hybrid design also uses UDP for heartbeat and discovery as described in the assignment's techniques section.

## 2. Team responsibilities

| Member | Primary ownership | Main deliverables |
|---|---|---|
| Lê Hoàng Phúc | Server and user management | TCP listener, authentication, sessions, user list, message routing, UDP endpoint registry, heartbeat expiry, UDP relay |
| Võ Hoàng Phúc Duy | Client and UI | Client connection lifecycle, browser UI, local HTTP bridge, login/chat/user list views, transfer progress display |
| Nam Anh | Protocol, file transfer, security support | Packet codec, shared constants, UDP file sender/receiver logic, reliability, SHA-256 integration, protocol tests |

Ownership does not mean isolated implementation. The shared packet header, type registry, and payload layouts are fixed by `protocol-spec.md`. A member changing the wire format must update the spec and coordinate the dependent modules before merging.

## 3. Repository layout

Use this layout from the repository root:

```text
Distributed-Chat-App/
├── client/
│   ├── main.cpp
│   ├── client.h
│   ├── client.cpp
│   ├── tcp_client.h
│   ├── tcp_client.cpp
│   ├── udp_client.h
│   ├── udp_client.cpp
│   ├── file_sender.h
│   ├── file_sender.cpp
│   ├── file_receiver.h
│   └── file_receiver.cpp
├── server/
│   ├── main.cpp
│   ├── server.h
│   ├── server.cpp
│   ├── user_manager.h
│   ├── user_manager.cpp
│   ├── tcp_session.h
│   ├── tcp_session.cpp
│   ├── udp_service.h
│   ├── udp_service.cpp
│   ├── file_relay.h
│   └── file_relay.cpp
├── protocol/
│   ├── protocol.h
│   ├── protocol.cpp
│   ├── packet.h
│   ├── packet.cpp
│   └── constants.h
├── web/
│   ├── index.html
│   ├── style.css
│   └── app.js
├── ui_bridge/
│   ├── http_server.h
│   └── http_server.cpp
├── platform/
│   ├── socket_compat.h
│   └── socket_compat.cpp
├── tests/
│   └── protocol_tests.cpp
├── docs/
│   ├── integration-guide.md
│   ├── protocol-spec.md
│   ├── file-transfer-spec.md
│   ├── project-checklist.md
│   └── test-plan.md
├── received_files/
├── README.md
└── .gitignore
```

The team may adapt filenames, but keep module boundaries. The three specification files remain separate and are committed with the source code.

## 4. Module interfaces and responsibilities

### 4.1 Shared Protocol module

The Protocol module is used by both Client and Server. It owns:
- Message type constants and protocol constants.
- Packet header encoding and decoding.
- Big-endian integer helpers.
- Length-prefixed UTF-8 string helpers.
- TCP complete-packet read/write helpers.
- UDP datagram validation.
- Payload serializers/parsers for each message type.

Suggested interface:

```cpp
namespace protocol {
    constexpr std::uint32_t MAGIC = 0x44434150;
    constexpr std::uint8_t VERSION = 1;
    constexpr std::size_t HEADER_SIZE = 14;
    constexpr std::size_t MAX_TCP_PAYLOAD = 1024 * 1024;
    constexpr std::size_t MAX_UDP_DATAGRAM = 1200;

    enum class Type : std::uint8_t {
        AUTH = 1, AUTH_OK = 2, AUTH_FAIL = 3,
        MSG = 4, PRIVATE_MSG = 5, USER_LIST = 6,
        ACK = 7, ERROR = 8, LOGOUT = 9,
        PING = 10, PONG = 11,
        FILE_OFFER = 12, FILE_DECISION = 13,
        FILE_CANCEL = 14, FILE_RESULT = 15,
        SESSION_INFO = 16, SERVER_NOTICE = 17,

        UDP_REGISTER = 32, UDP_REGISTER_OK = 33,
        HEARTBEAT = 34, HEARTBEAT_ACK = 35,
        DISCOVERY = 36, DISCOVERY_REPLY = 37,

        FILE_START = 40, FILE_ACCEPT_UDP = 41,
        FILE_CHUNK = 42, FILE_ACK = 43,
        FILE_END = 44, FILE_COMPLETE = 45,
        FILE_ERROR_UDP = 46, FILE_CANCEL_UDP = 47
    };

    struct Packet {
        Type type;
        std::uint32_t sequence;
        std::vector<std::uint8_t> payload;
    };

    bool encodePacket(const Packet& packet,
                      std::vector<std::uint8_t>& output);

    bool decodeDatagram(const std::uint8_t* data,
                        std::size_t length,
                        Packet& output);

    bool sendTcpPacket(SocketHandle socket, const Packet& packet);
    bool receiveTcpPacket(SocketHandle socket, Packet& packet);
}
```

`SocketHandle` is defined by `platform/socket_compat.h`; do not expose the Windows-only `SOCKET` type in shared module interfaces. This is an interface guide. The final header must compile consistently in Client and Server. Do not put file-system operations or UI logic inside the Protocol module.

### 4.2 Server module

Server responsibilities:
1. Initialize the platform socket runtime once through `initializeSocketRuntime()` (a Winsock startup call on Windows and a no-op on macOS/Linux).
2. Create and bind the TCP listener on `0.0.0.0:54000`.
3. Create and bind the UDP socket on `0.0.0.0:54001`.
4. Accept TCP connections without blocking the UDP service.
5. Create one session handler per accepted TCP connection (or use a tested worker pool).
6. Require `AUTH` as the first application packet.
7. Reject empty/invalid/duplicate usernames.
8. Maintain username → session mapping and session → socket mapping.
9. Send `AUTH_OK` and `SESSION_INFO` after login.
10. Register UDP endpoint only after validating `UDP_REGISTER`.
11. Process heartbeat and expire stale UDP endpoint registrations.
12. Route global and private messages.
13. Relay file-transfer datagrams only for authorized active transfers.
14. Remove sessions and update `USER_LIST` on disconnect.
15. Shut down sockets and worker threads cleanly, then call `cleanupSocketRuntime()` after all networking has stopped.

Suggested server state:

```cpp
struct ClientSession {
    std::string username;
    SocketHandle tcpSocket;
    std::array<std::uint8_t, 16> sessionId;
    std::array<std::uint8_t, 32> udpToken;
    sockaddr_in udpEndpoint;
    bool udpRegistered;
    std::uint64_t lastHeartbeatMs;
    std::mutex sendMutex;
};
```

Protect shared maps and user-list revision with a mutex. Do not hold the global session mutex while calling blocking `send()`, `sendto()`, or waiting on a thread. Copy the required session/socket data under lock, release the lock, then perform I/O. Use a per-session send mutex so multiple Server threads do not interleave TCP packets on one socket.

### 4.3 Client Agent module

Client Agent responsibilities:
- Initialize the platform socket runtime once through `initializeSocketRuntime()` before opening TCP/UDP sockets.
- Connect to the configured Server IPv4 address and TCP port.
- Send `AUTH` immediately after TCP connection.
- Process `AUTH_OK`, `AUTH_FAIL`, and `SESSION_INFO`.
- Start UDP endpoint registration using the received token.
- Send periodic heartbeat datagrams.
- Maintain separate TCP and UDP receive loops.
- Provide thread-safe events/state to the UI bridge.
- Send chat and file-control messages over TCP.
- Send/receive file chunks and ACKs over UDP.
- Reconnect only according to a documented policy; do not silently reuse an expired token.

Suggested threads:
- UI/HTTP thread: handles browser requests and returns current application state.
- TCP receive thread: reads complete TCP packets and dispatches events.
- UDP receive thread: receives and validates one datagram at a time.
- File worker: reads, hashes, writes, and verifies files without blocking UI.
- Optional timer/maintenance worker: heartbeat and retransmission timeout processing.

Do not perform disk I/O, hashing, or long socket waits in the browser request handler.

### 4.4 User Manager

`UserManager` owns:
- Online username uniqueness.
- Session insertion/removal.
- User-list revision.
- Snapshot generation.
- Lookup of recipient sessions.
- Session token validation and UDP endpoint association.

The user list is a snapshot, not a stream of individual add/remove operations. Whenever membership changes, increment `revision` and broadcast the full current list to authenticated users.

### 4.5 UDP service and file relay

`UdpService` owns the Server UDP socket and dispatches only valid UDP message types. It must:
- Validate datagram size and common header.
- Validate the session token for registration.
- Bind the observed source IP/port to the authenticated session.
- Update heartbeat time.
- Route discovery replies.
- Forward file packets only when `transfer_id`, sender, recipient, and transfer state match the Server's active transfer table.
- Never trust the username or recipient embedded in an unregistered UDP packet.
- Drop malformed, unauthorized, wrong-session, or wrong-transfer datagrams.

The Server relay does not rewrite file chunk bytes. It validates the outer packet and transfer authorization, then forwards the complete datagram to the registered receiver endpoint. The receiver's ACK follows the reverse route.

### 4.6 Browser UI and local HTTP bridge

The assignment asks for an HTTP-based UI with client-side JavaScript and prohibits high-level HTTP frameworks and WebSockets. The chosen implementation is:
- C++ Client Agent opens a local TCP listener on `127.0.0.1:8080`.
- It serves static `index.html`, `style.css`, and `app.js`.
- It exposes a small set of HTTP endpoints for login, chat send, private message, online users, message history, file selection/transfer status, and cancellation.
- Browser JavaScript uses `fetch()` to call those local endpoints.
- The HTTP bridge forwards commands to the Client Agent's internal application layer; it does not open the Server TCP/UDP sockets itself.
- Use periodic polling (for example every 500 ms) for message/user/transfer updates. Do not use WebSockets.

Bind only to `127.0.0.1` by default. Do not expose the local UI bridge to the LAN. Validate HTTP method, path, content length, and JSON/body fields. Restrict file selection to a user-selected local file path through a safe native file-picker or a controlled upload workflow; never let arbitrary browser input request arbitrary local file paths.

The browser cannot directly use raw TCP/UDP sockets. The local C++ Agent is therefore the network bridge between the browser and Server.

## 5. Threading and concurrency model

### 5.1 Server

```text
Main
 ├── TCP accept loop
 │    ├── Client session worker A
 │    ├── Client session worker B
 │    └── Client session worker ...
 └── UDP receive loop
      └── validate + dispatch + relay datagrams
```

The Server must support at least 10–20 concurrent clients as required by the assignment. A thread-per-client design is acceptable for the initial 10–20-client target if cleanup and synchronization are correct. Do not create an unbounded number of threads without limits.

### 5.2 Client

```text
Client Agent
 ├── HTTP/UI bridge thread
 ├── TCP receive thread
 ├── UDP receive thread
 ├── heartbeat/retransmission maintenance
 └── file worker(s)
```

Shared state must be protected. Prefer passing immutable event objects from network threads to the UI state layer. Avoid holding a mutex while calling the browser bridge or blocking socket operations.

## 6. Security configuration

The Server and every Client Agent require the same 256-bit pre-shared key. Generate it once on a trusted machine:

```bash
openssl rand -hex 32
```

Set the resulting 64-character hexadecimal value in `DCAP_KEY_HEX` in each process environment. PowerShell:

```powershell
$env:DCAP_KEY_HEX = "<paste-the-generated-64-hex-character-key>"
```

macOS/Linux:

```bash
export DCAP_KEY_HEX="<paste-the-generated-64-hex-character-key>"
```

Set it in the terminal that starts the Server and separately in each Client Agent terminal. Do not commit the key or expose it to the browser. If it is missing or invalid, startup/connection fails; the program never falls back to plaintext.

## 7. Build and run — Windows, macOS, and Linux

The same C++17 source code must build and run on all three target operating-system families. Keep OS-specific socket calls inside `platform/socket_compat.h` and `platform/socket_compat.cpp`; the Server, Client, and Protocol modules must use the shared `SocketHandle` type and portable wrapper functions instead of calling Winsock-only functions directly.

### 6.1 Prerequisites

- **Windows:** MinGW-w64 GCC (recommended for the commands below) or MSVC, with C++17 support. Winsock2 is provided by Windows.
- **macOS:** Xcode Command Line Tools, which provide Clang and the system BSD socket API.
- **Linux:** GCC or Clang with C++17 support and the system POSIX socket API.
- VS Code or another editor is optional; it is not a runtime requirement.
- OpenSSL is required only when the implementation uses its SHA-256 API or authenticated encryption. Install the development package for the selected OS and use the same build configuration for all modules.

Check the compiler available on the machine:

```text
g++ --version
clang++ --version
```

### 6.2 Cross-platform socket compatibility layer

Add `platform/socket_compat.h` and `platform/socket_compat.cpp` as the only place that handles operating-system differences. The compatibility layer must provide at least:

- `SocketHandle`: `SOCKET` on Windows and `int` on macOS/Linux, plus a portable invalid-socket value and validity check.
- `SocketLength`: the appropriate socket-address length type for the host OS.
- `initializeSocketRuntime()` / `cleanupSocketRuntime()`: call `WSAStartup()` / `WSACleanup()` on Windows; perform no startup/cleanup work on macOS/Linux.
- `closeSocket()` and `shutdownSocket()`: map to the correct close/shutdown calls and constants on each OS.
- Portable wrappers for socket error retrieval, socket options, and TCP/UDP send/receive operations where function signatures or error handling differ.

Include the compatibility header wherever socket types or socket APIs are used. Do not put `SOCKET`, `WSAStartup`, `closesocket`, `WSAGetLastError`, or `-lws2_32` in shared Client, Server, or Protocol code. Keep calls whose signatures or constants differ (`setsockopt`, `send`/`recv`, `sendto`/`recvfrom`, `shutdown`, and `select`/polling) behind the same abstraction. On POSIX systems, account for `errno`, `socklen_t`, and the possibility of `SIGPIPE` when sending on a closed TCP connection.

### 6.3 Build Server

Run the relevant command from the repository root. The Windows example uses PowerShell line continuation; the macOS and Linux examples use shell line continuation.

**Windows (MinGW-w64, PowerShell):**

```powershell
g++ -std=c++17 -Wall -Wextra -pthread `
  server/main.cpp server/server.cpp server/user_manager.cpp `
  server/tcp_session.cpp server/udp_service.cpp server/file_relay.cpp `
  protocol/packet.cpp protocol/protocol.cpp `
  platform/socket_compat.cpp `
  -o server.exe -lws2_32
```

**macOS (Clang):**

```bash
clang++ -std=c++17 -Wall -Wextra -pthread \
  server/main.cpp server/server.cpp server/user_manager.cpp \
  server/tcp_session.cpp server/udp_service.cpp server/file_relay.cpp \
  protocol/packet.cpp protocol/protocol.cpp \
  platform/socket_compat.cpp \
  -o server
```

**Linux (GCC):**

```bash
g++ -std=c++17 -Wall -Wextra -pthread \
  server/main.cpp server/server.cpp server/user_manager.cpp \
  server/tcp_session.cpp server/udp_service.cpp server/file_relay.cpp \
  protocol/packet.cpp protocol/protocol.cpp \
  platform/socket_compat.cpp \
  -o server
```

### 6.4 Build Client

**Windows (MinGW-w64, PowerShell):**

```powershell
g++ -std=c++17 -Wall -Wextra -pthread `
  client/main.cpp client/client.cpp client/tcp_client.cpp `
  client/udp_client.cpp client/file_sender.cpp client/file_receiver.cpp `
  ui_bridge/http_server.cpp `
  protocol/packet.cpp protocol/protocol.cpp `
  platform/socket_compat.cpp `
  -o client.exe -lws2_32
```

**macOS (Clang):**

```bash
clang++ -std=c++17 -Wall -Wextra -pthread \
  client/main.cpp client/client.cpp client/tcp_client.cpp \
  client/udp_client.cpp client/file_sender.cpp client/file_receiver.cpp \
  ui_bridge/http_server.cpp \
  protocol/packet.cpp protocol/protocol.cpp \
  platform/socket_compat.cpp \
  -o client
```

**Linux (GCC):**

```bash
g++ -std=c++17 -Wall -Wextra -pthread \
  client/main.cpp client/client.cpp client/tcp_client.cpp \
  client/udp_client.cpp client/file_sender.cpp client/file_receiver.cpp \
  ui_bridge/http_server.cpp \
  protocol/packet.cpp protocol/protocol.cpp \
  platform/socket_compat.cpp \
  -o client
```

If SHA-256 uses OpenSSL, add the installed include/library flags. On systems that provide `pkg-config`, `pkg-config --cflags --libs openssl` can help supply those flags; the exact installation path may differ by OS and package manager. Do not copy another machine's OpenSSL paths blindly. The Windows commands link Winsock2 with `-lws2_32`; macOS and Linux use their system socket libraries and do not use that flag.

### 6.5 Run with localhost and LAN addressing

Start the Server first. On Windows:

```powershell
.\server.exe
```

On macOS/Linux:

```bash
./server
```

Start each Client Agent in a separate terminal. On Windows:

```powershell
.\client.exe 127.0.0.1 54000
```

On macOS/Linux:

```bash
./client 127.0.0.1 54000
```

For same-machine testing, connect to `127.0.0.1` (or `localhost`). For LAN testing, replace it with the Server computer's LAN IPv4 address. The Server listens on `0.0.0.0` for the application ports; the local HTTP bridge remains bound to `127.0.0.1` by default. Open the browser at `http://127.0.0.1:8080` on the same machine as that Client Agent.

Allow inbound TCP port `54000` and UDP port `54001` through the host firewall for LAN tests. Do not expose the UI bridge on a public or LAN interface by default. If port `8080` is already in use, report the error and allow a configured local UI port; do not silently bind to an arbitrary external interface.

## 8. Required end-to-end interaction sequence

1. Start Server; it binds TCP `54000` and UDP `54001`.
2. Start Client Agent; it starts local HTTP UI bridge.
3. Browser loads the local UI.
4. User submits username.
5. Client Agent opens TCP connection and sends `AUTH`.
6. Server validates username and returns `AUTH_OK`, `SESSION_INFO`, and `USER_LIST`.
7. Client registers its UDP endpoint with `UDP_REGISTER`.
8. Server returns `UDP_REGISTER_OK`.
9. Client sends heartbeat every 5 seconds.
10. Client sends global/private messages over TCP.
11. Server routes messages and returns ACK/ERROR.
12. For a file, sender sends `FILE_OFFER` over TCP.
13. Receiver accepts/rejects using `FILE_DECISION`.
14. If accepted, clients exchange `FILE_START`, `FILE_CHUNK`, `FILE_ACK`, `FILE_END`, and `FILE_COMPLETE` over UDP through Server relay.
15. Server sends `FILE_RESULT` over TCP to both participants.
16. On logout/disconnect, Server invalidates session and UDP mapping and broadcasts updated `USER_LIST`.

## 9. Git workflow and integration rules

1. Keep `main` stable.
2. Use `dev` as the integration branch.
3. Each member works on a feature branch:
   - `feature/server`
   - `feature/client-ui`
   - `feature/protocol-file-transfer`
4. Commit small, buildable changes.
5. Open Pull Requests into `dev`.
6. Every PR includes build command, run command, test procedure, and known limitations.
7. Merge `dev` into `main` only after integrated tests pass.

Suggested commits:
- `feat(server): add TCP session manager`
- `feat(client): add login and chat UI`
- `feat(protocol): implement packet codec`
- `feat(network): add UDP registration and heartbeat`
- `feat(file-transfer): add chunking and selective retransmission`
- `fix(server): clean up disconnected sessions`
- `docs: update protocol and integration guide`

### 8.1 Change-control rule

A wire-protocol change is not a local implementation detail. Before merging it:
1. Update `protocol-spec.md` and, if applicable, `file-transfer-spec.md`.
2. State the compatibility impact.
3. Update both sender and receiver code.
4. Add or update a serialization test.
5. Obtain review from the owners of the affected modules.
