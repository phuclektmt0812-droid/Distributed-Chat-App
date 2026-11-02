# Distributed-Chat-App

Distributed and streaming-based client-server collaboration application for the Network Application Development course.

## Protocol and endpoints

The integrated implementation uses the DCAP v1 wire contract in `docs/protocol-spec.md`:

- TCP control, authentication, broadcast/private chat, and file negotiation: port `54000`.
- UDP registration, heartbeat, discovery, and relayed file transfer: port `54001`.
- Browser UI to local Client Agent: HTTP on loopback port `8080`.

Authentication in protocol v1 is username-based. The protocol does not transmit or validate passwords; this is a course prototype, not password-based account security.

## Project structure

- `server/`: TCP sessions, authentication/session management, UDP service, and file relay.
- `protocol/`: DCAP packet framing, payload codecs, file-transfer messages, and secure-payload helpers.
- `platform/`: Windows/POSIX socket compatibility.
- `clients/dcap_client.*`: DCAP TCP/UDP client, session registration, chat, and file-transfer coordination.
- `clients/dcap_http_bridge.*`: local HTTP bridge for the browser UI.
- `clients/file_sender.*`, `clients/file_receiver.*`: UDP file-transfer state machines.
- `clients/client_agent_main.cpp`: starts the local Client Agent.
- `web/`: HTML/CSS/JS interface retained and connected to the DCAP HTTP bridge.
- `clients/client.*`, `clients/connection.*`, `clients/http_bridge.*`, and `protocol/message.*`: legacy JSON implementation retained for reference; not used by the DCAP targets.
- `tests/`: previous JSON-protocol tests retained for reference. DCAP-specific tests have not yet been added.

## Build requirements

- CMake 3.20+
- C++17 compiler
- OpenSSL Crypto development package
- Threads
- nlohmann/json headers or CMake package

## Build

```sh
cmake -S . -B build
cmake --build build
```

Targets:
- `server_app`: DCAP server.
- `client_agent`: DCAP client plus local HTTP bridge.
- `client_file_transfer`: reusable file sender/receiver library.

## Run

Start the server:

```sh
./build/server_app
```

Start the client agent in another terminal:

```sh
./build/client_agent 127.0.0.1 54000 8080
```

Open `http://127.0.0.1:8080` in a browser; the Client Agent serves the web UI and its API from the same local HTTP listener. For LAN clients, pass the server computer's LAN IPv4 address to `client_agent`.

## File transfer

Choose an online recipient in the UI and upload a file. The bridge stages the file locally, sends a TCP `FILE_OFFER`, and the recipient is prompted to accept or reject. Accepted file data is sent through the server's UDP relay using the DCAP file-transfer state machine. Received files are written under `received_files/`.

## Team ownership

- Lê Hoàng Phúc: server, TCP sessions, authentication, online users, message routing, UDP service, and file relay.
- Võ Hoàng Phúc Duy: client connection lifecycle, local HTTP bridge, and browser UI.
- Protocol/file-transfer work: packet format, payload codecs, UDP reliability, integrity, and security support.

## Integration status

The DCAP server, client agent, HTTP bridge, browser login/chat/user-list flow, and file-offer UI are now connected in source. The old JSON client and bridge remain in the repository but are excluded from the DCAP build targets.

**Build and tests have intentionally not been run yet**, as requested. Compilation, runtime behavior, and end-to-end file delivery are therefore not verified. Dedicated DCAP protocol, client/server integration, and file-transfer tests are still needed before considering the integration complete.
