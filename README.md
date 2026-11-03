# Distributed-Chat-App

Distributed and streaming-based client-server collaboration application for the Network Application Development course.

## Protocol and endpoints

The integrated implementation uses the DCAP v1 wire contract in `docs/protocol-spec.md`:

- TCP control, authentication, broadcast/private chat, and file negotiation: port `54000`.
- UDP registration, heartbeat, discovery, and relayed file transfer: port `54001`.
- Browser UI to local Client Agent: HTTP on loopback port `8080`.

Authentication in protocol v1 is username-based. The protocol does not transmit or validate passwords; this is a course prototype, not password-based account security.

## Transport security

All DCAP TCP packets and UDP datagrams use AES-256-GCM authenticated encryption. The 14-byte DCAP header remains visible and is authenticated as GCM additional data; the payload is encrypted. The Server decrypts UDP relay traffic to authorize the transfer and re-encrypts it for the destination client.

On first startup, the Server automatically generates a 256-bit key with OpenSSL and saves it to `dcap.key` in the working directory. Later Server restarts reuse that file. Each Client Agent reads the same file automatically; it does not generate a separate key. The file is excluded by `.gitignore` and must never be committed or exposed to the browser. On POSIX systems it is created with owner-only permissions (`0600`). If a Client runs on another machine, securely copy the Server’s `dcap.key` to that machine or set `DCAP_KEY_FILE` to its secure location. The optional `DCAP_KEY_FILE` environment variable overrides the default file path.

This shared-key setup does not provide per-user keys, password authentication, automatic key exchange/rotation, forward secrecy, or replay protection. It is a course-project security profile, not production-grade security.

## Project structure

- `server/`: TCP sessions, authentication/session management, UDP service, and file relay.
- `protocol/`: DCAP packet framing, payload codecs, file-transfer messages, and secure-payload helpers.
- `platform/`: Windows/POSIX socket compatibility.
- `clients/client.*`: DCAP TCP/UDP client, session registration, chat, and file-transfer coordination.
- `clients/http_bridge.*`: local HTTP bridge for the browser UI.
- `clients/file_sender.*`, `clients/file_receiver.*`: UDP file-transfer state machines.
- `clients/client_agent_main.cpp`: starts the local Client Agent.
- `web/`: HTML/CSS/JS interface retained and connected to the DCAP HTTP bridge.
- `tests/`: DCAP protocol, client/server integration, file-transfer, security, and HTTP/web tests.

## Build requirements

- GNU Make
- C++17 compiler (GCC or a compatible compiler)
- OpenSSL Crypto development package
- Threads
- nlohmann/json headers
- On Windows, GNU Make plus a GCC-compatible toolchain (MSYS2/MinGW-w64 or MinGW-w64 GCC), OpenSSL, and the Windows socket library.
- On macOS, Xcode Command Line Tools (or another Clang/GCC toolchain) and OpenSSL (for example, installed with Homebrew).

### Install the JSON dependency

The client and HTTP bridge use the header-only `nlohmann/json` library. Install its development headers for the same compiler/toolchain used to build this project:

**Windows (MSYS2 UCRT64):** open the MSYS2 UCRT64 terminal and run:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-nlohmann-json
```

The package installs `nlohmann/json.hpp` under the UCRT64 include directory. Use the UCRT64 compiler (`C:/msys64/ucrt64/bin/g++.exe`) to build the project. <https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-nlohmann-json>

**Ubuntu/Debian:**

```sh
sudo apt update
sudo apt install nlohmann-json3-dev
```

**macOS (Homebrew):**

```sh
brew install nlohmann-json
```

CMake 3.20+ is optional; GNU Make can build the application and test programs without CMake.

## Build with Make

Run these commands from the repository root:

\`\`\`sh
make
\`\`\`

The default \`make\` target builds the server, client agent, and all 23 test executables.

Useful targets:

- \`make app\`: build only the server and client.
- \`make server\`: build only \`server_app\`.
- \`make clients\`: build only \`client_agent\`.
- \`make tests\`: build all test executables without running them.
- \`make test\`: build and run all 23 tests sequentially.
- \`make dcap_login_tests\`: build one test executable (replace with any test name listed in \`tests/README.md\`).
- \`make run-dcap_login_tests\`: build and run one test executable.
- \`make clean\`: remove Make-generated binaries and the \`make-build/\` directory.

Build outputs are placed in \`make-build/tests/\`; the server and client executables are created in the repository root.

## Build with CMake (optional)

\`\`\`sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
\`\`\`

Targets:
- \`server_app\`: DCAP server.
- \`client_agent\`: DCAP client plus local HTTP bridge.
- \`client_file_transfer\`: reusable file sender/receiver library.

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

The DCAP server, client agent, HTTP bridge, browser login/chat/user-list flow, and file-offer UI are now connected in source. The unused legacy client, connection, file-transfer, and HTTP bridge implementations have been removed; the active DCAP implementation now lives in the standard `client.*` and `http_bridge.*` files.

**Verification note:** the Make targets describe the build and test commands; a successful build or test run is not implied until those commands have been executed in the target development environment.
