# DCAP repository test suite

This suite is organized around the source modules in the repository. It contains
unit/codec tests, state-machine tests, socket tests, and integration tests that
start the actual DCAP v1 server and clients.

## Registered CTest programs

| CTest name | Source | Main coverage |
|---|---|---|
| \`dcap_protocol_tests\` | \`test_protocol.cpp\` | Packet encode/decode, TCP stream fragmentation/coalescing, payload smoke tests |
| \`dcap_protocol_edge_tests\` | \`test_protocol_edges.cpp\` | Type ranges, byte reader/writer boundaries, UTF-8, filenames, malformed headers, payload limits, parser reset, concurrent sequence allocation |
| \`dcap_transport_tests\` | \`test_transport.cpp\` | Integer/string helpers, plain and encrypted TCP send/receive, plain and encrypted UDP send/receive, wrong transport type and wrong-key rejection |
| \`dcap_message_codec_tests\` | \`test_message_codecs.cpp\` | AUTH, AUTH_OK/FAIL, session/UDP registration, heartbeat, chat/private chat, user list, ACK, ERROR, notices and invalid values |
| \`dcap_file_message_codec_tests\` | \`test_file_message_codecs.cpp\` | All FILE_* metadata/control codecs, size/chunk calculations, invalid names, sizes, windows, bitmap and status values |
| \`dcap_security_tests\` | \`security_tests.cpp\` | AES-256-GCM round trip, wrong key, tampering, key persistence |
| \`dcap_security_edge_tests\` | \`test_security_edges.cpp\` | Empty payloads, nonce/tag/header tampering, TCP-vs-UDP restrictions, key file creation/format/permissions/environment |
| \`dcap_socket_compat_tests\` | \`test_socket_compat.cpp\` | Loopback TCP, partial/exact I/O helpers, zero-length I/O, close behavior, timeout and keepalive options |
| \`dcap_user_manager_tests\` | \`test_user_manager.cpp\` | Add/remove/find/snapshot/sorted names/revision, UDP token binding, endpoint lookup, heartbeat expiry, concurrent access |
| \`dcap_connection_tests\` | \`test_connection.cpp\` | Real TCP connect/AUTH, disconnect cleanup, reconnect, LOGOUT and server stop/restart |
| \`dcap_handshake_tests\` | \`test_handshake.cpp\` | AUTH-first enforcement and malformed AUTH rejection |
| \`dcap_tcp_control_tests\` | \`test_tcp_control.cpp\` | PING/PONG, malformed MSG/PING, unsupported TCP packet, unknown FILE_CANCEL, LOGOUT ACK and session cleanup |
| \`dcap_login_tests\` | \`test_login.cpp\` | Successful login, duplicate username, invalid local usernames |
| \`dcap_client_validation_tests\` | \`test_client_validation.cpp\` | Client calls before connect/auth, empty/oversized chat, invalid recipient, missing file, unknown offer, repeated disconnect |
| \`dcap_chat_tests\` | \`test_chat.cpp\` | User list, sender-inclusive broadcast, private delivery isolation, offline recipient error, disconnect cleanup |
| \`dcap_udp_tests\` | \`test_udp.cpp\` | UDP registration success/failure, heartbeat ACK, discovery reply, malformed requests and unregistered endpoint rejection |
| \`dcap_concurrency_tests\` | \`test_concurrency.cpp\` | 20 simultaneous TCP clients, broadcast fan-out and disconnect cleanup |
| \`dcap_file_transfer_tests\` | \`test_file_transfer.cpp\` | Existing sender/receiver transfer state-machine happy path and SHA-256 verification |
| \`dcap_file_transfer_edge_tests\` | \`test_file_transfer_edges.cpp\` | Sender validation/rejection/retry/window, receiver out-of-order/duplicate chunks, early END, hash failure, cancellation, empty files, filename collision and partial-file ownership |
| \`dcap_file_transfer_integration_tests\` | \`test_file_transfer_integration.cpp\` | Actual two-client transfer through server UDP relay, final file bytes and completion events |
| \`dcap_file_relay_tests\` | \`test_file_relay.cpp\` | Recipient rejection, timeout expiry, peer disconnect cleanup and per-sender transfer limit |
| \`dcap_http_bridge_tests\` | \`test_http_bridge.cpp\` | HTTP status/login/logout, auth gating, user list, global message routing, web asset serving, malformed JSON/multipart, unknown endpoint and local-origin CORS |
| \`dcap_live_web_gateway_tests\` | \`test_live_web_gateway.cpp\` | Two independent browser cookies map to separate DCAP users; private messages reach both sender and recipient sessions |
| \`dcap_web_asset_tests\` | \`test_web_assets.cpp\` | HTML element/API contract, JS endpoint references and stylesheet presence |

There are 24 registered CTest programs. A program may contain several
assertions/scenarios; the table intentionally describes logical coverage rather
than claiming each assertion is a separate CTest case.

## Build and run with GNU Make (no CMake required)

Run from the repository root:

\`\`\`bash
# Build server, client, and all 24 test executables
make

# Or build only the test executables
make tests

# Build and run all tests sequentially
make test
\`\`\`

To build or run an individual test, use its CTest name as a Make target. For example:

\`\`\`bash
make dcap_login_tests       # build one test
make run-dcap_login_tests   # build and run one test
\`\`\`

Replace \`dcap_login_tests\` with any of the 24 names in the table above. Integration tests that use the fixed TCP/UDP ports are run sequentially by \`make test\` to avoid port conflicts.

Run `make test`, `make run-dcap_login_tests`, or `make test-dcap_login_tests` to overwrite `test-results.log` with the latest run's output. `make clean` removes that log. Clean Make-generated outputs with \`make clean\`.

## Build and run with CMake (optional)

\`\`\`bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
\`\`\`

The integration tests use the real server and fixed TCP port 54000 / UDP port
54001. CTest marks those tests \`RUN_SERIAL\`. They create a temporary key file
through \`DCAP_KEY_FILE\`; they do not intentionally overwrite the developer's
normal key. File-transfer tests use temporary directories. The web asset and
HTTP bridge tests run from the repository root.

## Important scope boundaries

The active DCAP v1 build targets are defined by \`CMakeLists.txt\` and
\`Makefile\`. The suite exercises the current server, \`DcapClient\`,
\`DcapHttpBridge\`, protocol codecs and transport helpers, socket compatibility
layer, file sender, file receiver, and file relay.

The repository still contains older server-side JSON/direct-UDP modules such as
`server/client_session.cpp` and `server/message_router.cpp`, plus their legacy
protocol codecs. They are not part of the current CMake/Makefile build and are
outside the scope of this DCAP v1 test suite. The duplicate legacy client,
connection, file-transfer, and HTTP bridge implementations have been removed.

## Known coverage gaps / manual or future tests

- The HTTP bridge tests exercise its HTTP endpoints, but the web UI is checked
  with a static DOM/API contract; no real browser automation framework is
  configured in this repository.
- The 20-client test checks a practical concurrency target, not a maximum-load
  benchmark or soak test.
- The UDP service test checks registration, heartbeat and discovery. Packet
  loss/reordering at the network layer and long-duration heartbeat expiry need
  a controllable network fault injector or a longer-running test harness.
- File relay tests cover normal completion, rejection, disconnect, expiry and
  per-sender capacity. Exhaustive race/fault injection across every relay state,
  server-wide capacity saturation, and forced UDP send failures remain future
  cases.
- OS-level socket timeout behavior and TCP keepalive timing vary by platform;
  the socket test checks option configuration and loopback I/O, not real
  dead-peer detection over a long network outage.
- The retained legacy server-side classes are not integrated into current
  build targets and therefore cannot be claimed as covered application paths.
- The UDP heartbeat-expiry test includes a clock-rollback boundary. Static
  inspection suggests the current unsigned timestamp subtraction may underflow
  when time moves backwards; this is intentionally represented as a regression
  assertion and may require a production-code fix.
- \`ERR_RATE_LIMITED\` is defined in the protocol constants, but the current
  \`TcpSession\` implementation does not enforce a rate limit; no test can
  validate a behavior that is not implemented.

No test result is implied by adding these sources. Build and CTest must be run
separately before reporting compile/pass status.
