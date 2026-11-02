# DCAP v1 test suite

The tests in this directory target the current DCAP v1 packet protocol. The
retired length-prefixed JSON protocol tests have been replaced.

## Test programs

| CTest name | Source | Coverage |
|---|---|---|
| \`dcap_protocol_tests\` | \`test_protocol.cpp\` | DCAP header encode/decode, malformed and truncated packets, TCP stream fragmentation/coalescing, message payloads, file metadata/chunks |
| \`dcap_connection_tests\` | \`test_connection.cpp\` | TCP connection, AUTH exchange, disconnect cleanup, reconnect using the same username |
| \`dcap_login_tests\` | \`test_login.cpp\` | Successful authentication, duplicate username rejection, invalid usernames rejected by the client |
| \`dcap_chat_tests\` | \`test_chat.cpp\` | Online user list, global broadcast, private routing, disconnected-user removal |
| \`dcap_file_transfer_tests\` | \`test_file_transfer.cpp\` | FILE_OFFER validation, sender/receiver state machine, UDP chunk handling, ACKs, final SHA-256 verification and byte-for-byte result |
| \`dcap_security_tests\` | \`security_tests.cpp\` | AES-256-GCM round trip, wrong-key rejection, tamper detection, key-file persistence |

## Build and run

From the repository root:

\`\`\`bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build --output-on-failure
\`\`\`

On multi-configuration generators such as Visual Studio, CTest uses the
configuration selected by \`-C\`:

\`\`\`bash
ctest --test-dir build -C Debug --output-on-failure
\`\`\`

The connection, login and chat tests start the real server implementation and
therefore require TCP port 54000 and UDP port 54001 to be available. Those
network tests are marked serial in CTest. They use a temporary test key file;
they do not overwrite the normal development key.

## Scope and limitations

These tests are a focused automated baseline, not proof that every milestone is
complete. In particular, they do not yet simulate packet loss/reordering across
a real network, 10-20 concurrent clients, long idle timeouts, HTTP bridge
requests, or a complete two-client file transfer through the server's UDP
relay. Those require additional end-to-end and network-resilience tests.
