# Distributed-Chat-App

Distributed and streaming-based client-server collaboration application for the Network Application Development course.

## Current integration direction

The integration branch uses the **DCAP v1 binary protocol** defined in `docs/protocol-spec.md` as the single wire contract:

- TCP control/chat: port `54000`
- UDP heartbeat, discovery, and relayed file data: port `54001`
- Browser-to-local-client HTTP bridge: port `8080` (client bridge migration is still pending)

The 14-byte DCAP header, payload codecs, transport rules, message types, and file-transfer behavior are specified in `docs/protocol-spec.md` and `docs/file-transfer-spec.md`.

## Project structure

- `server/`: TCP sessions, authentication, online-user management, UDP service, and file relay.
- `protocol/`: DCAP packet framing, payload codecs, file-transfer messages, and secure-payload helpers.
- `platform/`: Windows/POSIX socket compatibility.
- `clients/file_sender.*`, `clients/file_receiver.*`: UDP file-transfer endpoints.
- `web/`: existing HTML/CSS/JS interface retained from the dev branch.
- `clients/client.*`, `clients/connection.*`, `clients/http_bridge.*`, and the previous JSON protocol sources: legacy dev implementation; these still use the old length-prefixed JSON wire format and are **not yet compatible with the DCAP server**.
- `tests/`: previous JSON-protocol tests retained for reference; they are not registered in the DCAP CMake configuration.

## Build

Requirements: CMake 3.20+, C++17 compiler, OpenSSL Crypto development package, and platform thread support.

```sh
cmake -S . -B build
cmake --build build
```

The CMake targets are:
- `server_app`: DCAP server.
- `client_file_transfer`: reusable file sender/receiver library.

The Makefile from the Phuc branch also provides `make server` and compiles the file-transfer endpoint objects with `make clients`.

## Run

After building, run `server_app`. It listens on TCP 54000 and UDP 54001. Press Enter in its terminal to stop it.

## Team ownership

- Lê Hoàng Phúc: server, TCP sessions, authentication, online users, message routing, UDP service, and file relay.
- Võ Hoàng Phúc Duy: client connection lifecycle and browser UI.
- Protocol/file-transfer work: shared packet format, payload codecs, UDP transfer reliability, integrity, and security support.

## Integration status

This branch is an integration checkpoint, not a claim of a fully working end-to-end application. The server and shared DCAP modules have been brought together with the existing web assets, but the legacy JSON client and HTTP bridge still need to be ported to DCAP. The old JSON tests also need replacement with DCAP tests.

Build and tests have intentionally **not** been run yet, as requested. Do not treat successful compilation or runtime behavior as verified until that step is performed.
