# Distributed Chat App

A C++17 client-server chat and collaboration application with a browser-based user interface. The system uses raw TCP and UDP sockets for server communication and a small local HTTP bridge between the browser and the native Client Agent.

## Overview

The application is made up of three parts:

- **Server** — handles client sessions, username authentication, online-user state, chat routing, UDP registration/heartbeat, and relayed file transfers.
- **Client Agent** — maintains the TCP and UDP connections to the Server, handles the DCAP protocol, and exposes a local HTTP interface to the browser.
- **Browser UI** — provides login, online users, global/private chat, and file-transfer interactions.

The browser does not connect directly to the Server's TCP or UDP sockets. It communicates with the Client Agent over HTTP on the same computer.

## Features

- Username-based login and online-session management.
- Global messages and private messages.
- Online-user list and client/server connection status.
- UDP endpoint registration, heartbeat, and discovery.
- File offers and accept/reject decisions over TCP.
- File data relayed through the Server over UDP, with chunking, acknowledgements, retransmission, and SHA-256 integrity verification.
- AES-256-GCM authenticated encryption for DCAP TCP and UDP packet payloads.
- Cross-platform socket compatibility layer for Windows, Linux, and macOS.

## Architecture and network endpoints

```text
+-----------------------+
|      Web Browser      |
|  HTML / CSS / JavaScript
+-----------+-----------+
            |
            | HTTP/1.1, loopback only
            | 127.0.0.1:8080
            v
+-----------------------+
|     Client Agent      |
|  C++ TCP / UDP client |
|  Local HTTP bridge    |
+-----------+-----------+
            |
            | TCP : 54000
            | UDP : 54001
            v
+-----------------------+
|        Server         |
| Sessions / chat / UDP |
| File-transfer relay   |
+-----------------------+
```

| Endpoint | Default address | Purpose |
|---|---|---|
| Server TCP | `<server-ip>:54000` | Login, session control, global/private chat, file negotiation and results |
| Server UDP | `<server-ip>:54001` | UDP registration, heartbeat, discovery, file chunks and acknowledgements |
| Client Agent HTTP | `127.0.0.1:8080` | Browser UI and local Client Agent API |

The Server binds its TCP and UDP sockets to all local IPv4 interfaces. The Client Agent's HTTP bridge binds to loopback (`127.0.0.1`) and is not intended to be exposed to the LAN. The Server's TCP and UDP ports are fixed by the current protocol implementation; the Client Agent's HTTP port can be supplied as a command-line argument.

## Repository layout

```text
.
├── clients/                 # Native Client Agent, HTTP bridge, file sender/receiver
├── docs/
│   ├── integration-guide.md # Architecture, module roles, integration notes
│   ├── protocol-spec.md     # DCAP v1 packet and message specification
│   └── file-transfer-spec.md # UDP file-transfer protocol and reliability
├── platform/                # Windows/POSIX socket compatibility
├── protocol/                # Packet codec, payloads, security, file messages
├── scripts/                 # Test runner scripts (PowerShell on Windows)
├── server/                  # TCP server, sessions, user manager, UDP relay
├── tests/                   # Unit, protocol, security and integration tests
│   └── Expected_Result/     # Expected success-summary output per test program
├── web/                     # Browser UI (HTML, CSS, JavaScript)
├── CMakeLists.txt
├── Makefile
└── README.md
```

## Requirements

- C++17 compiler: GCC, Clang, or a compatible compiler.
- GNU Make for the Makefile workflow. On macOS, Homebrew installs GNU Make as `gmake`.
- OpenSSL development headers and Crypto library.
- `nlohmann/json` headers.
- Thread support.
- CMake 3.20 or later only if using the optional CMake workflow.

Install dependencies using the package manager for the same compiler/toolchain that will build the project.

### Windows — MSYS2 UCRT64

Install the UCRT64 compiler and libraries from an **MSYS2 UCRT64** terminal:

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-nlohmann-json
```

Use the UCRT64 compiler (normally `C:\msys64\ucrt64\bin\g++.exe`). Make sure the `g++` found by PowerShell is from this same UCRT64 environment. The Makefile's Windows test runner uses PowerShell.

### Linux — Ubuntu / Debian

```bash
sudo apt update
sudo apt install build-essential make libssl-dev nlohmann-json3-dev
```

### macOS — Homebrew

```bash
xcode-select --install
brew install make openssl nlohmann-json pkg-config
```

Use `gmake` for the GNU Make commands below. If Homebrew's OpenSSL is not in the compiler's default search paths, expose its flags through `pkg-config` before building:

```bash
export PKG_CONFIG_PATH="$(brew --prefix openssl)/lib/pkgconfig"
export CPPFLAGS="-I. $(pkg-config --cflags openssl)"
export LDLIBS="$(pkg-config --libs openssl)"
```

## Build

Run build commands from the repository root.

### GNU Make (recommended)

On Windows and Linux:

```bash
make
```

On macOS, use `gmake` instead of `make`.

The default target builds the Server, Client Agent, and all 23 test executables. It does **not** run the tests.

| Command | Action |
|---|---|
| `make` | Build Server, Client Agent, and all test executables |
| `make app` | Build Server and Client Agent only |
| `make server` | Build Server only |
| `make clients` | Build Client Agent only |
| `make tests` | Build all test executables without running them |
| `make test` | Build and run all 23 tests sequentially |
| `make dcap_login_tests` | Build one test executable |
| `make run-dcap_login_tests` | Build and run one test executable |
| `make test-one TEST=dcap_login_tests` | Alternative command to run one test |
| `make list-tests` | List available test target names |
| `make clean` | Remove Make-generated binaries and the `make-build/` directory |

Replace `dcap_login_tests` with any test target listed by `make list-tests`.

Build outputs:

- `server_app` / `server_app.exe` and `client_agent` / `client_agent.exe` are created in the repository root.
- Test executables are created under `make-build/tests/`.

### CMake (optional)

```bash
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

CMake build output locations depend on the selected generator and configuration. On multi-configuration generators, executables may be placed under a configuration directory such as `build/Release/`.

## Run the application

Run both programs from the **repository root**. The Server creates or reads `dcap.key` relative to its working directory, and the Client Agent locates the browser assets under `web/`.

### 1. Start the Server

Open a terminal:

**Windows PowerShell**

```powershell
.\server_app.exe
```

**Linux / macOS**

```bash
./server_app
```

The Server listens on TCP port `54000` and UDP port `54001`. On first startup it creates the shared encryption key file `dcap.key` if the file does not already exist.

### 2. Start the Client Agent

Open a second terminal in the repository root:

**Windows PowerShell**

```powershell
.\client_agent.exe 127.0.0.1 54000 8080
```

**Linux / macOS**

```bash
./client_agent 127.0.0.1 54000 8080
```

The arguments are:

```text
client_agent <server-ip> <server-tcp-port> <local-http-port>
```

All arguments are optional. Defaults are:

- Server address: `127.0.0.1`
- Server TCP port: `54000`
- Local HTTP port: `8080`

### 3. Open the browser UI

Open:

```text
http://127.0.0.1:8080/
```

Enter a username to log in. Protocol v1 uses username-based authentication; it does not have password authentication.

Press Enter in the Client Agent terminal to stop the Client Agent. Press Enter in the Server terminal to stop the Server.

### Run on a LAN

1. Start the Server on the host machine.
2. Allow inbound TCP port `54000` and UDP port `54001` through the host firewall.
3. Copy the Server's `dcap.key` securely to each Client machine. Do not send it through the chat application or commit it to the repository.
4. Set `DCAP_KEY_FILE` for the Server and each Client Agent if the key is not in the process working directory.
5. Start each Client Agent with the Server machine's LAN IPv4 address:

```powershell
.\client_agent.exe 192.168.1.10 54000 8080
```

Replace `192.168.1.10` with the actual Server LAN address. Open the browser on the same machine as that Client Agent at `http://127.0.0.1:8080/`. If multiple Client Agents run on one computer, give each one a different local HTTP port (for example, `8080` and `8081`).

The HTTP bridge remains local to each Client Agent machine. It is not the port that remote clients use to connect to the Server.

## Shared encryption key

- The Server generates a random 256-bit AES key and stores it in `dcap.key` on first startup.
- The Server reuses the existing key on later starts.
- Each Client Agent reads the same key file when it connects; it does not create its own key.
- The optional environment variable `DCAP_KEY_FILE` overrides the default path.

PowerShell example:

```powershell
$env:DCAP_KEY_FILE = "C:\secure\dcap.key"
```

Linux / macOS example:

```bash
export DCAP_KEY_FILE="/secure/dcap.key"
```

On POSIX systems, the key file is created with owner-only permissions (`0600`). Keep the key private and do not commit it. If the key is deleted or replaced, all Server and Client Agent instances must use the same newly generated/copied key. The repository's `.gitignore` excludes the runtime key file.

## Public browser gateway (experimental)

The `live-web` branch adds an optional `live_web_gateway` executable. It serves the browser UI over HTTP and creates an independent DCAP TCP/UDP client session for each browser login, so remote users do not need to run `client_agent` locally.

Build it with:

```bash
make live-web-gateway
```

See [Public Web Gateway deployment guide](docs/live-web-deployment.md) for local testing and HTTPS reverse-proxy deployment. The current DCAP protocol still uses username-only authentication; do not expose this prototype to untrusted public users until real user authentication and abuse protections are added.

## Browser HTTP API

The local HTTP bridge is intended for the browser UI running on the same machine. It serves the static UI and exposes these routes:

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/`, `/index.html` | Serve the browser UI |
| `GET` | `/app.js`, `/style.css` | Serve UI assets |
| `POST` | `/login` | Log in with a JSON `username` |
| `POST` | `/logout` | Log out the current session |
| `POST` | `/global_message` | Send a global chat message |
| `POST` | `/private_message` | Send a private message |
| `GET` | `/user_list` | Get the online-user list |
| `GET` | `/events` | Poll queued chat/session/file-transfer events |
| `GET` | `/status` | Get Client Agent connection/authentication status |
| `POST` | `/file_upload` | Upload a file using multipart form data (`recipient` and `file` fields) |
| `POST` | `/file_decision` | Accept or reject an incoming file offer |

JSON request examples:

```json
{ "username": "alice" }
```

```json
{ "content": "Hello everyone" }
```

```json
{ "recipient": "bob", "content": "Hello Bob" }
```

```json
{ "transfer_id": "transfer-id-hex", "accept": true }
```

The UI polls the local bridge for events and status; it does not use WebSockets. The bridge accepts local-origin browser requests and is not designed as a public HTTP service.

## File transfer

1. The sender uploads a file through the browser UI. The Client Agent stages the upload under its working directory's `uploads/` folder.
2. The sender sends a TCP `FILE_OFFER` to the Server.
3. The recipient accepts or rejects the offer through the UI.
4. After acceptance, file data and acknowledgements travel over UDP through the Server relay.
5. The receiver writes the incoming data to a temporary `.part` file, verifies the final size and SHA-256 digest, then renames it to a collision-free filename under `received_files/`.

Current implementation limits include:

- Maximum file size: 100 MiB.
- File chunk data: 1,100 bytes per chunk.
- Maximum concurrent transfers: bounded by Client and Server limits.
- Resume after reconnect: not supported in protocol v1; interrupted partial transfers are removed.

The browser's initial upload progress describes transfer from the browser to its local Client Agent. Delivery progress and final completion are reported separately by the file-transfer events.

## Testing

The repository contains 23 test executables covering packet and message codecs, transport, authentication, chat routing, UDP service, session management, concurrency, file-transfer state machines, server-relay integration, security, and the HTTP bridge.

Run all tests:

```bash
make test
```

Run one test:

```bash
make run-dcap_login_tests
```

See [tests/README.md](tests/README.md) for the complete test list and coverage descriptions. Expected success-summary output files are stored in [tests/Expected_Result/](tests/Expected_Result/). The current Make test runner checks each test process's exit code; it does not automatically compare standard output with those expected-result files.

### Latest recorded verification

On Windows with MSYS2 UCRT64 / MinGW-w64 GCC 14.2.0:

- The full `make` build completed successfully.
- `make test` completed with **23/23 test programs passed**.

This is a reported Windows test run, not a claim that CI has tested every operating system. Linux and macOS builds should be run in their respective environments before treating those platforms as verified.

## Documentation

- [Integration guide](docs/integration-guide.md) — architecture, module responsibilities, build/run integration notes, and team workflow.
- [Protocol specification](docs/protocol-spec.md) — DCAP v1 wire format, message types, and transport rules.
- [File-transfer specification](docs/file-transfer-spec.md) — UDP file-transfer state machine, reliability, integrity, and limits.
- [Test documentation](tests/README.md) — test names, coverage, execution, and known test gaps.

## Security notes and known limitations

- **No password authentication:** protocol v1 authenticates a username only. It is a course-project prototype, not a user-account system.
- **Shared encryption key:** AES-256-GCM protects packet payloads, but all participants use the same shared key. The current design does not provide per-user keys, automated key exchange/rotation, forward secrecy, or replay protection.
- **Local HTTP bridge:** the bridge is bound to loopback by default. Do not change it to a public/LAN-facing listener without adding an appropriate authentication and security design.
- **IPv4:** the current socket and protocol implementation targets IPv4.
- **No file-transfer resume:** an interrupted transfer must be started again.
- **Not production hardened:** review the protocol, key distribution, deployment, logging, and threat model before using this project outside its course/demo environment.

## Team responsibilities

| Contributor | Main responsibility |
|---|---|
| Lê Hoàng Phúc | Server, TCP sessions, authentication, online-user management, message routing, UDP service, and file relay |
| Võ Hoàng Phúc Duy | Client connection lifecycle, local HTTP bridge, and browser UI |
| Nam Anh | Shared protocol, packet/payload codecs, UDP file-transfer logic, reliability, integrity, and security support |

---

**Project context:** Network Application Development course — Distributed and Streaming-Based Client-Server Collaboration Application.
