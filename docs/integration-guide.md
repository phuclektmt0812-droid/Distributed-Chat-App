# INTEGRATION GUIDE — Distributed Chat Application

- Document version: 1.0
- Shared contract: `protocol-spec.md`
- Target development environment: Windows + VS Code + MinGW-w64 GCC

## 1. Team responsibilities

| Member | Responsibility | Deliverable |
|---|---|---|
| Member 1 | Server and user management | TCP listener, sessions, authentication, online list, broadcast, private routing, disconnect handling |
| Member 2 | Client and UI | TCP client, login, send/receive, chat UI, online list, private/global messaging |
| Member 3 | Protocol, file transfer, security support | Packet codec/spec, file-transfer design and implementation, integrity/reliability tests |

Member 3 maintains the protocol document, but all three members must follow it. No one changes packet fields or message type values alone.

## 2. Recommended repository layout
```text
Distributed-Chat-App/
├── client/                 # Member 2
├── server/                 # Member 1
├── protocol/               # Shared packet codec; Member 3 coordinates
├── web/                    # Member 2, if using web UI files
├── tests/
├── docs/
│   ├── protocol-spec.md
│   └── integration-guide.md
├── .gitignore
└── README.md
```
Keep public interfaces stable after another member starts using them.

## 3. Agreed network defaults

| Setting | Value |
|---|---|
| Transport for chat | TCP |
| Address family | IPv4 |
| Server port | `54000` |
| Protocol version | `1` |
| Magic | `DCAP` (`0x44434150`) |
| Header size | 14 bytes |
| Maximum payload | 1 MiB |
| Text encoding | UTF-8 |

Local Client connects to `127.0.0.1:54000`. For LAN testing, use the Server computer's LAN IPv4 address and allow the port through Windows Firewall if necessary.

## 4. Module interfaces

### 4.1 Protocol module — Member 3
The shared interface should expose:
```cpp
namespace protocol {
    enum class Type : std::uint8_t {
        AUTH = 1, AUTH_OK = 2, AUTH_FAIL = 3, MSG = 4,
        PRIVATE_MSG = 5, USER_LIST = 6, ACK = 7, ERROR = 8
    };

    struct Packet {
        Type type;
        std::uint32_t sequence;
        std::string payload;
    };

    bool sendPacket(SOCKET socket, const Packet& packet);
    bool receivePacket(SOCKET socket, Packet& packet);
}
```
The actual encoder/decoder must match `protocol-spec.md`. Client and Server must not invent separate packet layouts.

### 4.2 Server module — Member 1
- Accept TCP connections.
- Require `AUTH` first.
- Reject empty/duplicate usernames.
- Maintain username/session mapping.
- Handle `MSG` and `PRIVATE_MSG`.
- Broadcast `USER_LIST` after login/disconnect.
- Clean up disconnected sessions and sockets.
- Protect shared session state and per-socket writes with synchronization.

### 4.3 Client module — Member 2
- Connect to configured IPv4 address and port.
- Send `AUTH` immediately after connection.
- Wait for `AUTH_OK` or `AUTH_FAIL`.
- After successful login, handle `USER_LIST`, `MSG`, `PRIVATE_MSG`, `ACK`, `ERROR`.
- Send global `MSG` with message text only.
- Send private `PRIVATE_MSG` as `recipient + LF + message`.
- Receive packets without blocking the UI thread.
- Handle partial TCP reads using the shared decoder.

## 5. Build and run — Windows + MinGW-w64

Check compiler:
```bash
g++ --version
```

Build current Server starter from repository root:
```bash
g++ -std=c++17 -Wall -Wextra server/main.cpp server/user_manager.cpp protocol/protocol.cpp -o server.exe -lws2_32
```

Run:
```bash
./server.exe
```

Optional port:
```bash
./server.exe 54000
```

Include `<winsock2.h>` before `<windows.h>` if both are needed. Link Winsock with `-lws2_32`.

## 6. Required interaction sequence
1. Server starts and listens on TCP port 54000.
2. Client connects.
3. Client sends `AUTH` containing username.
4. Server replies `AUTH_OK` or `AUTH_FAIL`.
5. After successful login, Server sends/broadcasts `USER_LIST`.
6. Client sends `MSG` or `PRIVATE_MSG`.
7. Server routes the message and replies `ACK` or `ERROR`.
8. Server updates `USER_LIST` after disconnect.

Payload formats are defined only in `protocol-spec.md`.

## 7. Git workflow for low-communication teamwork
1. Repository owner creates `dev` once.
2. Each member branches from `dev`: `feature/server`, `feature/client`, `feature/protocol`.
3. Commit primarily files owned by that member.
4. Open Pull Requests into `dev`.
5. Include build/run/test instructions in each PR.
6. Merge `dev` into `main` only after integrated tests pass.

Suggested commit messages:
- `feat(server): add TCP connection handling`
- `feat(client): implement login and chat UI`
- `feat(protocol): add packet encoder and decoder`
- `fix(server): clean up disconnected sessions`
- `docs: update protocol specification`

## 8. Handoff checklist
Every member's Pull Request must state:
- Files changed and features completed.
- Build command and run command.
- Short test procedure and expected result.
- Known limitations or unfinished work.
- Any requested protocol change (must be agreed before merging).

Commit source code and documentation, not only screenshots or executable files.

## 9. Integration test plan

Run the Server, then launch at least three Clients.

| Test | Action | Expected result |
|---|---|---|
| T1 | Start Server | Listening message appears |
| T2 | One Client sends AUTH | Receives AUTH_OK |
| T3 | Second Client uses same username | Receives AUTH_FAIL |
| T4 | Multiple unique users log in | USER_LIST updates |
| T5 | Client A sends MSG | All authenticated Clients receive it |
| T6 | Client A sends PRIVATE_MSG to B | B receives it; A receives ACK |
| T7 | Client B disconnects | B removed; USER_LIST updates |
| T8 | Malformed/oversized packet | Connection rejected/closed safely |

## 10. Decisions still requiring group agreement
1. **File transfer transport:** TCP or UDP. Assignment wording appears inconsistent; ask instructor if possible.
2. **File-transfer packet layout:** type values, transfer ID, metadata, chunk index/offset, chunk size, integrity hash, ACK/retry rules.
3. **Encryption:** select a standard library and define key exchange and authenticated-encryption format.
4. **UI architecture:** define how HTML/JavaScript communicates with the C++ Client while respecting the raw-socket restrictions.
5. **Graceful server shutdown:** decide whether to add a console command or another controlled shutdown mechanism.

Until these decisions are agreed, complete the v1 chat features without inventing incompatible extensions.

## 11. Definition of done
- Server and Client compile in the agreed Windows/MinGW environment.
- At least three Clients authenticate concurrently.
- Duplicate usernames are rejected.
- Global and private messages route correctly.
- Online list updates after login/disconnect.
- A disconnected Client does not crash the Server.
- All modules follow the same protocol specification.
