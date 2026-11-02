# PROTOCOL SPECIFICATION — Distributed and Streaming-Based Client-Server Collaboration Application

- Protocol version: 1
- Related documents: `integration-guide.md`, `file-transfer-spec.md`
- Target implementation: C++17, raw sockets, IPv4
- Text encoding: UTF-8
- Integer byte order: big-endian (network byte order)

This document is the authoritative wire-protocol contract. Client, Server, and UI bridge implementations MUST follow the field order, sizes, message types, transport assignments, and validation rules below. `file-transfer-spec.md` defines the detailed file-transfer state machine and reliability algorithm; it MUST NOT contradict this document.

---

## 1. Scope and architecture

The application uses a hybrid transport architecture:

| Function                                                                                       | Transport         | Default endpoint             | Reason                                                                                                |
| ---------------------------------------------------------------------------------------------- | ----------------- | ---------------------------- | ----------------------------------------------------------------------------------------------------- |
| Initial connection, authentication, chat, private messages, online list, file-transfer control | TCP               | Server `54000`               | Reliable ordered control and messaging                                                                |
| Heartbeat, UDP endpoint registration, peer-presence discovery                                  | UDP               | Server `54001`               | Lightweight periodic datagrams                                                                        |
| File data, file ACKs, retransmissions                                                          | UDP               | Server `54001`               | Explicitly satisfies the UDP file-transfer milestone; reliability is implemented at application layer |
| Browser UI ↔ local Client Agent                                                                | HTTP/1.1 over TCP | Local Agent `127.0.0.1:8080` | Browser JavaScript cannot open arbitrary raw TCP/UDP sockets                                          |

All network traffic between application peers and the Server uses raw sockets. The HTTP listener is a small HTTP/1.1 implementation over raw TCP sockets; frameworks, WebSockets, Flask, and Django are not used.

### 1.1 Design principles

1. TCP and UDP are separate transports with separate receive loops.
2. TCP is a byte stream. A packet may arrive over multiple `recv()` calls or multiple packets may arrive in one call.
3. UDP preserves datagram boundaries but does not guarantee delivery, ordering, or duplicate suppression.
4. Every multi-byte integer is serialized explicitly in big-endian order.
5. Never transmit a native C/C++ struct directly.
6. Validate packet size, type, version, session, and payload fields before using them.
7. File bytes are binary data. Never treat file chunks as UTF-8 strings.
8. `Sequence` identifies packets in one sender direction and transport session; `transfer_id` and `chunk_index` identify file-transfer work.

## 2. Network constants

| Constant                         |                   Value |
| -------------------------------- | ----------------------: |
| Address family                   |                    IPv4 |
| TCP server port                  |                 `54000` |
| UDP server port                  |                 `54001` |
| Local HTTP UI port               |                  `8080` |
| Protocol version                 |                     `1` |
| Magic bytes                      |            ASCII `DCAP` |
| Magic integer                    |            `0x44434150` |
| Header size                      |                14 bytes |
| Maximum TCP payload              | 1,048,576 bytes (1 MiB) |
| Maximum UDP application datagram |             1,200 bytes |
| Default heartbeat interval       |               5 seconds |
| Heartbeat timeout                |              15 seconds |
| Maximum username length          |          32 UTF-8 bytes |
| Maximum chat message length      |       4,096 UTF-8 bytes |

The Server listens on `0.0.0.0` for TCP and UDP. A local Client connects to `127.0.0.1`; a LAN Client connects to the Server computer's LAN IPv4 address. Firewall rules must permit TCP `54000` and UDP `54001`. Port `8080` is local to the Client Agent by default.

## 3. Common packet header

Every TCP application packet and every UDP application datagram begins with this 14-byte header.

| Offset | Field          |     Size | Type     | Rule                              |
| -----: | -------------- | -------: | -------- | --------------------------------- |
|      0 | Magic          |        4 | `uint32` | `0x44434150`, bytes `44 43 41 50` |
|      4 | Version        |        1 | `uint8`  | `1`                               |
|      5 | Type           |        1 | `uint8`  | Message type from Section 4       |
|      6 | Sequence       |        4 | `uint32` | Big-endian packet sequence        |
|     10 | Payload Length |        4 | `uint32` | Number of payload bytes           |
|     14 | Payload        | variable | bytes    | Exactly `Payload Length` bytes    |

### 3.1 Serialization rules

- `uint8`, `uint16`, `uint32`, `uint64`: unsigned integers of the stated width.
- All multi-byte integers use big-endian byte order.
- `byte[N]`: exactly N raw bytes.
- `string`: `uint16 byte_length` followed by exactly that many UTF-8 bytes.
- Strings are length-prefixed; they are not null-terminated.
- Payload length counts bytes, not Unicode characters.
- Reject an invalid UTF-8 text field where a text field is required.
- Reject a packet if its actual payload length differs from the declared length.
- Never cast an incoming byte buffer to a packed struct and assume alignment.

### 3.2 TCP framing

TCP has no message boundaries. The receiver MUST:

1. Read exactly 14 bytes for the header, looping until complete.
2. Validate Magic, Version, Type, and Payload Length.
3. Reject a payload length greater than 1 MiB before allocating memory.
4. Read exactly the declared payload length, looping until complete.
5. Dispatch one complete packet.
6. Continue reading the next packet.

A `recv()` returning 0 means the peer closed the connection. A socket error requires checking the platform socket error code. A send operation MUST loop until all packet bytes are written or an unrecoverable error occurs.

### 3.3 UDP framing

One UDP datagram contains exactly one complete application packet. Use `recvfrom()` and `sendto()`, not the TCP stream framing loop. Reject datagrams shorter than 14 bytes, longer than 1,200 bytes, truncated datagrams, or datagrams whose declared payload length does not match the received length minus 14.

## 4. Message type registry

Type values are global and MUST NOT be reused for a different meaning.

### 4.1 TCP message types

| Value | Name            | Direction                             | Payload purpose                             |
| ----: | --------------- | ------------------------------------- | ------------------------------------------- |
|     1 | `AUTH`          | Client → Server                       | Login request                               |
|     2 | `AUTH_OK`       | Server → Client                       | Login accepted; returns session information |
|     3 | `AUTH_FAIL`     | Server → Client                       | Login rejected                              |
|     4 | `MSG`           | Client ↔ Server                       | Global chat message                         |
|     5 | `PRIVATE_MSG`   | Client ↔ Server                       | Private chat message                        |
|     6 | `USER_LIST`     | Server → authenticated Clients        | Current online-user snapshot                |
|     7 | `ACK`           | Server → Client                       | Application-level command acknowledgement   |
|     8 | `ERROR`         | Server → Client                       | Error response                              |
|     9 | `LOGOUT`        | Client → Server                       | Graceful logout                             |
|    10 | `PING`          | Client ↔ Server                       | TCP application keepalive / RTT probe       |
|    11 | `PONG`          | Client ↔ Server                       | Reply to `PING`                             |
|    12 | `FILE_OFFER`    | Sender → Server → Receiver            | File-transfer invitation and metadata       |
|    13 | `FILE_DECISION` | Receiver → Server → Sender            | Accept or reject invitation                 |
|    14 | `FILE_CANCEL`   | Either Client → Server → other Client | Cancel transfer                             |
|    15 | `FILE_RESULT`   | Server → involved Clients             | Final transfer result                       |
|    16 | `SESSION_INFO`  | Server → Client                       | Session/UDP registration token              |
|    17 | `SERVER_NOTICE` | Server → authenticated Clients        | Server announcement                         |

### 4.2 UDP message types

| Value | Name              | Direction                            | Payload purpose                           |
| ----: | ----------------- | ------------------------------------ | ----------------------------------------- |
|    32 | `UDP_REGISTER`    | Client → Server                      | Bind observed UDP endpoint to TCP session |
|    33 | `UDP_REGISTER_OK` | Server → Client                      | Registration accepted                     |
|    34 | `HEARTBEAT`       | Client → Server                      | Presence/liveness update                  |
|    35 | `HEARTBEAT_ACK`   | Server → Client                      | Heartbeat response                        |
|    36 | `DISCOVERY`       | Client → Server                      | Request current peer presence             |
|    37 | `DISCOVERY_REPLY` | Server → Client                      | Presence result                           |
|    40 | `FILE_START`      | Sender → Server → Receiver           | Start transfer over UDP                   |
|    41 | `FILE_ACCEPT_UDP` | Receiver → Server → Sender           | Confirm readiness to receive              |
|    42 | `FILE_CHUNK`      | Sender → Server → Receiver           | File data chunk                           |
|    43 | `FILE_ACK`        | Receiver → Server → Sender           | Cumulative/selective chunk ACK            |
|    44 | `FILE_END`        | Sender → Server → Receiver           | Sender has submitted all chunks           |
|    45 | `FILE_COMPLETE`   | Receiver → Server → Sender           | Reassembly and integrity result           |
|    46 | `FILE_ERROR_UDP`  | Transfer participant → Server → peer | Transfer-specific error                   |
|    47 | `FILE_CANCEL_UDP` | Transfer participant → Server → peer | Cancel active UDP transfer                |

A packet received over the wrong transport is invalid. For example, a `FILE_CHUNK` on TCP or an `AUTH` on UDP MUST be rejected.

## 5. Payload layouts

Fields below appear in the listed order. Every string uses the length-prefixed UTF-8 convention from Section 3.1 unless a fixed byte array is specified.

### 5.1 Authentication and session

**`AUTH`**
| Field | Type | Meaning |
|---|---|---|
| `username` | string | 1–32 UTF-8 bytes; no LF or control characters |

Version 1 uses username-based session registration for the course prototype. It is not password-based account security. The Server rejects an empty username, duplicate online username, invalid UTF-8, or a username outside the stated length.

**`AUTH_OK`**
| Field | Type | Meaning |
|---|---|---|
| `username` | string | Canonical accepted username |
| `session_id` | `byte[16]` | Random session identifier |
| `server_time_ms` | `uint64` | Server Unix time in milliseconds |

**`AUTH_FAIL`**
| Field | Type | Meaning |
|---|---|---|
| `error_code` | `uint16` | Code from Section 8 |
| `reason` | string | Human-readable reason |

**`SESSION_INFO`**
| Field | Type | Meaning |
|---|---|---|
| `session_token` | `byte[32]` | Cryptographically random token for UDP registration |
| `udp_port` | `uint16` | Server UDP port, normally 54001 |
| `heartbeat_interval_ms` | `uint32` | Normally 5000 |

The token is delivered only over the authenticated TCP connection. Do not send it in chat messages or expose it to the browser UI.

**`UDP_REGISTER`**
| Field | Type | Meaning |
|---|---|---|
| `session_token` | `byte[32]` | Token received in `SESSION_INFO` |

The Server binds the observed UDP source IP address and port to the TCP session associated with this token. The client must not provide a username as proof of identity.

**`UDP_REGISTER_OK`**
| Field | Type | Meaning |
|---|---|---|
| `status` | `uint8` | `1 = registered`, `0 = rejected` |
| `reason_code` | `uint16` | 0 on success; otherwise error code |

**`HEARTBEAT` / `HEARTBEAT_ACK`**
| Field | Type | Meaning |
|---|---|---|
| `nonce` | `uint64` | Random/incrementing client nonce copied into ACK |
| `client_time_ms` | `uint64` | Client timestamp in milliseconds |

`HEARTBEAT_ACK` additionally contains `server_time_ms` (`uint64`). The Server updates the session's last-heartbeat timestamp only after validating the registered source endpoint.

**`DISCOVERY`** has an empty payload and requests the current online-user snapshot. `DISCOVERY_REPLY` uses the same payload layout as `USER_LIST`.

### 5.2 Chat

**Client → Server `MSG`**
| Field | Type | Meaning |
|---|---|---|
| `client_message_id` | `uint32` | Client-generated ID, increasing per TCP session |
| `message` | string | 1–4096 UTF-8 bytes |

**Server → Client `MSG`**
| Field | Type | Meaning |
|---|---|---|
| `server_message_id` | `uint32` | Server-generated increasing ID |
| `sender` | string | Sender username |
| `server_time_ms` | `uint64` | Server timestamp |
| `message` | string | Original message |

The Server broadcasts a global message to every authenticated session, including the sender.

**Client → Server `PRIVATE_MSG`**
| Field | Type | Meaning |
|---|---|---|
| `client_message_id` | `uint32` | Client-generated ID |
| `recipient` | string | Destination username |
| `message` | string | 1–4096 UTF-8 bytes |

**Server → Client `PRIVATE_MSG`**
| Field | Type | Meaning |
|---|---|---|
| `server_message_id` | `uint32` | Server-generated ID |
| `sender` | string | Sender username |
| `recipient` | string | Intended recipient |
| `server_time_ms` | `uint64` | Server timestamp |
| `message` | string | Original message |

The Server forwards a private message only to the named authenticated recipient. It returns `ACK` to the sender after successful routing; otherwise it returns `ERROR`.

**`USER_LIST`**
| Field | Type | Meaning |
|---|---|---|
| `revision` | `uint32` | Increases whenever online membership changes |
| `user_count` | `uint16` | Number of online users |
| `usernames` | repeated string | Exactly `user_count` usernames, sorted lexicographically |

Do not serialize the list as a newline-delimited string; repeated length-prefixed strings avoid delimiter ambiguity.

### 5.3 Generic responses and lifecycle

**`ACK`**
| Field | Type | Meaning |
|---|---|---|
| `acknowledged_type` | `uint8` | Type of request acknowledged |
| `request_sequence` | `uint32` | Sequence of original request |
| `status` | `uint8` | `0 = accepted`, `1 = processed`, `2 = queued` |
| `detail` | string | Optional short explanation |

**`ERROR`**
| Field | Type | Meaning |
|---|---|---|
| `error_code` | `uint16` | Stable numeric code |
| `related_type` | `uint8` | Request type, or 0 if not applicable |
| `related_sequence` | `uint32` | Request sequence, or 0 |
| `detail` | string | Human-readable description |

**`LOGOUT`** has an empty payload. The Server closes the session after responding with `ACK` if needed.

**`PING` / `PONG`**
| Field | Type | Meaning |
|---|---|---|
| `nonce` | `uint64` | Value copied unchanged into the reply |
| `client_time_ms` | `uint64` | Client timestamp |

`SERVER_NOTICE` contains `severity` (`uint8`, 0 info / 1 warning / 2 error), followed by a string.

### 5.4 File control over TCP

The TCP control messages announce a transfer and negotiate receiver acceptance. File bytes do not travel over TCP.

**`FILE_OFFER`**
| Field | Type | Meaning |
|---|---|---|
| `transfer_id` | `byte[16]` | Random unique transfer ID |
| `recipient` | string | Online recipient username; sender identity comes from the authenticated TCP session |
| `filename` | string | Base filename only, 1–255 UTF-8 bytes |
| `file_size` | `uint64` | Bytes; maximum 104,857,600 |
| `chunk_size` | `uint16` | MUST be 1100 |
| `total_chunks` | `uint32` | `ceil(file_size / 1100)` |
| `sha256` | `byte[32]` | Digest of complete source file |

**`FILE_DECISION`**
| Field | Type | Meaning |
|---|---|---|
| `transfer_id` | `byte[16]` | Offered transfer |
| `decision` | `uint8` | `0 = reject`, `1 = accept` |
| `reason_code` | `uint16` | 0 if accepted; error code if rejected |
| `reason` | string | Optional display text |

**`FILE_CANCEL`**
| Field | Type | Meaning |
|---|---|---|
| `transfer_id` | `byte[16]` | Transfer to cancel |
| `reason_code` | `uint16` | Cancellation reason |
| `reason` | string | Optional explanation |

**`FILE_RESULT`**
| Field | Type | Meaning |
|---|---|---|
| `transfer_id` | `byte[16]` | Completed/failed transfer |
| `status` | `uint8` | `0 = failed`, `1 = complete`, `2 = cancelled` |
| `error_code` | `uint16` | 0 on success |
| `bytes_transferred` | `uint64` | Final count |
| `detail` | string | User-facing result |

## 6. Sequence numbers and IDs

- Each TCP connection has independent send and receive sequence counters, starting at 1.
- Each UDP endpoint has independent send and receive sequence counters, starting at 1 for the current session.
- Sequence numbers increase monotonically and wrap modulo \(2^{32}\). Implement comparisons using serial-number arithmetic; do not compare wrapped values as ordinary signed integers.
- Retransmission of the same logical UDP packet MUST retain the same packet sequence number.
- A newly generated packet receives a new sequence number.
- `client_message_id` and `server_message_id` are application identifiers; they are not substitutes for the common packet `Sequence`.
- `transfer_id` identifies one file transfer and is a cryptographically random 16-byte value.
- `chunk_index` identifies one chunk within that transfer.

## 7. Required message flows

### 7.1 Login and UDP registration

```text
Client                         Server
  |--- TCP connect ------------->|
  |--- AUTH(username) ---------->|
  |<-- AUTH_OK(session_id) ------|
  |<-- SESSION_INFO(token, port)-|
  |--- UDP_REGISTER(token) ------>|
  |<-- UDP_REGISTER_OK -----------|
  |--- HEARTBEAT ---------------->|
  |<-- HEARTBEAT_ACK ------------|
  |<-- USER_LIST ----------------|
```

The Server MUST NOT accept chat or file control requests before successful TCP authentication. The Server MUST NOT associate a UDP endpoint with a user until the registration token is validated.

### 7.2 Global chat

```text
Client A                  Server                  Client B
   |--- MSG ---------------->|                         |
   |                         |--- MSG(A, text) ------->|
   |<-- ACK -----------------|                         |
   |<-- MSG(A, text) --------|                         |
```

### 7.3 Private chat

```text
Client A                  Server                  Client B
   |--- PRIVATE_MSG(B) ------>|                         |
   |                          |--- PRIVATE_MSG(A) ----->|
   |<-- ACK ------------------|                         |
```

### 7.4 File transfer

The detailed lifecycle is in `file-transfer-spec.md`. At a high level: TCP `FILE_OFFER` → TCP `FILE_DECISION` → UDP `FILE_START`/`FILE_ACCEPT_UDP` → UDP `FILE_CHUNK`/`FILE_ACK` → UDP `FILE_END` → UDP `FILE_COMPLETE` → TCP `FILE_RESULT`.

## 8. Error-code registry

| Code | Name                       | Meaning                                  |
| ---: | -------------------------- | ---------------------------------------- |
|  100 | `ERR_MALFORMED_PACKET`     | Invalid header, length, or field layout  |
|  101 | `ERR_UNSUPPORTED_VERSION`  | Protocol version unsupported             |
|  102 | `ERR_UNKNOWN_MESSAGE_TYPE` | Type not registered                      |
|  103 | `ERR_NOT_AUTHENTICATED`    | Request before successful login          |
|  104 | `ERR_INVALID_USERNAME`     | Username invalid                         |
|  105 | `ERR_USERNAME_IN_USE`      | Duplicate online username                |
|  106 | `ERR_USER_NOT_FOUND`       | Recipient is not online                  |
|  107 | `ERR_MESSAGE_TOO_LARGE`    | Chat message exceeds limit               |
|  108 | `ERR_RATE_LIMITED`         | Request rate exceeded                    |
|  109 | `ERR_INVALID_SESSION`      | Session/token invalid or expired         |
|  110 | `ERR_WRONG_TRANSPORT`      | Message type received on wrong transport |
|  200 | `ERR_TRANSFER_NOT_FOUND`   | Unknown or expired transfer              |
|  201 | `ERR_TRANSFER_REJECTED`    | Receiver rejected transfer               |
|  202 | `ERR_FILE_TOO_LARGE`       | File exceeds configured limit            |
|  203 | `ERR_INVALID_FILENAME`     | Unsafe or invalid filename               |
|  204 | `ERR_INVALID_CHUNK`        | Invalid chunk index/length               |
|  205 | `ERR_TRANSFER_TIMEOUT`     | Retry/time limit exceeded                |
|  206 | `ERR_INTEGRITY_FAILED`     | SHA-256 mismatch                         |
|  207 | `ERR_DISK_SPACE`           | Insufficient disk space                  |
|  208 | `ERR_TRANSFER_CANCELLED`   | Transfer cancelled                       |
|  209 | `ERR_TRANSFER_BUSY`        | Receiver/server capacity reached         |

## 9. Validation and connection lifecycle

- First TCP application packet MUST be `AUTH`.
- Duplicate online usernames are rejected.
- Unauthenticated sessions may not send chat, file, or discovery requests.
- Every TCP packet and UDP datagram is validated independently.
- Invalid Magic, unsupported Version, impossible lengths, unknown mandatory type, or wrong transport causes packet rejection; repeated malformed TCP packets cause connection closure.
- The Server removes a user after TCP disconnect or heartbeat expiry policy. TCP disconnect is authoritative; heartbeat expiry is a fallback for half-open or suspended clients.
- On disconnect, invalidate session token and UDP endpoint mapping, update `USER_LIST`, and notify peers involved in active transfers.
- Do not hold a global user-list mutex while performing blocking socket sends. Copy destination sessions under lock, release the lock, then send.

## 10. Security baseline

Version 1 is a course-project protocol, not a production security protocol.

### 10.1 Authenticated-encryption profile

The integrated implementation applies AES-256-GCM to every TCP packet and UDP datagram:

- The common 14-byte DCAP header remains visible for framing and message-type dispatch.
- The complete header is authenticated as GCM additional authenticated data (AAD); header tampering causes authentication failure.
- The wire payload is `nonce[12] | ciphertext[N] | tag[16]`.
- OpenSSL `RAND_bytes()` generates a fresh 96-bit nonce for every encrypted packet.
- The 32-byte pre-shared key is loaded from `DCAP_KEY_HEX`, exactly 64 hexadecimal characters. The Server and all Client Agents must use the same value.
- The Server authenticates/decrypts inbound UDP packets before session and transfer authorization, then encrypts the forwarded packet for its destination.
- Missing or malformed keys stop Server startup or Client connection. There is no plaintext fallback.

The 14-byte DCAP header and message type registry remain unchanged, but the payload wire representation changes. Plaintext DCAP peers are incompatible with this profile.

This shared-key profile is for a controlled course demonstration. It does not provide per-client key isolation, password-based identity, secure key exchange, key rotation, forward secrecy, or replay protection. A captured valid packet could be replayed unless per-session replay windows are added. Use TLS/DTLS or a reviewed key-establishment design before production use.

- Username-only login identifies an online nickname; it does not prove a real-world identity.
- UDP registration uses a 32-byte cryptographically random session token delivered over the authenticated TCP connection.
- Tokens are scoped to a session and invalidated at logout/disconnect.
- Validate every length and index before allocation or file I/O.
- Restrict file paths to a receiver-controlled directory.
- Use SHA-256 for file-integrity verification; SHA-256 alone does not authenticate the sender.
- The implementation MUST use a standard cryptographic library if payload encryption is implemented. Do not invent encryption or key exchange.
- The AES-256-GCM transport integration is implemented in this branch; its build, automated test, and end-to-end runtime results remain unverified until the project test suite is run.
