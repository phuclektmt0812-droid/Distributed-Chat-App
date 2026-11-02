# FILE TRANSFER SPECIFICATION — Reliable UDP File Transfer

- Related documents: `protocol-spec.md`, `integration-guide.md`
- Data transport: UDP/IPv4 through the Server relay
- Control transport: TCP/IPv4
- Maximum file size: 100 MiB
- Maximum UDP application datagram: 1,200 bytes
- Integrity algorithm: SHA-256

This document defines the complete file-transfer behavior. The Server relays UDP datagrams; clients do not send file data directly to one another. Transfer invitation and final user-visible result use TCP. File chunks and chunk acknowledgements use UDP.

---

## 1. Goals and requirements

The subsystem MUST:
1. Send files between authenticated online Clients.
2. Support binary files of arbitrary format within the configured 100 MiB limit.
3. Split files into bounded UDP chunks.
4. Reassemble chunks correctly even when they arrive out of order.
5. Detect missing and duplicate chunks.
6. Selectively retransmit missing chunks.
7. Apply bounded sender window and receiver flow control.
8. Verify the reconstructed file using SHA-256.
9. Reject unsafe filenames, invalid metadata, oversized files, and unauthorized transfers.
10. Handle timeout, disconnect, cancellation, insufficient disk space, and malformed datagrams without crashing.
11. Report progress and final status to the UI.
12. Use only raw sockets for network transfer; no WebSockets or high-level networking framework.

## 2. Fixed design decisions

| Item | Fixed value |
|---|---|
| Control plane | TCP connection on port 54000 |
| File data plane | UDP through Server relay on port 54001 |
| Routing | Sender → Server → Receiver |
| UDP datagram limit | 1,200 bytes including 14-byte common header |
| Transfer ID | 16 cryptographically random bytes |
| Filename encoding | UTF-8 base filename, no path |
| Maximum filename | 255 UTF-8 bytes |
| Maximum file size | 100 MiB (104,857,600 bytes) |
| Chunk data size | 1,100 bytes |
| Initial send window | 8 chunks |
| Maximum send window | 32 chunks |
| Receiver advertised window | 32 chunks initially |
| ACK strategy | Cumulative ACK base + selective bitmap |
| Initial RTO | 500 ms |
| Minimum RTO | 200 ms |
| Maximum RTO | 4,000 ms |
| Maximum retransmissions per chunk | 8 |
| Integrity | SHA-256 of entire file |
| Destination directory | `received_files/` |
| Temporary filename | `<transfer_id>.part` |
| Collision policy | Never overwrite; append ` (n)` before extension |
| Resume after reconnect | Not supported in version 1; delete partial file on terminal failure |
| Maximum concurrent transfers | 4 per Client; 20 on Server |
| UDP packet sequence | Monotonic per sender session; retransmit same packet with same sequence |

The 1,100-byte chunk data size leaves room for the 14-byte common header and file-chunk metadata while keeping the complete datagram below 1,200 bytes. Do not increase this value without updating the packet calculation and tests.

## 3. Architecture and routing

### 3.1 Control plane versus data plane

**TCP control plane**
- `FILE_OFFER`: sender asks to send a file to a recipient.
- `FILE_DECISION`: receiver accepts or rejects.
- `FILE_CANCEL`: user cancels.
- `FILE_RESULT`: Server reports final status.

**UDP data plane**
- `FILE_START`: sender begins an accepted transfer.
- `FILE_ACCEPT_UDP`: receiver confirms the transfer state is ready.
- `FILE_CHUNK`: sender submits file bytes.
- `FILE_ACK`: receiver acknowledges received chunks.
- `FILE_END`: sender has submitted all chunks.
- `FILE_COMPLETE`: receiver confirms final reassembly and digest result.
- `FILE_ERROR_UDP` / `FILE_CANCEL_UDP`: transfer-specific failure or cancellation.

### 3.2 Server relay

```text
Sender Client                 Server                 Receiver Client
      |                         |                           |
      |--- FILE_OFFER (TCP) --->|                           |
      |                         |--- FILE_OFFER (TCP) ----->|
      |                         |<-- FILE_DECISION (TCP) ---|
      |<-- decision (TCP) ------|                           |
      |                         |                           |
      |--- FILE_START (UDP) --->|--- FILE_START (UDP) ----->|
      |<-- FILE_ACCEPT_UDP -----|<-- FILE_ACCEPT_UDP -------|
      |                         |                           |
      |--- FILE_CHUNK (UDP) --->|--- FILE_CHUNK (UDP) ----->|
      |<------ FILE_ACK --------|<------ FILE_ACK ----------|
      |          ...            |             ...           |
      |--- FILE_END (UDP) ----->|--- FILE_END (UDP) ------->|
      |                         |<-- FILE_COMPLETE ---------|
      |<-- FILE_COMPLETE -------|                           |
      |<-- FILE_RESULT (TCP) ---|--- FILE_RESULT (TCP) ---->|
```

The Server is the only UDP relay endpoint. Clients register their UDP endpoints after login using the session token from `SESSION_INFO`. The Server checks that the packet's `transfer_id`, sender session, recipient session, and active transfer state agree before forwarding it.

### 3.3 Why relay is the selected model

- Fits the required client-server architecture.
- The Server can enforce user authentication and transfer authorization.
- Clients do not need to discover each other's IP address or negotiate direct NAT traversal.
- It is simpler to test on localhost and LAN.
- The Server consumes bandwidth equal to incoming plus outgoing relayed file traffic. This is accepted for the project's 10–20-client target.

Direct peer-to-peer UDP is outside version 1 and must not be added as an undocumented fallback.

## 4. Packet format and message types

All file-transfer packets use the common 14-byte header defined in `protocol-spec.md`.

| Offset | Field | Size |
|---:|---|---:|
| 0 | Magic (`DCAP`) | 4 |
| 4 | Version | 1 |
| 5 | Type | 1 |
| 6 | Sequence | 4 |
| 10 | Payload Length | 4 |
| 14 | Payload | variable |

All multi-byte integers are unsigned big-endian. A UDP datagram contains exactly one complete packet. The maximum complete datagram is 1,200 bytes.

### 4.1 File-transfer type registry

| Value | Name | Transport | Direction |
|---:|---|---|---|
| 12 | `FILE_OFFER` | TCP | Sender → Server → Receiver |
| 13 | `FILE_DECISION` | TCP | Receiver → Server → Sender |
| 14 | `FILE_CANCEL` | TCP | Either Client → Server → other Client |
| 15 | `FILE_RESULT` | TCP | Server → involved Clients |
| 40 | `FILE_START` | UDP | Sender → Server → Receiver |
| 41 | `FILE_ACCEPT_UDP` | UDP | Receiver → Server → Sender |
| 42 | `FILE_CHUNK` | UDP | Sender → Server → Receiver |
| 43 | `FILE_ACK` | UDP | Receiver → Server → Sender |
| 44 | `FILE_END` | UDP | Sender → Server → Receiver |
| 45 | `FILE_COMPLETE` | UDP | Receiver → Server → Sender |
| 46 | `FILE_ERROR_UDP` | UDP | Either participant → Server → other |
| 47 | `FILE_CANCEL_UDP` | UDP | Either participant → Server → other |

The global type registry is authoritative in `protocol-spec.md`. These values MUST match exactly.

## 5. Payload layouts

The common header's Payload Length equals the total number of bytes in the payload layout below.

### 5.1 `FILE_OFFER` — TCP

| Field | Type | Description |
|---|---|---|
| `transfer_id` | `byte[16]` | Random unique transfer ID |
| `recipient` | string | Online recipient username |
| `filename` | string | Base filename only |
| `file_size` | `uint64` | File size in bytes |
| `chunk_size` | `uint16` | MUST be 1100 |
| `total_chunks` | `uint32` | `ceil(file_size / 1100)` |
| `sha256` | `byte[32]` | Digest of original file |

The sender calculates the file size and SHA-256 before offering. The Server verifies sender identity from the authenticated TCP session; it does not trust a sender field in the payload.

### 5.2 `FILE_DECISION` — TCP

| Field | Type | Description |
|---|---|---|
| `transfer_id` | `byte[16]` | Offered transfer |
| `decision` | `uint8` | `0 = reject`, `1 = accept` |
| `reason_code` | `uint16` | 0 if accepted; otherwise error code |
| `reason` | string | Optional human-readable reason |

The Server forwards the decision only to the original sender. A receiver may accept only a transfer that was offered to its authenticated session and is still pending.

### 5.3 `FILE_START` — UDP

| Field | Type | Description |
|---|---|---|
| `transfer_id` | `byte[16]` | Transfer ID |
| `file_size` | `uint64` | Original size |
| `chunk_size` | `uint16` | 1100 |
| `total_chunks` | `uint32` | Expected number of chunks |
| `sha256` | `byte[32]` | Expected digest |

The Server forwards `FILE_START` only if the transfer was accepted over TCP. The Receiver verifies that all metadata matches the earlier `FILE_OFFER`.

### 5.4 `FILE_ACCEPT_UDP` — UDP

| Field | Type | Description |
|---|---|---|
| `transfer_id` | `byte[16]` | Transfer ID |
| `status` | `uint8` | `1 = ready`, `0 = reject` |
| `receive_window` | `uint16` | Maximum number of unacknowledged chunks; 1–32 |
| `reason_code` | `uint16` | 0 on success |
| `reason` | string | Optional reason |

The receiver sends this only after validating metadata, checking capacity and disk space, and creating its temporary output file.

### 5.5 `FILE_CHUNK` — UDP

| Field | Type | Description |
|---|---|---|
| `transfer_id` | `byte[16]` | Transfer ID |
| `chunk_index` | `uint32` | Zero-based chunk index |
| `chunk_data_length` | `uint16` | Number of data bytes |
| `chunk_data` | `byte[chunk_data_length]` | Raw file bytes |

Rules:
- First chunk index is 0.
- Last chunk index is `total_chunks - 1`.
- Every non-final chunk contains exactly 1100 bytes.
- Final chunk contains the remaining bytes and may be shorter.
- For an empty file, `total_chunks = 0`; no `FILE_CHUNK` is sent.
- `chunk_data_length` must match both the actual payload length and the expected size for that chunk.
- A chunk index outside `[0, total_chunks - 1]` is invalid.
- The Server does not convert, decode, or modify chunk data.

Maximum FILE_CHUNK datagram size is 14-byte header + 16-byte transfer ID + 4-byte index + 2-byte length + 1100 data = 1,136 bytes, below the 1,200-byte limit.

### 5.6 `FILE_ACK` — UDP

| Field | Type | Description |
|---|---|---|
| `transfer_id` | `byte[16]` | Transfer ID |
| `ack_base` | `uint32` | Lowest chunk index not yet cumulatively acknowledged |
| `bitmap_length` | `uint16` | Number of bitmap bytes |
| `received_bitmap` | `byte[bitmap_length]` | Selective ACK bitmap |
| `receive_window` | `uint16` | Current advertised receiver capacity |

Bitmap rules:
- Bit 0 represents `ack_base`.
- Bit i represents chunk `ack_base + i`.
- Bit value 1 means the receiver has that chunk.
- Bit value 0 means the chunk is missing/not yet received.
- `ack_base` advances while contiguous chunks from the beginning have been received.
- Bits outside the transfer's chunk range are zero.
- Maximum bitmap length is 4 bytes because the receive window is capped at 32 chunks.

`ack_base` acknowledges every chunk with index less than `ack_base`. The bitmap selectively acknowledges chunks at or above `ack_base`. The sender marks both cumulative and selective acknowledgements as complete.

### 5.7 `FILE_END` — UDP

| Field | Type | Description |
|---|---|---|
| `transfer_id` | `byte[16]` | Transfer ID |
| `total_chunks` | `uint32` | Expected chunk count |
| `file_size` | `uint64` | Expected size |
| `sha256` | `byte[32]` | Expected digest |

`FILE_END` means the sender has submitted all chunks at least once. It does not mean the receiver has all chunks.

### 5.8 `FILE_COMPLETE` — UDP

| Field | Type | Description |
|---|---|---|
| `transfer_id` | `byte[16]` | Transfer ID |
| `status` | `uint8` | `1 = verified`, `0 = failed` |
| `error_code` | `uint16` | 0 on success |
| `file_size` | `uint64` | Final reconstructed size |
| `sha256` | `byte[32]` | Digest calculated by receiver |

Success is sent only after all chunks are present, the file is flushed/closed, file size matches, and SHA-256 matches.

### 5.9 `FILE_ERROR_UDP` and `FILE_CANCEL_UDP`

| Field | Type | Description |
|---|---|---|
| `transfer_id` | `byte[16]` | Transfer ID |
| `error_code` | `uint16` | Stable numeric code |
| `detail` | string | Human-readable explanation |

Numeric codes are defined in `protocol-spec.md`. Program logic MUST use numeric codes, not parse English messages.

## 6. File metadata validation

Before accepting an offer, the receiver and Server validate:

- `file_size` is between 0 and 104,857,600 bytes.
- `chunk_size` equals 1100.
- `total_chunks` equals `ceil(file_size / 1100)`.
- `filename` is valid UTF-8 and 1–255 bytes.
- Filename contains no `/`, `\`, `:`, control characters, `.` or `..` as a path component, or other path traversal sequence.
- Destination path is created by the receiver inside `received_files/`.
- Receiver checks free disk space where possible.
- At most four active transfers exist per Client.
- At most twenty active transfers exist on the Server.
- The Server verifies the sender and recipient are authenticated and online.

Never concatenate an untrusted filename directly into an arbitrary path. Use a canonical receive directory and a collision-safe final name.

## 7. Transfer state machines

### 7.1 Sender

```text
IDLE
  |
  | user selects file and recipient
  v
HASHING
  |
  | FILE_OFFER sent over TCP
  v
WAIT_DECISION
  | rejected / timeout          | accepted
  v                              v
FAILED                         WAIT_UDP_READY
                                  |
                                  | FILE_ACCEPT_UDP(status=ready)
                                  v
                               SENDING
                                  |
                                  | all chunks ACKed
                                  v
                              WAIT_COMPLETE
                               |         |
                         verified       failed/timeout
                               |         |
                               v         v
                           COMPLETED   FAILED
```

### 7.2 Receiver

```text
IDLE
  |
  | FILE_OFFER
  v
WAIT_USER_DECISION
  | reject                  | accept
  v                         v
REJECTED                 PREPARING
                            |
                            | validate + create .part file
                            v
                         RECEIVING
                            |
                            | FILE_END + all chunks present
                            v
                         VERIFYING
                          |       |
                       hash OK   hash mismatch
                          |       |
                          v       v
                      COMPLETED  FAILED
```

Reject chunks for unknown, rejected, cancelled, expired, or completed transfer IDs.

## 8. Reliability algorithm

UDP does not guarantee delivery, order, or uniqueness. The application layer implements selective-repeat reliability.

### 8.1 Sender window

- Start with a congestion/send window of 8 chunks.
- Never exceed the receiver's advertised `receive_window`.
- Maximum window is 32 chunks.
- Send new chunks while the number of in-flight unacknowledged chunks is below the effective window.
- Track each chunk: `UNSENT`, `IN_FLIGHT`, `ACKED`; also store retry count, last-send timestamp, and current RTO.
- A chunk is considered acknowledged by cumulative `ack_base` or a set bitmap bit.
- Do not retransmit acknowledged chunks.
- Do not read the entire file into RAM. Read each chunk from the source file as the send window needs it.

Effective window:

```text
effective_window = min(congestion_window, receiver_window, 32)
```

### 8.2 Receiver behavior

For every valid `FILE_CHUNK`:
1. Find active transfer by `transfer_id`.
2. Validate sender session and transfer state (Server already validates route; Receiver validates transfer).
3. Validate chunk index and expected byte length.
4. If new, seek to `chunk_index * 1100` in the temporary file and write exactly the chunk bytes.
5. Mark the chunk received.
6. If duplicate, do not write it again.
7. Update cumulative `ack_base` and selective bitmap.
8. Send `FILE_ACK` immediately after each group of up to 4 new chunks, and immediately for a duplicate chunk or when a gap is detected.

The receiver uses random-access writes; it must not assume packets arrive in order.

### 8.3 Retransmission timeout

- Initial RTO: 500 ms.
- Minimum RTO: 200 ms.
- Maximum RTO: 4,000 ms.
- When a chunk is sent for the first time, record send time.
- If not acknowledged after RTO, retransmit it and double its RTO up to 4,000 ms.
- Maximum 8 retransmissions for one chunk. If still unacknowledged, fail the transfer with `ERR_TRANSFER_TIMEOUT`.
- When a valid ACK arrives, reset the retry state for newly acknowledged chunks.
- Retransmission uses the same logical packet sequence and chunk index.

For a first implementation, fixed initial RTO with exponential backoff is sufficient. If RTT estimation is added, use measured smoothed RTT and variation; do not reduce RTO below 200 ms.

### 8.4 Flow and congestion control

The receiver advertises its available window in every ACK. If `receive_window = 0`, the sender pauses new chunks but may retransmit timed-out in-flight chunks. The receiver increases its advertised window when its disk writer catches up.

Sender congestion window:
- Start at 8 chunks.
- On successful ACK progress without loss, increase by 1 chunk per acknowledged window, up to 32.
- On timeout, reduce congestion window to `max(1, floor(cwnd / 2))`.
- Do not send more than the effective window.
- This is a bounded AIMD congestion-control policy for the project. Do not label it BBR or CUBIC; those algorithms are not implemented by this policy.

The operating system's TCP congestion control applies only to TCP control/chat traffic. It does not automatically control the application's UDP file data.

### 8.5 ACK loss and duplicate handling

- If an ACK is lost, sender retransmits unacknowledged data after timeout.
- Receiver recognizes the duplicate chunk and sends the current ACK again.
- Duplicate chunks do not increment progress twice.
- Sender progress is based on acknowledged unique file bytes, not total bytes sent including retries.

## 9. File reassembly and integrity

### 9.1 Sender

1. Open source file in binary mode.
2. Determine file size.
3. Calculate SHA-256 over the exact source bytes.
4. Generate random 16-byte `transfer_id`.
5. Calculate `total_chunks = ceil(file_size / 1100)`.
6. Send TCP `FILE_OFFER`.
7. Wait for receiver decision.
8. Send UDP `FILE_START`.
9. Send chunks according to window and ACK state.
10. Send `FILE_END` after all chunks have been submitted.
11. Wait for `FILE_COMPLETE`.
12. Report success only after receiver confirms verified completion.

### 9.2 Receiver

1. Validate metadata and user decision.
2. Create `received_files/<transfer_id>.part`.
3. Pre-size file to expected size where supported.
4. Maintain received-chunk bitmap.
5. Write chunks at `chunk_index * 1100`.
6. On `FILE_END`, check whether all expected chunks exist.
7. If any are missing, send ACK describing missing chunks; continue receiving/retransmission.
8. When complete, flush and close file.
9. Verify exact file size.
10. Calculate SHA-256 over reconstructed bytes.
11. Compare all 32 digest bytes.
12. On match, rename to a safe final filename and send successful `FILE_COMPLETE`.
13. On mismatch, delete `.part`, send `FILE_COMPLETE(status=failed, ERR_INTEGRITY_FAILED)`, and notify Server.

A zero-byte file has zero chunks. The receiver creates an empty temporary file and verifies the standard SHA-256 digest of an empty byte sequence.

### 9.3 SHA-256 is not authentication

SHA-256 detects accidental corruption and mismatched reconstruction. It does not prove who sent the file and does not encrypt the contents. Sender identity and routing authorization come from the authenticated TCP session and Server transfer table.

## 10. Server relay rules

For every UDP file datagram, Server MUST:
1. Validate datagram length and common header.
2. Verify UDP source endpoint is registered to an active session.
3. Verify message type is allowed on UDP.
4. Extract `transfer_id` safely.
5. Find active transfer record.
6. Verify the source session is the permitted sender or receiver for this message type.
7. Verify destination session is still authenticated and UDP-registered.
8. Forward the original datagram to the destination's registered endpoint.
9. Drop unauthorized or malformed datagrams and increment a diagnostic counter.

The Server maintains an active transfer record containing:
- transfer ID,
- sender session ID,
- receiver session ID,
- file size,
- chunk size and total chunks,
- SHA-256 digest,
- transfer state,
- creation and last-activity timestamps.

The Server must not trust a UDP packet's claimed username. Session identity comes from the registered source endpoint and Server-side session map.

## 11. Error handling and cleanup

| Condition | Required behavior |
|---|---|
| Unknown transfer ID | Drop or return `ERR_TRANSFER_NOT_FOUND`; do not crash |
| Invalid sender/receiver session | Drop and record security event |
| Duplicate chunk | Do not rewrite; resend current ACK |
| Out-of-range chunk index | Reject with `ERR_INVALID_CHUNK` |
| Incorrect chunk length | Reject with `ERR_INVALID_CHUNK` |
| Invalid filename | Reject offer with `ERR_INVALID_FILENAME` |
| File exceeds 100 MiB | Reject with `ERR_FILE_TOO_LARGE` |
| Insufficient disk space | Reject before transfer if possible; otherwise abort safely |
| Missing chunks after FILE_END | ACK missing chunks; do not finalize |
| Retry limit exceeded | Fail transfer and delete partial file |
| SHA-256 mismatch | Delete partial file; never publish it |
| Sender disconnects | Fail transfer; delete partial file |
| Receiver disconnects | Stop sender; delete partial file |
| Server disconnects | Transfer fails; delete partial file |
| User cancellation | Stop new sends, notify peer, delete partial file |
| Malformed/oversized datagram | Drop safely; never read out of bounds |
| Filename collision | Generate a new safe name with ` (1)`, ` (2)`, etc. |

Version 1 does not support resume after reconnect. A terminal failure or cancellation deletes the partial `.part` file. A future resumable-transfer extension must define persistent transfer metadata and re-authentication before retaining partial data.

## 12. Security requirements

- Only authenticated online sessions can offer or receive files.
- Server checks sender and recipient identity from TCP sessions.
- UDP endpoint registration token is random, session-scoped, and delivered over TCP only.
- File chunks are accepted only for an active transfer ID and valid direction.
- Validate all lengths before allocation or file writes.
- Use a receiver-controlled directory and sanitized base filename.
- Enforce file-size and concurrent-transfer limits.
- Do not execute received files or trust file extensions.
- Do not claim UDP encryption unless a standard authenticated-encryption layer is implemented and tested.
- If encryption is added, update `protocol-spec.md` with key exchange, nonce construction, authenticated header fields, tag size, and exact overhead; reduce chunk size accordingly.

## 13. C++ module design

Suggested source organization:

```text
protocol/
    packet.h
    packet.cpp              # common header and packet codec

client/
    file_sender.h
    file_sender.cpp         # source file, hashing, send window, retransmission
    file_receiver.h
    file_receiver.cpp       # metadata, bitmap, random-access writes, verification

server/
    file_relay.h
    file_relay.cpp          # session authorization and UDP forwarding

docs/
    test-plan.md
```

Suggested interfaces:

```cpp
class FileSender {
public:
    bool offerFile(const std::string& path,
                   const std::string& recipient);
    void handleDecision(const protocol::Packet& packet);
    void handleAck(const protocol::Packet& packet);
    void handleComplete(const protocol::Packet& packet);
    void processTimeouts();
    void cancel();
};

class FileReceiver {
public:
    void handleOffer(const protocol::Packet& packet);
    void handleStart(const protocol::Packet& packet);
    void handleChunk(const protocol::Packet& packet);
    void handleEnd(const protocol::Packet& packet);
    void cancel();
};

class FileRelay {
public:
    void handleDatagram(const std::uint8_t* data,
                        std::size_t length,
                        const sockaddr_in& source);
};
```

The Protocol module serializes bytes only. `FileSender` and `FileReceiver` own transfer state. `FileRelay` owns routing authorization. UI code receives progress events and does not parse UDP packets.

## 14. Implementation order

### Phase 1 — shared packet codec
1. Implement common header encode/decode.
2. Implement big-endian integer helpers and length-prefixed strings.
3. Implement TCP complete-packet send/receive loops.
4. Implement UDP datagram validation.
5. Implement the corresponding protocol cases in `test-plan.md` (PT).

**Exit condition:** Client and Server produce and parse identical packet bytes.

### Phase 2 — one UDP registration and datagram
1. Start Server UDP listener on 54001.
2. Login Client over TCP.
3. Register UDP endpoint using session token.
4. Send one test heartbeat.
5. Confirm Server recognizes the registered session.

**Exit condition:** valid registration succeeds; invalid token fails.

### Phase 3 — transfer offer and receiver decision
1. Implement TCP `FILE_OFFER`.
2. Display incoming offer in receiver UI.
3. Implement accept/reject.
4. Store active transfer record on Server.
5. Implement UDP `FILE_START` and `FILE_ACCEPT_UDP`.

**Exit condition:** both Clients enter matching transfer states.

### Phase 4 — one chunk and reassembly
1. Send one binary chunk through Server relay.
2. Validate transfer ID and chunk index.
3. Write chunk to `.part` file.
4. Verify bytes against source.
5. Validate empty-file and one-chunk behavior using `test-plan.md` (FT1, FT2, and FT4).

**Exit condition:** one-chunk file arrives byte-identically.

### Phase 5 — multi-chunk transfer
1. Read source in 1100-byte chunks.
2. Send each chunk with zero-based index.
3. Write chunks at calculated offsets.
4. Handle out-of-order and duplicate chunks.
5. Add SHA-256 verification and safe final rename.

**Exit condition:** large file digest matches.

### Phase 6 — reliability
1. Add sender window.
2. Add ACK base and bitmap.
3. Add receiver advertised window.
4. Add retransmission timer and retry limit.
5. Add AIMD window reduction on timeout.
6. Run the network-impairment cases in `test-plan.md` (FT8–FT11).

**Exit condition:** file completes correctly under simulated packet loss.

### Phase 7 — UI, cancellation, and resilience
1. Display offer and accept/reject.
2. Display bytes acknowledged and percentage.
3. Add cancel on either side.
4. Handle sender/receiver/server disconnect.
5. Enforce concurrent transfer and file-size limits.
6. Run the LAN and cross-platform cases in `test-plan.md` (IT17 and OS1–OS3).

**Exit condition:** end-to-end transfer is usable and does not crash on tested failures.

## 15. Build and run guidance

The file-transfer module uses raw UDP sockets through the cross-platform compatibility layer defined in `integration-guide.md` (`platform/socket_compat.h` and `platform/socket_compat.cpp`). The same file-transfer implementation must build and run on Windows, macOS, and Linux. Do not call Winsock-only APIs directly from `file_sender`, `file_receiver`, or `file_relay`.

Use the platform-specific Server and Client build commands in Section 6 of `integration-guide.md`. Those commands include `platform/socket_compat.cpp` and apply the required platform linker settings: Windows links Winsock2 with `-lws2_32`, while macOS and Linux use their system socket libraries.

Platform-specific socket handling belongs only in the compatibility layer:
- **Windows:** the compatibility layer includes `<winsock2.h>` / `<ws2tcpip.h>`, initializes Winsock before socket use, uses `closesocket()` for cleanup, and calls `WSACleanup()` after networking workers stop.
- **macOS/Linux:** the compatibility layer uses the BSD/POSIX socket headers, handles `errno` and `socklen_t`, closes sockets with `close()`, and prevents a closed TCP peer from terminating the process through `SIGPIPE`.
- **All platforms:** bind the Server UDP socket to port `54001`; send and receive UDP datagrams through the portable wrappers; validate the returned datagram length before parsing; and permit TCP `54000` plus UDP `54001` through the host firewall for LAN testing.

If SHA-256 uses OpenSSL, add the installed OpenSSL include/library flags for the selected OS. Do not implement a home-made SHA-256 algorithm.
