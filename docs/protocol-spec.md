# PROTOCOL SPECIFICATION — Distributed Chat Application

- Document version: 1.0
- Protocol version: 1
- Transport: TCP/IPv4 for chat
- Text encoding: UTF-8
- Header size: 14 bytes
- Maximum payload: 1 MiB

## 1. Scope
This is the shared application-layer contract for Client, Server, and Protocol modules. All members must use the same field sizes, byte order, message type values, and payload conventions. Version 1 covers authentication, global chat, private chat, online-user listing, ACK, and ERROR. File-transfer packet types are not yet defined.

## 2. Connection rules
1. Server listens on TCP port `54000` by default.
2. Client connects to the Server's IPv4 address and port.
3. Each TCP connection represents one Client session.
4. Client must send `AUTH` as its first application packet.
5. Server closes the connection if authentication fails or the first packet is not `AUTH`.
6. TCP is a byte stream: one `send()`/`recv()` call is not guaranteed to transfer a complete packet. Implement send/receive loops.

## 3. Packet format
Every packet is a 14-byte header followed by exactly `Payload Length` bytes.

| Offset | Field | Size | Format |
|---:|---|---:|---|
| 0 | Magic | 4 bytes | Unsigned integer, network byte order |
| 4 | Version | 1 byte | Unsigned integer; value `1` |
| 5 | Type | 1 byte | Message type |
| 6 | Sequence | 4 bytes | Unsigned integer, network byte order |
| 10 | Payload Length | 4 bytes | Unsigned integer, network byte order |
| 14 | Payload | Variable | UTF-8 bytes |

Constants:
- Magic: `0x44434150` (ASCII bytes `DCAP`)
- Version: `1`
- Header size: `14` bytes
- Maximum payload: `1,048,576` bytes
- All multi-byte integers are big-endian.
- Payload length is measured in bytes, not characters. Do not include a C-string null terminator.

## 4. Message types

| Value | Name | Direction | Purpose |
|---:|---|---|---|
| 1 | `AUTH` | Client → Server | Login request |
| 2 | `AUTH_OK` | Server → Client | Login accepted |
| 3 | `AUTH_FAIL` | Server → Client | Login rejected |
| 4 | `MSG` | Client → Server; Server → Clients | Global chat |
| 5 | `PRIVATE_MSG` | Client → Server; Server → recipient | Private chat |
| 6 | `USER_LIST` | Server → authenticated Clients | Online usernames |
| 7 | `ACK` | Server → Client | Application-level acknowledgement |
| 8 | `ERROR` | Server → Client | Error response |

Unknown message types must not be silently reinterpreted.

## 5. Payload conventions
All text is UTF-8.

| Type | Payload |
|---|---|
| `AUTH` | Username only |
| `AUTH_OK` | Welcome text |
| `AUTH_FAIL` | Failure reason |
| `MSG` Client → Server | Message text only |
| `MSG` Server → Clients | `sender + LF + message` |
| `PRIVATE_MSG` Client → Server | `recipient + LF + message` |
| `PRIVATE_MSG` Server → recipient | `sender + LF + message` |
| `USER_LIST` | Usernames separated by LF (`\\n`); no required trailing LF |
| `ACK` | Acknowledgement text |
| `ERROR` | Error text |

`LF` is one newline byte (`0x0A`). Usernames must not contain LF. Split sender/recipient metadata at the first LF only; the rest belongs to the message.

Suggested validation: username 1–32 UTF-8 bytes, message 1–4096 UTF-8 bytes. The current starter Server only checks empty and duplicate usernames; the group should add consistent length validation.

## 6. Sequence field
`Sequence` is a 32-bit unsigned integer in network byte order. Each sender should increment its own sequence number, starting at 1, scoped to one TCP connection and direction. The receiver may log or validate it. The starter Server generates sequence values for server-originated packets; Client code must maintain its own counter.

`ACK` is an application-level response, not a TCP acknowledgement. In v1, ACK payload is explanatory text; it does not yet identify a specific sequence number. If explicit ACK-to-sequence mapping is required, agree on an updated spec first.

## 7. Message flows

### Login
```text
Client                                  Server
  | -------- AUTH(username) ----------> |
  | <------- AUTH_OK or AUTH_FAIL ----- |
  | <------- USER_LIST ---------------- |
```

### Global chat
```text
Client A                 Server                 Client B
   | -- MSG(message) --> |                         |
   |                    | -- MSG(A + LF + text) -->|
   | <----- ACK --------|                         |
```
Server broadcasts to all authenticated users, including the sender.

### Private chat
```text
Client A                 Server                 Client B
   | -- PRIVATE_MSG --> |                         |
   |  (B + LF + text)   | -- PRIVATE_MSG -------> |
   | <----- ACK --------|                         |
```
If the recipient is offline/unknown, sender receives `ERROR`.

After login or disconnect, Server broadcasts an updated `USER_LIST`.

## 8. Errors and disconnect
- `recv() == 0`: peer closed the connection.
- `SOCKET_ERROR`: inspect `WSAGetLastError()`.
- Invalid magic/version, malformed header, or excessive payload length: close the connection.
- Unauthenticated Clients may not send chat messages.
- On disconnect, remove the username and broadcast an updated user list.
- Failed sends should trigger safe session cleanup.

## 9. File-transfer extension — not defined in v1
The assignment wording about TCP/UDP file transfer needs clarification. Before implementation, the group must agree on transport, transfer ID, filename/size metadata, chunk index/offset, chunk size, integrity check (e.g. SHA-256), and ACK/retransmission rules if UDP is used. Allocate message types 9+ only after all members update this document together. Current Server starter does not implement file transfer.

## 10. Security limitations
This v1 username-only login is not password authentication and does not encrypt traffic. Use only test usernames/messages; do not send sensitive information. If adding encryption, use a standard cryptographic library and agree on key exchange, nonce/IV, authentication tag, and format. Do not invent custom cryptography.

## 11. Compatibility checklist
- Exact 14-byte header and offsets.
- Magic `DCAP`, version 1, big-endian 32-bit fields.
- Payload length in bytes.
- UTF-8 payload conventions as above.
- Partial TCP send/receive handled.
- No unilateral changes to message type values or payload layouts.
